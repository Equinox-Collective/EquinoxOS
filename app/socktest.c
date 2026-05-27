/*
 * socktest - quick userspace smoke test for the phase-1 socket syscalls
 *            (sys_socket / sys_connect / sys_send / sys_recv / sys_close_sock
 *            / sys_setsockopt).
 *
 * Build:   it is enough to add  $(ISO_ROOT)/bin/socktest.elf  to
 *          APP_ELFS_SIMPLE in the Makefile, then  make all  picks it up.
 *
 * Use:     on the host, run a plain HTTP server bound to the QEMU host
 *          gateway:
 *
 *              python -m http.server 80 --bind 0.0.0.0
 *
 *          then inside EquinoxOS:
 *
 *              run bin/socktest.elf
 *
 *          Expected output (the exact byte counts depend on the index page):
 *
 *              [socktest] sys_socket -> fd=0
 *              [socktest] sys_connect 10.0.2.2:80 -> 0
 *              [socktest] sys_send 36 -> 36
 *              [socktest] sys_recv chunk=512 (total=512)
 *              [socktest] sys_recv chunk=... (total=...)
 *              [socktest] sys_recv -> 0 (peer closed)
 *              [socktest] first 64 bytes: HTTP/1.0 200 OK\r\nServer: SimpleHTTP/0.6 Python/3.x...
 *              [socktest] sys_close_sock -> 0
 *
 * If you have Clumsy running with Drop 30% on tcp port 80, you should still
 * see the same trailer plus several `[TCP] RTX: retransmit ...` lines in the
 * QEMU serial log — that exercises both the new socket API AND the phase-0
 * retransmission queue at the same time.
 */

#include <equos.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>

#define HOST_IP   IPV4(10, 0, 2, 2)
#define HOST_PORT 80

static const char REQ[] =
    "GET / HTTP/1.0\r\n"
    "Host: 10.0.2.2\r\n"
    "\r\n";

int main(void) {
    int fd = sys_socket();
    printf("[socktest] sys_socket -> fd=%d\n", fd);
    if (fd < 0) return 1;

    /* 8 s recv timeout — generous, so a lossy link with Clumsy still finishes. */
    uint32_t rcvto = 8000;
    sys_setsockopt(fd, SOCK_LEVEL_SOCKET, SOCK_OPT_RCVTIMEO, &rcvto, sizeof rcvto);

    int rc = sys_connect(fd, HOST_IP, HOST_PORT);
    printf("[socktest] sys_connect 10.0.2.2:80 -> %d\n", rc);
    if (rc < 0) { sys_close_sock(fd); return 2; }

    int sent = sys_send(fd, REQ, (uint32_t)(sizeof REQ - 1));
    printf("[socktest] sys_send %u -> %d\n", (unsigned)(sizeof REQ - 1), sent);
    if (sent < 0) { sys_close_sock(fd); return 3; }

    /* Pull bytes until the peer closes (recv returns 0) or we hit the timeout
     * (recv returns SOCK_ERR_TIMEOUT). */
    char     buf[1024];
    char     head[65]   = {0};
    uint32_t head_len   = 0;
    uint32_t total      = 0;
    int      n;
    while ((n = sys_recv(fd, buf, sizeof buf)) > 0) {
        total += (uint32_t)n;
        printf("[socktest] sys_recv chunk=%d (total=%u)\n", n, (unsigned)total);
        if (head_len < sizeof head - 1) {
            uint32_t copy = (uint32_t)n;
            if (copy > sizeof head - 1 - head_len) copy = sizeof head - 1 - head_len;
            memcpy(head + head_len, buf, copy);
            head_len += copy;
        }
    }
    if (n == 0) {
        printf("[socktest] sys_recv -> 0 (peer closed)\n");
    } else {
        printf("[socktest] sys_recv -> %d (error)\n", n);
    }

    head[head_len] = 0;
    /* Strip embedded \r so the print stays on one line. */
    for (uint32_t i = 0; i < head_len; i++) if (head[i] == '\r') head[i] = ' ';
    printf("[socktest] first %u bytes: %s\n", (unsigned)head_len, head);

    rc = sys_close_sock(fd);
    printf("[socktest] sys_close_sock -> %d\n", rc);
    return 0;
}
