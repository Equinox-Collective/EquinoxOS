/* stattest.c — Этап 6c-1: настоящий stdio-ввод/вывод файлов musl + Linux stat.
 *
 * Проверяет, что поверх Linux-ABI шлюза (int 0x81) работают:
 *   - fopen/fprintf/fwrite/fflush/ftell/rewind/fread/fclose (через
 *     openat/writev/readv/lseek/close);
 *   - fstat(fd) -> Linux struct stat (S_IFREG, размер, blksize);
 *   - stat(path) по пути;
 *   - fstat(stdout) -> S_IFCHR + isatty();
 *   - access(path).
 *
 * Линкуется с предсобранной vendored-musl (third_party/musl), как musltest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

int main(void)
{
    int fails = 0;
    printf("stattest: musl stdio+stat 6c-1\n");

    const char *path = "stattest.txt";

    /* 1) Round-trip в одном потоке w+: запись -> rewind -> чтение. */
    FILE *f = fopen(path, "w+");
    if (!f) { printf("  fopen(w+) FAIL errno=%d\n", errno); return 1; }

    int nh = fprintf(f, "hello=%d\n", 42);   /* "hello=42\n" = 9 */
    size_t nw = fwrite("ABCDE", 1, 5, f);    /* +5 = 14 */
    fflush(f);
    long endpos = ftell(f);
    int total = nh + (int)nw;
    printf("  wrote %d+%zu bytes, ftell=%ld\n", nh, nw, endpos);

    rewind(f);
    char buf[64];
    memset(buf, 0, sizeof(buf));
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    buf[r] = '\0';
    printf("  fread %zu bytes: \"%s\"\n", r, buf);
    if ((int)r != total || strncmp(buf, "hello=42\nABCDE", 14) != 0) {
        printf("  roundtrip FAIL\n"); fails++;
    } else {
        printf("  roundtrip OK\n");
    }

    /* 2) fstat на открытом файле. */
    struct stat st;
    if (fstat(fileno(f), &st) == 0) {
        printf("  fstat: size=%lld mode=0%o isreg=%d blksize=%ld\n",
               (long long)st.st_size, (unsigned)st.st_mode,
               S_ISREG(st.st_mode) ? 1 : 0, (long)st.st_blksize);
        if (!S_ISREG(st.st_mode) || st.st_size != total) { printf("  fstat FAIL\n"); fails++; }
    } else {
        printf("  fstat FAIL errno=%d\n", errno); fails++;
    }

    fclose(f);

    /* 3) stat по пути (после fclose -> файл сброшен в ext2). Информативно. */
    struct stat st2;
    if (stat(path, &st2) == 0)
        printf("  stat(%s): size=%lld isreg=%d\n",
               path, (long long)st2.st_size, S_ISREG(st2.st_mode) ? 1 : 0);
    else
        printf("  stat(%s): errno=%d (персист ext2?)\n", path, errno);

    /* 4) stdout — символьное устройство + isatty. */
    struct stat sto;
    if (fstat(1, &sto) == 0)
        printf("  stdout: ischr=%d isatty=%d\n", S_ISCHR(sto.st_mode) ? 1 : 0, isatty(1));
    else { printf("  fstat(stdout) FAIL\n"); fails++; }

    /* 5) access. */
    printf("  access(%s,F_OK)=%d  access(/nope_xyz,F_OK)=%d\n",
           path, access(path, F_OK), access("/nope_xyz", F_OK));

    if (fails) printf("stattest: FAIL (%d)\n", fails);
    else       printf("stattest: PASS\n");
    return fails ? 1 : 0;
}
