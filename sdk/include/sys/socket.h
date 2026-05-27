#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
/* ---------------------------------------------------------------------------
 * EquinoxOS userspace socket API (phase 1).
 *
 * Tiny BSD-flavoured shim over syscalls 80–85. All numbers are signed ints
 * packed in rax; negative values are SOCK_ERR_* (see below). The kernel
 * blocks inside connect()/recv() — there is no select() / poll() yet.
 *
 * Typical TLS-bootstrap flow (used in phase 3 by the BearSSL port):
 *
 *     int s = sys_socket();
 *     if (s < 0) goto fail;
 *     if (sys_connect(s, ip, 443) < 0) goto fail;
 *     uint32_t rcvto = 8000;
 *     sys_setsockopt(s, SOCK_LEVEL_SOCKET, SOCK_OPT_RCVTIMEO,
 *                    &rcvto, sizeof rcvto);
 *     sys_send(s, "GET / HTTP/1.1\r\n...\r\n\r\n", n);
 *     for (;;) {
 *         int got = sys_recv(s, buf, sizeof buf);
 *         if (got <= 0) break;
 *         ...
 *     }
 *     sys_close_sock(s);
 *
 * The address argument is the raw IPv4 in network byte order (high byte = a
 * in a.b.c.d). Use the helper IPV4(a,b,c,d) below or call SYS_NET_DNS_RESOLVE
 * (syscall 40) first.
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include "../equos.h"

/* Error codes (mirror kernel's net/socket.h). */
#define SOCK_ERR_BADFD     -1
#define SOCK_ERR_NOMEM     -2
#define SOCK_ERR_NOTCONN   -3
#define SOCK_ERR_TIMEOUT   -4
#define SOCK_ERR_REFUSED   -5
#define SOCK_ERR_CLOSED    -6
#define SOCK_ERR_AGAIN     -7
#define SOCK_ERR_INVAL     -8

/* setsockopt levels / options. */
#define SOCK_LEVEL_SOCKET   1
#define SOCK_OPT_RCVTIMEO   1   /* uint32_t ms                          */
#define SOCK_OPT_NODELAY    2   /* accepted, ignored (we never Nagle)   */

#define IPV4(a,b,c,d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  |  (uint32_t)(d))

static inline int sys_socket(void) {
  return (int)(int64_t)_syscall(SYS_SOCKET, 0, 0, 0, 0, 0);
}
static inline int sys_connect(int fd, uint32_t ip_be, uint16_t port) {
  return (int)(int64_t)_syscall(SYS_CONNECT,
                                (uint64_t)fd, (uint64_t)ip_be,
                                (uint64_t)port, 0, 0);
}
static inline int sys_send(int fd, const void *buf, uint32_t len) {
  return (int)(int64_t)_syscall(SYS_SEND,
                                (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                                (uint64_t)len, 0, 0);
}
static inline int sys_recv(int fd, void *buf, uint32_t len) {
  return (int)(int64_t)_syscall(SYS_RECV,
                                (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                                (uint64_t)len, 0, 0);
}
static inline int sys_close_sock(int fd) {
  return (int)(int64_t)_syscall(SYS_CLOSE_SOCK, (uint64_t)fd, 0, 0, 0, 0);
}
static inline int sys_setsockopt(int fd, int level, int optname,
                                 const void *val, uint32_t vallen) {
  return (int)(int64_t)_syscall(SYS_SETSOCKOPT,
                                (uint64_t)fd, (uint64_t)level,
                                (uint64_t)optname,
                                (uint64_t)(uintptr_t)val, (uint64_t)vallen);
}

#endif
