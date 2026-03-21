#pragma once
#include <stdint.h>
#include <stddef.h>

#define VFS_FILE 0
#define VFS_DIR  1

void        vfs_init(void);
int         vfs_stat(const char *path, int *out_is_dir, uint32_t *out_size);
int         vfs_readdir(const char *path,
                        void (*cb)(const char *name, int is_dir,
                                   uint32_t size, void *ud),
                        void *ud);
int         vfs_read(const char *path, void *buf, uint32_t maxlen);
int         vfs_write(const char *path, const void *buf, uint32_t len);
int         vfs_append(const char *path, const void *buf, uint32_t len);
int         vfs_create(const char *path);
int         vfs_mkdir(const char *path);
int         vfs_delete(const char *path);
int         vfs_chdir(const char *path);
const char *vfs_pwd(void);
