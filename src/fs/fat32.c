#include <stdint.h>
#include <stddef.h>
#include "drivers/ata.h"
#include "core/heap.h"
#include "fs/fat32.h"

/* ------------------------------------------------------------------ */
/* Attribute flags                                                      */
/* ------------------------------------------------------------------ */
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

/* FAT32 end-of-chain marker threshold */
#define FAT32_EOC_MIN  0x0FFFFFF8U
#define FAT32_FREE     0x00000000U
#define FAT32_MASK     0x0FFFFFFFU

/* ------------------------------------------------------------------ */
/* On-disk structures (packed)                                          */
/* ------------------------------------------------------------------ */

struct fat32_bpb {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed));

struct fat32_dirent {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed));

struct fat32_lfn {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Mount-time globals                                                   */
/* ------------------------------------------------------------------ */

static uint32_t fat_lba   = 0;
static uint32_t fat_sectors = 0;
static uint32_t data_lba  = 0;
static uint32_t root_clus = 0;
static uint32_t spc       = 0;   /* sectors per cluster */
static uint32_t bps       = 512; /* bytes per sector */
static uint8_t  num_fats  = 0;

static uint8_t  fat_sector_cache[512];
static uint32_t fat_cache_lba = 0xFFFFFFFF;
static int      fat_cache_dirty = 0;

static uint8_t *clus_buf  = NULL;  /* spc*bps bytes */
static int      mounted   = 0;

/* ------------------------------------------------------------------ */
/* Internal string helpers (no stdlib)                                  */
/* ------------------------------------------------------------------ */

static size_t f32_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void f32_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) p[i] = val;
}

static void f32_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static char f32_toupper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static int f32_isalnum(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/* Case-insensitive compare */
static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = f32_toupper(*a);
        char cb = f32_toupper(*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return f32_toupper(*a) == f32_toupper(*b);
}

/* ------------------------------------------------------------------ */
/* Cluster helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t clus_to_lba(uint32_t c) {
    return data_lba + (c - 2) * spc;
}

static int is_eoc(uint32_t c) {
    return (c & FAT32_MASK) >= FAT32_EOC_MIN;
}

/* Flush FAT cache to disk if dirty */
static void fat_cache_flush(void) {
    if (!fat_cache_dirty) return;
    ata_write(fat_cache_lba, 1, fat_sector_cache);
    if (num_fats >= 2) {
        ata_write(fat_cache_lba + fat_sectors, 1, fat_sector_cache);
    }
    fat_cache_dirty = 0;
}

/* Load FAT sector containing cluster c's entry */
static int fat_load_sector(uint32_t c) {
    uint32_t byte_off = c * 4;
    uint32_t lba = fat_lba + byte_off / bps;
    if (lba != fat_cache_lba) {
        fat_cache_flush();
        if (ata_read(lba, 1, fat_sector_cache) != 0) return -1;
        fat_cache_lba = lba;
        fat_cache_dirty = 0;
    }
    return 0;
}

static uint32_t fat_get(uint32_t c) {
    if (fat_load_sector(c) != 0) return 0x0FFFFFFF;
    uint32_t byte_off = c * 4;
    uint32_t idx = byte_off % bps;
    uint32_t val = (uint32_t)fat_sector_cache[idx]
                 | ((uint32_t)fat_sector_cache[idx+1] << 8)
                 | ((uint32_t)fat_sector_cache[idx+2] << 16)
                 | ((uint32_t)fat_sector_cache[idx+3] << 24);
    return val & FAT32_MASK;
}

static void fat_set(uint32_t c, uint32_t val) {
    if (fat_load_sector(c) != 0) return;
    uint32_t byte_off = c * 4;
    uint32_t idx = byte_off % bps;
    val &= FAT32_MASK;
    /* Preserve upper 4 bits */
    uint32_t old = (uint32_t)fat_sector_cache[idx]
                 | ((uint32_t)fat_sector_cache[idx+1] << 8)
                 | ((uint32_t)fat_sector_cache[idx+2] << 16)
                 | ((uint32_t)fat_sector_cache[idx+3] << 24);
    val |= (old & 0xF0000000U);
    fat_sector_cache[idx]   = (uint8_t)(val & 0xFF);
    fat_sector_cache[idx+1] = (uint8_t)((val >> 8) & 0xFF);
    fat_sector_cache[idx+2] = (uint8_t)((val >> 16) & 0xFF);
    fat_sector_cache[idx+3] = (uint8_t)((val >> 24) & 0xFF);
    fat_cache_dirty = 1;
    fat_cache_flush();
}

/* Allocate a free cluster, link after 'prev' (0 = first in chain).
   Returns new cluster number, or 0 on failure. */
static uint32_t fat_alloc(uint32_t prev) {
    /* Total clusters = (data area sectors) / spc */
    uint32_t total_data_sectors = ata_total_sectors();
    if (total_data_sectors > data_lba)
        total_data_sectors -= data_lba;
    else
        return 0;
    uint32_t total_clusters = total_data_sectors / spc;

    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        uint32_t val = fat_get(c);
        if (val == FAT32_FREE) {
            /* Mark as EOC */
            fat_set(c, 0x0FFFFFFF);
            if (prev != 0) {
                fat_set(prev, c);
            }
            return c;
        }
    }
    return 0; /* disk full */
}

/* Free entire cluster chain starting at c */
static void fat_free_chain(uint32_t c) {
    while (c >= 2 && !is_eoc(c)) {
        uint32_t next = fat_get(c);
        fat_set(c, FAT32_FREE);
        c = next;
    }
    if (c >= 2 && is_eoc(c)) {
        fat_set(c, FAT32_FREE);
    }
}

/* ------------------------------------------------------------------ */
/* 8.3 name helpers                                                     */
/* ------------------------------------------------------------------ */

/* Compute 8.3 checksum over 11-byte name */
static uint8_t dos83_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
    }
    return sum;
}

/* Convert long name → 8.3 short name.
   Fills out[11] uppercase space-padded.  Always uses ~1 suffix. */
static void make_83(const char *longname, char out[11]) {
    f32_memset(out, ' ', 11);

    /* Find last dot */
    int dot_pos = -1;
    int len = (int)f32_strlen(longname);
    for (int i = len - 1; i >= 0; i--) {
        if (longname[i] == '.') { dot_pos = i; break; }
    }

    /* Copy up to 6 alphanumeric chars from base */
    int base_end = (dot_pos >= 0) ? dot_pos : len;
    int bi = 0;
    for (int i = 0; i < base_end && bi < 6; i++) {
        if (f32_isalnum(longname[i])) {
            out[bi++] = f32_toupper(longname[i]);
        }
    }
    /* Append ~1 */
    out[6] = '~';
    out[7] = '1';

    /* Extension */
    if (dot_pos >= 0) {
        int ei = 0;
        for (int i = dot_pos + 1; i < len && ei < 3; i++) {
            if (longname[i] != ' ') {
                out[8 + ei++] = f32_toupper(longname[i]);
            }
        }
    }
}

/* Extract LFN name chars from an LFN entry into out[13].
   Returns number of chars written (stops at null terminator). */
static int lfn_copy(const struct fat32_lfn *lfn, char *out) {
    int n = 0;
    for (int i = 0; i < 5 && n < 13; i++) {
        uint16_t ch = lfn->name1[i];
        if (ch == 0x0000) return n;
        if (ch == 0xFFFF) { out[n++] = '\0'; return n; }
        out[n++] = (char)(ch & 0xFF);
    }
    for (int i = 0; i < 6 && n < 13; i++) {
        uint16_t ch = lfn->name2[i];
        if (ch == 0x0000) return n;
        if (ch == 0xFFFF) { out[n++] = '\0'; return n; }
        out[n++] = (char)(ch & 0xFF);
    }
    for (int i = 0; i < 2 && n < 13; i++) {
        uint16_t ch = lfn->name3[i];
        if (ch == 0x0000) return n;
        if (ch == 0xFFFF) { out[n++] = '\0'; return n; }
        out[n++] = (char)(ch & 0xFF);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Mount                                                                */
/* ------------------------------------------------------------------ */

int fat32_mount(void) {
    if (ata_init() != 0) return -1;

    uint8_t sector0[512];
    if (ata_read(0, 1, sector0) != 0) return -1;

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector0;

    /* Validate */
    if (bpb->bytes_per_sector == 0) return -1;
    if (bpb->sectors_per_cluster == 0) return -1;
    if (bpb->fat_size_32 == 0) return -1;

    bps       = bpb->bytes_per_sector;
    spc       = bpb->sectors_per_cluster;
    num_fats  = bpb->num_fats;
    fat_lba   = bpb->reserved_sectors;
    fat_sectors = bpb->fat_size_32;
    data_lba  = fat_lba + (uint32_t)num_fats * fat_sectors;
    root_clus = bpb->root_cluster;

    /* Allocate cluster buffer */
    if (clus_buf) kfree(clus_buf);
    clus_buf = (uint8_t *)kmalloc(spc * bps);
    if (!clus_buf) return -1;

    fat_cache_lba = 0xFFFFFFFF;
    fat_cache_dirty = 0;
    mounted = 1;
    return 0;
}

uint32_t fat32_root_cluster(void) {
    return root_clus;
}

/* ------------------------------------------------------------------ */
/* Read a cluster chain into clus_buf                                  */
/* ------------------------------------------------------------------ */

static int read_cluster(uint32_t c) {
    uint32_t lba = clus_to_lba(c);
    f32_memset(clus_buf, 0, spc * bps);
    return ata_read(lba, (uint8_t)spc, clus_buf);
}

static int write_cluster(uint32_t c) {
    uint32_t lba = clus_to_lba(c);
    return ata_write(lba, (uint8_t)spc, clus_buf);
}

/* ------------------------------------------------------------------ */
/* Build 8.3 printable name from dirent (e.g. "FOO     BAR" → "foo.bar") */
/* ------------------------------------------------------------------ */

static void dirent_to_name(const struct fat32_dirent *de, char *out, int max) {
    int pos = 0;

    /* Base name (strip trailing spaces) */
    int base_end = 8;
    while (base_end > 0 && de->name[base_end-1] == ' ') base_end--;

    for (int i = 0; i < base_end && pos < max - 1; i++) {
        char c = de->name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32); /* to lower */
        out[pos++] = c;
    }

    /* Extension */
    int ext_end = 3;
    while (ext_end > 0 && de->ext[ext_end-1] == ' ') ext_end--;
    if (ext_end > 0 && pos < max - 1) {
        out[pos++] = '.';
        for (int i = 0; i < ext_end && pos < max - 1; i++) {
            char c = de->ext[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
}

/* ------------------------------------------------------------------ */
/* Directory iteration helpers                                          */
/* ------------------------------------------------------------------ */

/* LFN accumulation buffer: up to 20 LFN entries × 13 chars = 260 chars */
#define LFN_MAX_ENTRIES 20
#define LFN_BUF_SIZE    (LFN_MAX_ENTRIES * 13 + 1)

/* Walk all 32-byte entries in dir_cluster chain.
   For each 8.3 entry, call fn(de, fullname, ud).
   fullname is built from LFN entries if available, else 8.3 name.
   fn returns:
     0  = continue
     1  = stop (found)
    -1  = error
*/
typedef int (*dir_entry_cb)(const struct fat32_dirent *de,
                            const char *fullname,
                            uint32_t entry_lba,
                            uint32_t entry_offset_in_sector,
                            uint32_t dir_cluster_of_entry,
                            int      entry_idx_in_cluster,
                            void    *ud);

struct dir_iter_state {
    uint32_t cur_clus;
    uint32_t entry_idx; /* 32-byte entry index within cluster */
    uint32_t entries_per_cluster;
};

/* Context for locating a specific entry to patch */
struct find_ctx {
    const char *target;
    uint32_t out_cluster;
    uint32_t out_size;
    int      out_is_dir;
    int      found;

    /* for modification: entry location */
    uint32_t entry_clus;
    int      entry_idx;
    /* LFN entry count before the 8.3 entry */
    int      lfn_count;
    /* LFN start entry index in cluster */
    int      lfn_start_idx;
    uint32_t lfn_start_clus;
};

static int dir_iter(uint32_t dir_cluster, dir_entry_cb fn, void *ud) {
    if (!mounted || !clus_buf) return -1;

    char lfn_buf[LFN_BUF_SIZE];
    int  lfn_pending = 0;    /* number of LFN entries collected */
    int  lfn_expect  = -1;   /* expected next sequence number */
    uint8_t lfn_checksum = 0;

    uint32_t entries_per_cluster = (spc * bps) / 32;
    uint32_t cur = dir_cluster;

    while (cur >= 2 && !is_eoc(cur)) {
        if (read_cluster(cur) != 0) return -1;

        for (uint32_t ei = 0; ei < entries_per_cluster; ei++) {
            struct fat32_dirent *de = (struct fat32_dirent *)(clus_buf + ei * 32);
            uint8_t first = (uint8_t)de->name[0];

            /* End of directory */
            if (first == 0x00) return 0;

            /* Deleted entry */
            if (first == 0xE5) {
                lfn_pending = 0;
                lfn_expect = -1;
                continue;
            }

            /* LFN entry */
            if (de->attr == ATTR_LFN) {
                struct fat32_lfn *lfn = (struct fat32_lfn *)(void *)de;
                int seq = lfn->order & 0x3F;
                if (lfn->order & 0x40) {
                    /* Last LFN entry (first in sequence on disk) */
                    lfn_pending = 0;
                    lfn_expect = seq;
                    lfn_checksum = lfn->checksum;
                    f32_memset(lfn_buf, 0, LFN_BUF_SIZE);
                } else if (seq != lfn_expect || lfn->checksum != lfn_checksum) {
                    lfn_pending = 0;
                    lfn_expect = -1;
                    continue;
                }
                /* Place chars at right offset: (seq-1)*13 */
                if (seq >= 1 && seq <= LFN_MAX_ENTRIES) {
                    char tmp[13];
                    int nc = lfn_copy(lfn, tmp);
                    int off = (seq - 1) * 13;
                    for (int k = 0; k < nc && off + k < LFN_BUF_SIZE - 1; k++)
                        lfn_buf[off + k] = tmp[k];
                    lfn_pending++;
                    lfn_expect = seq - 1;
                }
                continue;
            }

            /* Skip volume labels */
            if (de->attr & ATTR_VOLUME_ID) {
                lfn_pending = 0;
                lfn_expect = -1;
                continue;
            }

            /* Normal 8.3 entry */
            char fullname[LFN_BUF_SIZE];

            if (lfn_pending > 0 && lfn_buf[0] != '\0') {
                /* Use LFN name */
                f32_memcpy(fullname, lfn_buf, LFN_BUF_SIZE);
            } else {
                /* Build from 8.3 */
                dirent_to_name(de, fullname, (int)LFN_BUF_SIZE);
            }

            int r = fn(de, fullname, 0, 0, cur, (int)ei, ud);
            if (r == 1) return 0;
            if (r < 0)  return -1;

            lfn_pending = 0;
            lfn_expect = -1;
        }

        cur = fat_get(cur);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* fat32_find                                                           */
/* ------------------------------------------------------------------ */

struct find_ud {
    const char *target;
    uint32_t   out_cluster;
    uint32_t   out_size;
    int        out_is_dir;
    int        found;
    uint32_t   entry_clus;
    int        entry_idx;
};

static int find_cb(const struct fat32_dirent *de, const char *fullname,
                   uint32_t elba, uint32_t eoff,
                   uint32_t eclus, int eidx, void *ud) {
    (void)elba; (void)eoff;
    struct find_ud *f = (struct find_ud *)ud;
    if (ci_eq(fullname, f->target)) {
        f->out_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
        f->out_size    = de->file_size;
        f->out_is_dir  = (de->attr & ATTR_DIRECTORY) ? 1 : 0;
        f->found       = 1;
        f->entry_clus  = eclus;
        f->entry_idx   = eidx;
        return 1; /* stop */
    }
    return 0;
}

int fat32_find(uint32_t dir_cluster, const char *name,
               uint32_t *out_cluster, uint32_t *out_size, int *out_is_dir) {
    if (!mounted) return -1;
    struct find_ud ud;
    ud.target   = name;
    ud.found    = 0;
    ud.out_cluster = 0;
    ud.out_size    = 0;
    ud.out_is_dir  = 0;
    ud.entry_clus  = 0;
    ud.entry_idx   = 0;

    dir_iter(dir_cluster, find_cb, &ud);

    if (!ud.found) return -1;
    if (out_cluster) *out_cluster = ud.out_cluster;
    if (out_size)    *out_size    = ud.out_size;
    if (out_is_dir)  *out_is_dir  = ud.out_is_dir;
    return 0;
}

/* ------------------------------------------------------------------ */
/* fat32_list                                                           */
/* ------------------------------------------------------------------ */

struct list_ud {
    void (*cb)(const char *name, int is_dir, uint32_t size, void *ud2);
    void *ud2;
};

static int list_cb(const struct fat32_dirent *de, const char *fullname,
                   uint32_t elba, uint32_t eoff,
                   uint32_t eclus, int eidx, void *ud) {
    (void)elba; (void)eoff; (void)eclus; (void)eidx;
    struct list_ud *l = (struct list_ud *)ud;

    /* Skip "." and ".." */
    if (fullname[0] == '.' &&
        (fullname[1] == '\0' || (fullname[1] == '.' && fullname[2] == '\0')))
        return 0;

    int is_dir = (de->attr & ATTR_DIRECTORY) ? 1 : 0;
    l->cb(fullname, is_dir, de->file_size, l->ud2);
    return 0;
}

int fat32_list(uint32_t dir_cluster,
               void (*cb)(const char *name, int is_dir,
                          uint32_t size, void *ud),
               void *ud) {
    if (!mounted) return -1;
    struct list_ud l;
    l.cb  = cb;
    l.ud2 = ud;
    return dir_iter(dir_cluster, list_cb, &l);
}

/* ------------------------------------------------------------------ */
/* fat32_read                                                           */
/* ------------------------------------------------------------------ */

int fat32_read(uint32_t start_cluster, uint32_t file_size,
               void *buf, uint32_t maxlen) {
    if (!mounted) return -1;
    if (start_cluster < 2) return -1;

    uint32_t clus_bytes = spc * bps;
    uint8_t *out = (uint8_t *)buf;
    uint32_t remaining = (file_size < maxlen) ? file_size : maxlen;
    uint32_t total_read = 0;
    uint32_t cur = start_cluster;

    while (cur >= 2 && !is_eoc(cur) && remaining > 0) {
        if (read_cluster(cur) != 0) return -1;

        uint32_t to_copy = (remaining < clus_bytes) ? remaining : clus_bytes;
        f32_memcpy(out, clus_buf, to_copy);
        out        += to_copy;
        total_read += to_copy;
        remaining  -= to_copy;
        cur = fat_get(cur);
    }

    return (int)total_read;
}

/* ------------------------------------------------------------------ */
/* Directory entry writing                                              */
/* ------------------------------------------------------------------ */

/* Directly scan for free slots (dir_iter doesn't expose deleted entries) */
static int find_free_slots(uint32_t dir_cluster, int n_need,
                            uint32_t *out_clus, int *out_idx) {
    uint32_t entries_per_cluster = (spc * bps) / 32;
    uint32_t cur = dir_cluster;
    int consecutive = 0;
    uint32_t start_clus = 0;
    int      start_idx  = 0;

    while (cur >= 2 && !is_eoc(cur)) {
        if (read_cluster(cur) != 0) return -1;

        for (uint32_t ei = 0; ei < entries_per_cluster; ei++) {
            uint8_t first = clus_buf[ei * 32];
            if (first == 0x00 || first == 0xE5) {
                if (consecutive == 0) {
                    start_clus = cur;
                    start_idx  = (int)ei;
                }
                consecutive++;
                if (consecutive >= n_need) {
                    *out_clus = start_clus;
                    *out_idx  = start_idx;
                    return 0;
                }
            } else {
                consecutive = 0;
            }
        }
        cur = fat_get(cur);
    }

    /* Try extending directory with a new cluster */
    uint32_t prev = dir_cluster;
    uint32_t c = fat_get(prev);
    while (c >= 2 && !is_eoc(c)) { prev = c; c = fat_get(c); }

    uint32_t new_clus = fat_alloc(prev);
    if (new_clus == 0) return -1;

    /* Zero the new cluster */
    f32_memset(clus_buf, 0, spc * bps);
    if (write_cluster(new_clus) != 0) return -1;

    *out_clus = new_clus;
    *out_idx  = 0;
    return 0;
}

/* Write one 32-byte entry at (cluster, idx).
   If idx >= entries_per_cluster, it wraps to next cluster (must be allocated). */
static int write_dirent_at(uint32_t clus, int idx, const void *entry32) {
    uint32_t entries_per_cluster = (spc * bps) / 32;
    /* Navigate to the right cluster */
    while ((uint32_t)idx >= entries_per_cluster) {
        idx -= (int)entries_per_cluster;
        uint32_t next = fat_get(clus);
        if (next < 2 || is_eoc(next)) return -1;
        clus = next;
    }
    if (read_cluster(clus) != 0) return -1;
    f32_memcpy(clus_buf + (uint32_t)idx * 32, entry32, 32);
    return write_cluster(clus);
}

/* Write a directory entry with LFN entries */
static int dir_write_entry(uint32_t dir_cluster, const char *name,
                            uint8_t attr, uint32_t first_cluster,
                            uint32_t file_size) {
    /* Compute 8.3 name */
    char short83[11];
    make_83(name, short83);

    /* Compute LFN count */
    int namelen = (int)f32_strlen(name);
    int n_lfn   = (namelen + 12) / 13;
    int n_total = n_lfn + 1; /* LFN entries + 8.3 entry */

    /* Find free slots */
    uint32_t slot_clus;
    int      slot_idx;
    if (find_free_slots(dir_cluster, n_total, &slot_clus, &slot_idx) != 0)
        return -1;

    /* Checksum */
    uint8_t cksum = dos83_checksum((const uint8_t *)short83);

    uint32_t entries_per_cluster = (spc * bps) / 32;
    int cur_idx = slot_idx;
    uint32_t cur_clus = slot_clus;

    /* Helper lambda to advance cluster/idx */
    /* (Inline since we have no closures in C) */

    /* Write LFN entries in reverse order (highest seq first) */
    for (int seq = n_lfn; seq >= 1; seq--) {
        struct fat32_lfn lfn_entry;
        f32_memset(&lfn_entry, 0xFF, 32);
        lfn_entry.attr     = ATTR_LFN;
        lfn_entry.type     = 0;
        lfn_entry.checksum = cksum;
        lfn_entry.zero     = 0;
        lfn_entry.order    = (uint8_t)seq;
        if (seq == n_lfn) lfn_entry.order |= 0x40; /* last LFN entry marker */

        /* Fill name chars for this entry */
        int base = (seq - 1) * 13;
        for (int k = 0; k < 5; k++) {
            int ci = base + k;
            if (ci < namelen) lfn_entry.name1[k] = (uint16_t)(uint8_t)name[ci];
            else if (ci == namelen) lfn_entry.name1[k] = 0x0000;
            /* else stays 0xFFFF */
        }
        for (int k = 0; k < 6; k++) {
            int ci = base + 5 + k;
            if (ci < namelen) lfn_entry.name2[k] = (uint16_t)(uint8_t)name[ci];
            else if (ci == namelen) lfn_entry.name2[k] = 0x0000;
        }
        for (int k = 0; k < 2; k++) {
            int ci = base + 11 + k;
            if (ci < namelen) lfn_entry.name3[k] = (uint16_t)(uint8_t)name[ci];
            else if (ci == namelen) lfn_entry.name3[k] = 0x0000;
        }

        /* Write at current position */
        if (write_dirent_at(cur_clus, cur_idx, &lfn_entry) != 0) return -1;

        cur_idx++;
        if ((uint32_t)cur_idx >= entries_per_cluster) {
            uint32_t next = fat_get(cur_clus);
            if (next < 2 || is_eoc(next)) return -1;
            cur_clus = next;
            cur_idx = 0;
        }
    }

    /* Write 8.3 entry */
    struct fat32_dirent de;
    f32_memset(&de, 0, sizeof(de));
    f32_memcpy(de.name, short83, 8);
    f32_memcpy(de.ext, short83 + 8, 3);
    de.attr        = attr;
    de.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    de.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    de.file_size   = file_size;
    /* Date: 1980-01-01 */
    de.wrt_date    = (1 << 5) | 1;
    de.crt_date    = de.wrt_date;

    if (write_dirent_at(cur_clus, cur_idx, &de) != 0) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Update an existing dir entry (cluster + size) given its location    */
/* ------------------------------------------------------------------ */

static int dir_update_entry(uint32_t entry_clus, int entry_idx,
                             uint32_t new_cluster, uint32_t new_size) {
    if (read_cluster(entry_clus) != 0) return -1;

    struct fat32_dirent *de = (struct fat32_dirent *)(clus_buf + (uint32_t)entry_idx * 32);
    de->fst_clus_hi = (uint16_t)(new_cluster >> 16);
    de->fst_clus_lo = (uint16_t)(new_cluster & 0xFFFF);
    de->file_size   = new_size;

    return write_cluster(entry_clus);
}

/* ------------------------------------------------------------------ */
/* fat32_write                                                          */
/* ------------------------------------------------------------------ */

int fat32_write(uint32_t dir_cluster, const char *name,
                const void *data, uint32_t len) {
    if (!mounted) return -1;

    /* Check if file exists */
    struct find_ud fud;
    fud.target    = name;
    fud.found     = 0;
    fud.out_cluster = 0;
    fud.out_size    = 0;
    fud.out_is_dir  = 0;
    fud.entry_clus  = 0;
    fud.entry_idx   = 0;
    dir_iter(dir_cluster, find_cb, &fud);

    if (fud.found) {
        /* Free existing cluster chain */
        if (fud.out_cluster >= 2) fat_free_chain(fud.out_cluster);
        /* Reset cluster pointer and size in dir entry */
        if (dir_update_entry(fud.entry_clus, fud.entry_idx, 0, 0) != 0)
            return -1;
    }

    /* Allocate clusters and write data */
    uint32_t clus_bytes = spc * bps;
    uint32_t first_clus = 0;
    uint32_t prev_clus  = 0;
    uint32_t written    = 0;

    if (len == 0) {
        /* Empty file — just update dir entry */
        if (fud.found) {
            return dir_update_entry(fud.entry_clus, fud.entry_idx, 0, 0);
        } else {
            return dir_write_entry(dir_cluster, name, ATTR_ARCHIVE, 0, 0);
        }
    }

    const uint8_t *src = (const uint8_t *)data;

    while (written < len) {
        uint32_t c = fat_alloc(prev_clus);
        if (c == 0) return -1;
        if (first_clus == 0) first_clus = c;
        prev_clus = c;

        uint32_t chunk = len - written;
        if (chunk > clus_bytes) chunk = clus_bytes;

        f32_memset(clus_buf, 0, clus_bytes);
        f32_memcpy(clus_buf, src + written, chunk);
        if (write_cluster(c) != 0) return -1;

        written += chunk;
    }

    if (fud.found) {
        return dir_update_entry(fud.entry_clus, fud.entry_idx,
                                 first_clus, len);
    } else {
        return dir_write_entry(dir_cluster, name, ATTR_ARCHIVE,
                               first_clus, len);
    }
}

/* ------------------------------------------------------------------ */
/* fat32_append                                                         */
/* ------------------------------------------------------------------ */

int fat32_append(uint32_t dir_cluster, const char *name,
                 const void *data, uint32_t len) {
    if (!mounted) return -1;

    struct find_ud fud;
    fud.target    = name;
    fud.found     = 0;
    fud.out_cluster = 0;
    fud.out_size    = 0;
    fud.out_is_dir  = 0;
    fud.entry_clus  = 0;
    fud.entry_idx   = 0;
    dir_iter(dir_cluster, find_cb, &fud);

    if (!fud.found) {
        /* File doesn't exist — create it */
        return fat32_write(dir_cluster, name, data, len);
    }

    /* Read existing content into a temp buf */
    uint32_t old_size = fud.out_size;
    uint32_t new_size = old_size + len;
    uint8_t *tmp = (uint8_t *)kmalloc(new_size);
    if (!tmp) return -1;

    if (old_size > 0 && fud.out_cluster >= 2) {
        int r = fat32_read(fud.out_cluster, old_size, tmp, old_size);
        if (r < 0) { kfree(tmp); return -1; }
    }
    f32_memcpy(tmp + old_size, data, len);

    /* Free old chain */
    if (fud.out_cluster >= 2) fat_free_chain(fud.out_cluster);

    /* Allocate new clusters */
    uint32_t clus_bytes = spc * bps;
    uint32_t first_clus = 0;
    uint32_t prev_clus  = 0;
    uint32_t written    = 0;

    while (written < new_size) {
        uint32_t c = fat_alloc(prev_clus);
        if (c == 0) { kfree(tmp); return -1; }
        if (first_clus == 0) first_clus = c;
        prev_clus = c;

        uint32_t chunk = new_size - written;
        if (chunk > clus_bytes) chunk = clus_bytes;

        f32_memset(clus_buf, 0, clus_bytes);
        f32_memcpy(clus_buf, tmp + written, chunk);
        if (write_cluster(c) != 0) { kfree(tmp); return -1; }
        written += chunk;
    }

    kfree(tmp);

    return dir_update_entry(fud.entry_clus, fud.entry_idx,
                             first_clus, new_size);
}

/* ------------------------------------------------------------------ */
/* fat32_create                                                         */
/* ------------------------------------------------------------------ */

int fat32_create(uint32_t dir_cluster, const char *name,
                 int is_dir, uint32_t parent_cluster) {
    if (!mounted) return -1;

    /* Check if already exists */
    uint32_t dummy_c, dummy_s;
    int dummy_d;
    if (fat32_find(dir_cluster, name, &dummy_c, &dummy_s, &dummy_d) == 0)
        return -1; /* already exists */

    if (!is_dir) {
        return dir_write_entry(dir_cluster, name, ATTR_ARCHIVE, 0, 0);
    }

    /* Allocate one cluster for the new directory */
    uint32_t new_clus = fat_alloc(0);
    if (new_clus == 0) return -1;

    /* Write . and .. entries */
    f32_memset(clus_buf, 0, spc * bps);

    struct fat32_dirent dot;
    f32_memset(&dot, 0, sizeof(dot));
    f32_memset(dot.name, ' ', 8);
    f32_memset(dot.ext,  ' ', 3);
    dot.name[0]     = '.';
    dot.attr        = ATTR_DIRECTORY;
    dot.fst_clus_hi = (uint16_t)(new_clus >> 16);
    dot.fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
    dot.wrt_date    = (1 << 5) | 1;
    dot.crt_date    = dot.wrt_date;

    struct fat32_dirent dotdot;
    f32_memset(&dotdot, 0, sizeof(dotdot));
    f32_memset(dotdot.name, ' ', 8);
    f32_memset(dotdot.ext,  ' ', 3);
    dotdot.name[0]  = '.';
    dotdot.name[1]  = '.';
    dotdot.attr     = ATTR_DIRECTORY;
    /* If parent is root, parent cluster = 0 */
    uint32_t pclus = (parent_cluster == root_clus) ? 0 : parent_cluster;
    dotdot.fst_clus_hi = (uint16_t)(pclus >> 16);
    dotdot.fst_clus_lo = (uint16_t)(pclus & 0xFFFF);
    dotdot.wrt_date    = (1 << 5) | 1;
    dotdot.crt_date    = dotdot.wrt_date;

    f32_memcpy(clus_buf,       &dot,    32);
    f32_memcpy(clus_buf + 32,  &dotdot, 32);

    if (write_cluster(new_clus) != 0) {
        fat_set(new_clus, FAT32_FREE);
        return -1;
    }

    return dir_write_entry(dir_cluster, name, ATTR_DIRECTORY, new_clus, 0);
}

/* ------------------------------------------------------------------ */
/* fat32_delete                                                         */
/* ------------------------------------------------------------------ */

/* We need a custom iteration to collect LFN + 8.3 positions */
static int delete_iter(uint32_t dir_cluster, const char *target,
                       uint32_t *out_file_cluster, int *out_is_dir,
                       uint32_t *del_clus, int *del_idx, int *del_count) {
    if (!mounted || !clus_buf) return -1;

    char lfn_buf[LFN_BUF_SIZE];
    int  lfn_pending = 0;

    uint32_t lfn_clus_arr[LFN_MAX_ENTRIES];
    int      lfn_idx_arr[LFN_MAX_ENTRIES];

    uint32_t entries_per_cluster = (spc * bps) / 32;
    uint32_t cur = dir_cluster;

    while (cur >= 2 && !is_eoc(cur)) {
        if (read_cluster(cur) != 0) return -1;

        for (uint32_t ei = 0; ei < entries_per_cluster; ei++) {
            struct fat32_dirent *de = (struct fat32_dirent *)(clus_buf + ei * 32);
            uint8_t first = (uint8_t)de->name[0];

            if (first == 0x00) return -1; /* not found */
            if (first == 0xE5) { lfn_pending = 0; continue; }

            if (de->attr == ATTR_LFN) {
                struct fat32_lfn *lfn = (struct fat32_lfn *)(void *)de;
                int seq = lfn->order & 0x3F;
                if (lfn->order & 0x40) {
                    lfn_pending = 0;
                    f32_memset(lfn_buf, 0, LFN_BUF_SIZE);
                }
                if (seq >= 1 && seq <= LFN_MAX_ENTRIES) {
                    char tmp[13];
                    int nc = lfn_copy(lfn, tmp);
                    int off = (seq - 1) * 13;
                    for (int k = 0; k < nc && off + k < LFN_BUF_SIZE - 1; k++)
                        lfn_buf[off + k] = tmp[k];
                    int pos = lfn_pending;
                    if (pos < LFN_MAX_ENTRIES) {
                        lfn_clus_arr[pos] = cur;
                        lfn_idx_arr[pos]  = (int)ei;
                    }
                    lfn_pending++;
                }
                continue;
            }

            if (de->attr & ATTR_VOLUME_ID) { lfn_pending = 0; continue; }

            /* 8.3 entry */
            char fullname[LFN_BUF_SIZE];
            if (lfn_pending > 0 && lfn_buf[0] != '\0') {
                f32_memcpy(fullname, lfn_buf, LFN_BUF_SIZE);
            } else {
                dirent_to_name(de, fullname, (int)LFN_BUF_SIZE);
            }

            if (ci_eq(fullname, target)) {
                *out_file_cluster = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
                *out_is_dir       = (de->attr & ATTR_DIRECTORY) ? 1 : 0;

                /* Collect LFN positions */
                int cnt = 0;
                for (int k = 0; k < lfn_pending && k < LFN_MAX_ENTRIES; k++) {
                    del_clus[cnt] = lfn_clus_arr[k];
                    del_idx[cnt]  = lfn_idx_arr[k];
                    cnt++;
                }
                /* 8.3 entry */
                del_clus[cnt] = cur;
                del_idx[cnt]  = (int)ei;
                cnt++;
                *del_count = cnt;
                return 0;
            }

            lfn_pending = 0;
        }

        cur = fat_get(cur);
    }
    return -1;
}

/* Check if directory is empty (only . and ..) */
static int dir_is_empty(uint32_t cluster) {
    uint32_t entries_per_cluster = (spc * bps) / 32;
    uint32_t cur = cluster;

    while (cur >= 2 && !is_eoc(cur)) {
        if (read_cluster(cur) != 0) return 0;
        for (uint32_t ei = 0; ei < entries_per_cluster; ei++) {
            struct fat32_dirent *de = (struct fat32_dirent *)(clus_buf + ei * 32);
            uint8_t first = (uint8_t)de->name[0];
            if (first == 0x00) return 1;
            if (first == 0xE5) continue;
            if (de->attr == ATTR_LFN) continue;
            if (de->attr & ATTR_VOLUME_ID) continue;
            /* Check for . and .. */
            if (de->name[0] == '.') {
                if (de->name[1] == ' ' || de->name[1] == '.') continue;
            }
            return 0; /* has real entries */
        }
        cur = fat_get(cur);
    }
    return 1;
}

int fat32_delete(uint32_t dir_cluster, const char *name) {
    if (!mounted) return -1;

    uint32_t del_clus[LFN_MAX_ENTRIES + 1];
    int      del_idx[LFN_MAX_ENTRIES + 1];
    int      del_count = 0;
    uint32_t file_cluster = 0;
    int      is_dir = 0;

    if (delete_iter(dir_cluster, name,
                    &file_cluster, &is_dir,
                    del_clus, del_idx, &del_count) != 0)
        return -1;

    if (is_dir && file_cluster >= 2) {
        if (!dir_is_empty(file_cluster)) return -1;
    }

    /* Mark all located entries as deleted */
    for (int k = 0; k < del_count; k++) {
        uint32_t c = del_clus[k];
        int      i = del_idx[k];
        if (read_cluster(c) != 0) return -1;
        clus_buf[i * 32] = 0xE5;
        if (write_cluster(c) != 0) return -1;
    }

    /* Free cluster chain */
    if (file_cluster >= 2) fat_free_chain(file_cluster);

    return 0;
}
