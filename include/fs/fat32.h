#pragma once
#include <stdint.h>
#include <stddef.h>

/* Mount FAT32 from disk. Returns 0 on success. */
int      fat32_mount(void);

/* Returns the root directory cluster number. */
uint32_t fat32_root_cluster(void);

/*
 * Find a named entry in a directory cluster.
 * name: the filename to search for (case-insensitive, UTF-8 ASCII)
 * out_cluster: first data cluster of found entry
 * out_size:    file size in bytes (0 for dirs)
 * out_is_dir:  1 if directory, 0 if file
 * Returns 0 on success, -1 if not found.
 */
int fat32_find(uint32_t dir_cluster, const char *name,
               uint32_t *out_cluster, uint32_t *out_size,
               int *out_is_dir);

/*
 * List all entries in a directory cluster.
 * Calls cb(name, is_dir, size, ud) for each valid entry.
 * Returns 0 on success, -1 on error.
 */
int fat32_list(uint32_t dir_cluster,
               void (*cb)(const char *name, int is_dir,
                          uint32_t size, void *ud),
               void *ud);

/*
 * Read file data starting at start_cluster.
 * Reads up to maxlen bytes into buf.
 * Returns bytes read, or -1 on error.
 */
int fat32_read(uint32_t start_cluster, uint32_t file_size,
               void *buf, uint32_t maxlen);

/*
 * Write data to a file. If the file has existing clusters, they are
 * freed and replaced. The directory entry at (dir_cluster, name) is
 * updated with the new first_cluster and size.
 *
 * Creates the file if it does not exist.
 * Returns 0 on success, -1 on error.
 */
int fat32_write(uint32_t dir_cluster, const char *name,
                const void *data, uint32_t len);

/*
 * Append data to an existing file.
 * If the file does not exist, it is created.
 */
int fat32_append(uint32_t dir_cluster, const char *name,
                 const void *data, uint32_t len);

/*
 * Create an empty file (attr = ATTR_ARCHIVE) or directory
 * (attr = ATTR_DIRECTORY) in dir_cluster.
 * Returns 0 on success, -1 if already exists or error.
 * For directories: allocates one cluster, writes . and .. entries.
 */
int fat32_create(uint32_t dir_cluster, const char *name,
                 int is_dir, uint32_t parent_cluster);

/*
 * Delete a file or empty directory from dir_cluster.
 * Returns 0 on success, -1 if not found or directory not empty.
 */
int fat32_delete(uint32_t dir_cluster, const char *name);
