#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stddef.h>
#include "files.h"

int sys_open(const char *path, int flags);
long sys_read(int fd, void *buf, unsigned long count);
long sys_write(int fd, const void *buf, unsigned long count);
int sys_close(int fd);
int sys_mkdir(const char *path);
int sys_remove(const char *path);
int sys_rmdir(const char *path);
void *sys_malloc(size_t size);
void sys_free(void *ptr);

#endif
