/*
 * envtest — приёмочный тест Этапа 3 (env + cwd).
 *
 * Сценарии:
 *   1) setenv/getenv, overwrite=0 не перезаписывает;
 *   2) fork наследует окружение (копия адресного пространства), изменения
 *      в ребёнке НЕ видны родителю;
 *   3) execve/execv передаёт окружение новому образу (re-exec себя с "child");
 *   4) cwd: getcwd, chdir, относительное разрешение путей до/после cd,
 *      ошибка chdir в несуществующий каталог, сворачивание "..".
 *
 * Ожидаемый вывод:
 *   [env] start
 *   [env] getenv STAGE3_VAR=hello-env
 *   [env] after no-overwrite=hello-env
 *   [env] MYPATH=/bin:/usr/bin
 *   [env-child] inherited STAGE3_VAR=hello-env
 *   [env-child] modified=child-modified
 *   [env] parent STAGE3_VAR still=hello-env
 *   [execee] STAGE3_VAR=hello-env
 *   [execee] MYPATH=/bin:/usr/bin
 *   [cwd] start cwd=/
 *   [cwd] stat envtest.elf from / -> not found
 *   [cwd] chdir /bin -> 0
 *   [cwd] now cwd=/bin
 *   [cwd] stat envtest.elf from /bin -> FOUND
 *   [cwd] chdir /nonexistent_dir -> -1 (expect -1)
 *   [cwd] after /bin then .. cwd=/
 *   [envtest] all done
 */
#include <equos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    /* --- "execee"-ветка: пришли сюда через execv --- */
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        char *v = getenv("STAGE3_VAR");
        printf("[execee] STAGE3_VAR=%s\n", v ? v : "(null)");
        char *p = getenv("MYPATH");
        printf("[execee] MYPATH=%s\n", p ? p : "(null)");
        exit(0);
    }

    printf("[env] start\n");

    /* 1. setenv / getenv */
    setenv("STAGE3_VAR", "hello-env", 1);
    printf("[env] getenv STAGE3_VAR=%s\n", getenv("STAGE3_VAR"));
    setenv("STAGE3_VAR", "ignored", 0);                 /* overwrite=0 -> не меняет */
    printf("[env] after no-overwrite=%s\n", getenv("STAGE3_VAR"));
    setenv("MYPATH", "/bin:/usr/bin", 1);
    printf("[env] MYPATH=%s\n", getenv("MYPATH"));

    /* 2. fork наследует окружение */
    pid_t pid = fork();
    if (pid == 0) {
        char *v = getenv("STAGE3_VAR");
        printf("[env-child] inherited STAGE3_VAR=%s\n", v ? v : "(null)");
        setenv("STAGE3_VAR", "child-modified", 1);
        printf("[env-child] modified=%s\n", getenv("STAGE3_VAR"));
        exit(0);
    }
    waitpid(pid, 0, 0);
    printf("[env] parent STAGE3_VAR still=%s\n", getenv("STAGE3_VAR"));

    /* 3. execve/execv передаёт окружение */
    pid = fork();
    if (pid == 0) {
        char *av[] = { "bin/envtest.elf", "child", 0 };
        execv("bin/envtest.elf", av);                   /* execv передаёт environ */
        printf("[env-child] execv FAILED\n");
        exit(99);
    }
    waitpid(pid, 0, 0);

    /* 4. cwd */
    char buf[128];
    getcwd(buf, sizeof(buf));
    printf("[cwd] start cwd=%s\n", buf);

    struct stat st;
    int r1 = stat("envtest.elf", &st);                  /* "/"+rel -> "envtest.elf" -> нет */
    printf("[cwd] stat envtest.elf from / -> %s\n", r1 == 0 ? "FOUND" : "not found");

    int cd = chdir("/bin");
    printf("[cwd] chdir /bin -> %d\n", cd);
    getcwd(buf, sizeof(buf));
    printf("[cwd] now cwd=%s\n", buf);

    int r2 = stat("envtest.elf", &st);                  /* "/bin"+rel -> "bin/envtest.elf" -> есть */
    printf("[cwd] stat envtest.elf from /bin -> %s\n", r2 == 0 ? "FOUND" : "not found");

    int cd2 = chdir("/nonexistent_dir");
    printf("[cwd] chdir /nonexistent_dir -> %d (expect -1)\n", cd2);

    chdir("/bin");
    chdir("..");
    getcwd(buf, sizeof(buf));
    printf("[cwd] after /bin then .. cwd=%s\n", buf);

    printf("[envtest] all done\n");
    return 0;
}
