#ifndef MINI_OS_NET_H
#define MINI_OS_NET_H
#include <stddef.h>
int net_init(void);
int tcp_connect(const char *ipv4, unsigned short port);
long tcp_send(int socket, const void *data, size_t count);
long tcp_recv(int socket, void *data, size_t count);
int tcp_close(int socket);
#endif
