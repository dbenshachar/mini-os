#include "syscalls.h"
#include "heap.h"

int sys_open(const char *path, int flags) {
    return fs_open(path, flags);
}

long sys_read(int fd, void *buf, unsigned long count) {
    return fs_read(fd, buf, count);
}

long sys_write(int fd, const void *buf, unsigned long count) {
    return fs_write(fd, buf, count);
}

int sys_close(int fd) {
    return fs_close(fd);
}

int sys_mkdir(const char *path) {
    return fs_mkdir(path);
}

int sys_remove(const char *path) {
    return fs_remove(path);
}

int sys_rmdir(const char *path) {
    return fs_rmdir(path);
}

void *sys_malloc(size_t size) {
    return malloc(size);
}

void sys_free(void *ptr) {
    free(ptr);
}
