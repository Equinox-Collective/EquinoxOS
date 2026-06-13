/* mmfork — диагностический тест Этапа 9: переживают ли mmap-регионы fork.
 * 1) mmap страницы, записать магию; 2) fork; 3) ребёнок читает магию
 *    и пишет результат; 4) родитель ждёт. */
#include <equos.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    volatile uint64_t *m =
        (volatile uint64_t *)_syscall(SYS_MMAP, 0, 8192, 0, 0, 0);
    if (!m) { printf("[mmfork] mmap FAILED\n"); return 1; }
    printf("[mmfork] mmap at %p\n", (void *)m);
    m[0] = 0xDEADBEEFCAFEBABEULL;
    m[512] = 0x1122334455667788ULL;

    int64_t pid = sys_fork();
    if (pid < 0) { printf("[mmfork] fork FAILED\n"); return 1; }
    if (pid == 0) {
        uint64_t a = m[0], b = m[512];
        if (a == 0xDEADBEEFCAFEBABEULL && b == 0x1122334455667788ULL)
            printf("[mmfork] CHILD OK: mmap survived fork\n");
        else
            printf("[mmfork] CHILD BAD: a=%lx b=%lx\n",
                   (unsigned long)a, (unsigned long)b);
        exit(0);
    }
    int st = 0;
    sys_waitpid(pid, &st);
    printf("[mmfork] parent done\n");
    return 0;
}
