/* musltest — первый запуск настоящего musl на EquinoxOS (Этап 6b-2).
 * Линкуется с musl crt1.o + libc.a. _start даёт musl, он читает SysV-кадр
 * (6b-1), ставит TLS через arch_prctl (6b), затем зовёт main. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
    /* 1. printf через настоящий musl stdio -> write(1) */
    printf("musltest: musl %s\n", "1.2.5");
    printf("  argc=%d argv0=%s\n", argc, argc ? argv[0] : "(null)");

    /* 2. malloc/free (mmap-арена) */
    char *buf = malloc(256);
    if (!buf) { printf("  malloc FAIL\n"); return 1; }
    strcpy(buf, "heap-ok");
    printf("  malloc/strcpy: %s @ %p\n", buf, (void*)buf);
    free(buf);

    /* 3. getenv */
    const char *path = getenv("PATH");
    printf("  getenv(PATH)=%s\n", path ? path : "(unset)");

    /* 4. clock_gettime (syscall 228) */
    struct timespec ts = {0};
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("  clock_gettime rc=%d sec=%ld\n", rc, (long)ts.tv_sec);

    /* 5. uname (syscall 63) */
    struct utsname un;
    memset(&un, 0, sizeof un);
    if (uname(&un) == 0)
        printf("  uname: %s %s\n", un.sysname, un.release);
    else
        printf("  uname FAIL\n");

    /* 6. snprintf — чисто пользовательский путь форматирования */
    char small[32];
    snprintf(small, sizeof small, "%d+%d=%d", 2, 40, 42);
    printf("  snprintf: %s\n", small);

    printf("musltest: PASS\n");
    return 0;
}
