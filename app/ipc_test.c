/*
 * ipc_test - quick userspace test for sys_pipe_* and sys_mq_*.
 *
 * Drop into `app/ipc_test.c`. Make sure $(ISO_ROOT)/bin/ipc_test.elf is
 * listed in APP_ELFS_SIMPLE in the Makefile. Then `make all && make run`,
 * and in the EquinoxOS shell:  run bin/ipc_test.elf
 */

#include <equos.h>
#include <stdio.h>
#include <string.h>

#define MQ_MSG_SIZE 32

int main(void) {
    /* ---- pipe ---- */
    int p = sys_pipe_create();
    printf("pipe: id=%d\n", p);

    const char *msg = "hello";
    int wrote = sys_pipe_write(p, msg, 5);
    char buf[16] = {0};
    int got = sys_pipe_read(p, buf, sizeof(buf) - 1);
    if (got < 0) got = 0;
    buf[got] = 0;
    printf("pipe: wrote=%d read=%d '%s'\n", wrote, got, buf);
    sys_pipe_close(p);

    /* ---- mqueue: priority ordering ----
     * Fixed message size per queue (set at create time). */
    int q = sys_mq_create(MQ_MSG_SIZE);
    printf("mq:   id=%d\n", q);

    /* Pad messages to MQ_MSG_SIZE so the queue stores them cleanly. */
    char m_low [MQ_MSG_SIZE] = {0};
    char m_high[MQ_MSG_SIZE] = {0};
    char m_mid [MQ_MSG_SIZE] = {0};
    strcpy(m_low,  "low");
    strcpy(m_high, "high");
    strcpy(m_mid,  "mid");

    sys_mq_send(q, m_low,  1);
    sys_mq_send(q, m_high, 9);
    sys_mq_send(q, m_mid,  5);

    char rb[MQ_MSG_SIZE + 1];
    for (int i = 0; i < 3; i++) {
        memset(rb, 0, sizeof(rb));
        int n = sys_mq_recv(q, rb);
        (void)n;
        printf("mq:   recv[%d] = '%s'\n", i, rb);
    }
    sys_mq_close(q);

    printf("ipc_test: OK\n");

    while (1) sys_yield();
    return 0;
}
