#ifndef MINI_OS_FILES_H
#define MINI_OS_FILES_H

#include <stdint.h>
#include <stddef.h>

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

#define FS_SECTOR_SIZE 512

typedef struct {
    char name[13];
    uint8_t is_dir;
    uint32_t size;
} FsDirEntry;

int fs_init();
int fs_open(const char *path, int flags);
int fs_close(int fd);
long fs_read(int fd, void *buf, unsigned long count);
long fs_write(int fd, const void *buf, unsigned long count);
int fs_list(const char *path, void (*emit)(const FsDirEntry *entry, void *ctx), void *ctx);
int fs_mkdir(const char *path);
int fs_remove(const char *path);
int fs_rmdir(const char *path);

#endif
