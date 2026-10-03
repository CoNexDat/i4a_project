#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int fd; } fd_set;
struct timeval { long tv_sec, tv_usec; };
#define FD_ZERO(set) ((set)->fd = -1)
#define FD_SET(sock, set) ((set)->fd = (sock))
#define SHUT_RDWR 2
#define SOL_SOCKET 1
#define SO_SNDTIMEO 2
#define SO_KEEPALIVE 3
#define IPPROTO_TCP 4
#define TCP_KEEPIDLE 5
#define TCP_KEEPINTVL 6
#define TCP_KEEPCNT 7
int shutdown(int sock, int how);
int setsockopt(int sock, int level, int option, const void *value, unsigned len);
int send(int sock, const void *data, unsigned len, int flags);
int recv(int sock, void *data, unsigned len, int flags);
int lwip_select(int nfds, fd_set *readers, fd_set *writers, fd_set *except, struct timeval *timeout);
