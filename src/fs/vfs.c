#include <stdint.h>
#include <stddef.h>
#include "fs/vfs.h"
#include "fs/fat32.h"

/* ------------------------------------------------------------------ */
/* Internal string helpers (no stdlib)                                 */
/* ------------------------------------------------------------------ */

static size_t vstr_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void vstr_cpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int vstr_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static uint32_t cwd_cluster;
static char     pwd_buf[512];
static int      fat32_ok = 0;

/* ------------------------------------------------------------------ */
/* vfs_init                                                            */
/* ------------------------------------------------------------------ */

void vfs_init(void) {
    if (fat32_mount() == 0) {
        fat32_ok    = 1;
        cwd_cluster = fat32_root_cluster();
        vstr_cpy(pwd_buf, "/", 512);
    } else {
        fat32_ok = 0;
        cwd_cluster = 0;
        vstr_cpy(pwd_buf, "/", 512);
    }
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/*                                                                     */
/* resolve_path:                                                       */
/*   - Walks path component by component from root or cwd             */
/*   - Returns final cluster in *out_cluster                           */
/*   - Returns parent cluster in *out_parent (may be 0 for root)      */
/*   - Copies last component name into out_name (up to name_max bytes) */
/*   - Returns 0 on full success (last component exists)              */
/*   - Returns 1 if parent exists but last component not resolved     */
/*     (out_parent and out_name filled, out_cluster = 0)              */
/*   - Returns -1 on error                                            */
/* ------------------------------------------------------------------ */

#define COMP_MAX 256

static int resolve_path(const char *path,
                        uint32_t *out_cluster,
                        uint32_t *out_parent,
                        char     *out_name,
                        size_t    name_max) {
    if (!fat32_ok) return -1;
    if (!path || !path[0]) return -1;

    uint32_t cur;
    const char *p = path;

    if (*p == '/') {
        cur = fat32_root_cluster();
        p++;
    } else {
        cur = cwd_cluster;
    }

    uint32_t parent = cur;
    char comp[COMP_MAX];

    /* If path was just "/" */
    if (*p == '\0') {
        if (out_cluster) *out_cluster = cur;
        if (out_parent)  *out_parent  = cur;
        if (out_name && name_max > 0) out_name[0] = '\0';
        return 0;
    }

    while (*p) {
        /* Extract component */
        size_t ci = 0;
        while (*p && *p != '/' && ci + 1 < COMP_MAX) {
            comp[ci++] = *p++;
        }
        comp[ci] = '\0';
        if (*p == '/') p++;

        /* Skip empty or "." */
        if (ci == 0 || vstr_eq(comp, ".")) continue;

        /* Check if more components follow */
        int more = (*p != '\0');

        if (!more) {
            /* This is the last component */
            if (out_parent) *out_parent = cur;
            if (out_name)   vstr_cpy(out_name, comp, name_max);

            /* Try to resolve it */
            uint32_t next_c = 0, next_s = 0;
            int      next_d = 0;
            if (fat32_find(cur, comp, &next_c, &next_s, &next_d) == 0) {
                if (out_cluster) *out_cluster = next_c;
                return 0;
            } else {
                if (out_cluster) *out_cluster = 0;
                return 1; /* parent found, but last component doesn't exist */
            }
        } else {
            /* Intermediate component */
            uint32_t next_c = 0, next_s = 0;
            int      next_d = 0;
            if (fat32_find(cur, comp, &next_c, &next_s, &next_d) != 0)
                return -1;
            if (!next_d) return -1; /* intermediate must be a dir */
            parent = cur;
            cur    = next_c;
        }
    }

    /* Path ended with a slash — cur is the target */
    if (out_cluster) *out_cluster = cur;
    if (out_parent)  *out_parent  = parent;
    if (out_name && name_max > 0) out_name[0] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_stat                                                            */
/* ------------------------------------------------------------------ */

int vfs_stat(const char *path, int *out_is_dir, uint32_t *out_size) {
    if (!fat32_ok) return -1;

    /* Root directory special case */
    if (vstr_eq(path, "/")) {
        if (out_is_dir) *out_is_dir = 1;
        if (out_size)   *out_size   = 0;
        return 0;
    }

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (r != 0) return -1;

    /* Re-find via parent to get is_dir and size */
    uint32_t fc = 0, fsz = 0;
    int      fdir = 0;
    if (fat32_find(parent, name, &fc, &fsz, &fdir) != 0) return -1;

    if (out_is_dir) *out_is_dir = fdir;
    if (out_size)   *out_size   = fsz;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_readdir                                                         */
/* ------------------------------------------------------------------ */

int vfs_readdir(const char *path,
                void (*cb)(const char *name, int is_dir, uint32_t size, void *ud),
                void *ud) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];

    if (vstr_eq(path, "/") || vstr_eq(path, ".")) {
        cluster = (vstr_eq(path, "/")) ? fat32_root_cluster() : cwd_cluster;
    } else {
        int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
        if (r != 0) return -1;
        if (cluster == 0) return -1;
    }

    return fat32_list(cluster, cb, ud);
}

/* ------------------------------------------------------------------ */
/* vfs_read                                                            */
/* ------------------------------------------------------------------ */

int vfs_read(const char *path, void *buf, uint32_t maxlen) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (r != 0) return -1;
    if (cluster == 0) return -1;

    /* Get size via stat */
    uint32_t fc = 0, fsz = 0;
    int      fdir = 0;
    if (fat32_find(parent, name, &fc, &fsz, &fdir) != 0) return -1;
    if (fdir) return -1; /* can't read a directory */

    return fat32_read(fc, fsz, buf, maxlen);
}

/* ------------------------------------------------------------------ */
/* vfs_write                                                           */
/* ------------------------------------------------------------------ */

int vfs_write(const char *path, const void *buf, uint32_t len) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    /* We don't care if the last component doesn't exist (create case) */
    resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (parent == 0 && !vstr_eq(path, "/")) {
        /* Couldn't resolve parent */
        return -1;
    }
    if (name[0] == '\0') return -1;

    return fat32_write(parent, name, buf, len);
}

/* ------------------------------------------------------------------ */
/* vfs_append                                                          */
/* ------------------------------------------------------------------ */

int vfs_append(const char *path, const void *buf, uint32_t len) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (parent == 0 && !vstr_eq(path, "/")) return -1;
    if (name[0] == '\0') return -1;

    return fat32_append(parent, name, buf, len);
}

/* ------------------------------------------------------------------ */
/* vfs_create                                                          */
/* ------------------------------------------------------------------ */

int vfs_create(const char *path) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
    /* r==1 means parent exists but file doesn't — that's what we want */
    if (r == 0) return -1; /* already exists */
    if (r == -1) return -1;
    if (name[0] == '\0') return -1;

    return fat32_create(parent, name, 0, parent);
}

/* ------------------------------------------------------------------ */
/* vfs_mkdir                                                           */
/* ------------------------------------------------------------------ */

int vfs_mkdir(const char *path) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (r == 0) return -1; /* already exists */
    if (r == -1) return -1;
    if (name[0] == '\0') return -1;

    return fat32_create(parent, name, 1, parent);
}

/* ------------------------------------------------------------------ */
/* vfs_delete                                                          */
/* ------------------------------------------------------------------ */

int vfs_delete(const char *path) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];
    int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (r != 0) return -1;
    if (name[0] == '\0') return -1;

    return fat32_delete(parent, name);
}

/* ------------------------------------------------------------------ */
/* vfs_chdir                                                           */
/* ------------------------------------------------------------------ */

int vfs_chdir(const char *path) {
    if (!fat32_ok) return -1;

    uint32_t cluster = 0, parent = 0;
    char name[COMP_MAX];

    /* Special case: cd "/" */
    if (vstr_eq(path, "/")) {
        cwd_cluster = fat32_root_cluster();
        vstr_cpy(pwd_buf, "/", 512);
        return 0;
    }

    int r = resolve_path(path, &cluster, &parent, name, COMP_MAX);
    if (r != 0) return -1;
    if (cluster == 0) return -1;

    /* Verify it's a directory */
    if (!vstr_eq(path, "/")) {
        uint32_t fc = 0, fsz = 0;
        int      fdir = 0;
        if (fat32_find(parent, name, &fc, &fsz, &fdir) != 0) return -1;
        if (!fdir) return -1;
    }

    cwd_cluster = cluster;

    /* Update pwd_buf */
    if (path[0] == '/') {
        /* Absolute path */
        vstr_cpy(pwd_buf, path, 512);
        /* Normalize trailing slash */
        size_t plen = vstr_len(pwd_buf);
        if (plen > 1 && pwd_buf[plen - 1] == '/')
            pwd_buf[plen - 1] = '\0';
    } else {
        /* Relative path — walk components */
        const char *p = path;
        char comp2[COMP_MAX];

        while (*p) {
            size_t ci = 0;
            while (*p && *p != '/' && ci + 1 < COMP_MAX)
                comp2[ci++] = *p++;
            comp2[ci] = '\0';
            if (*p == '/') p++;
            if (ci == 0 || vstr_eq(comp2, ".")) continue;

            if (vstr_eq(comp2, "..")) {
                /* Strip last component from pwd_buf */
                size_t plen = vstr_len(pwd_buf);
                if (plen > 1) {
                    /* Find last '/' */
                    int li = (int)plen - 1;
                    while (li > 0 && pwd_buf[li] != '/') li--;
                    if (li == 0)
                        pwd_buf[1] = '\0'; /* back to root */
                    else
                        pwd_buf[li] = '\0';
                }
            } else {
                /* Append /comp2 */
                size_t plen = vstr_len(pwd_buf);
                if (plen + 1 + ci + 1 < 512) {
                    if (plen > 1 || pwd_buf[plen-1] != '/') {
                        pwd_buf[plen++] = '/';
                    }
                    for (size_t k = 0; k < ci; k++)
                        pwd_buf[plen + k] = comp2[k];
                    pwd_buf[plen + ci] = '\0';
                }
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* vfs_pwd                                                             */
/* ------------------------------------------------------------------ */

const char *vfs_pwd(void) {
    return pwd_buf;
}
