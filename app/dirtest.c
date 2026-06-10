/*
 * dirtest.c — Этап 6c-2: каталоги через musl opendir/readdir (getdents64=217).
 *
 * Линкуется с vendored-musl (как musltest/stattest). Проверяет:
 *   • opendir("/bin") + readdir перечисляет файлы, среди них stattest.elf и
 *     musltest.elf, плюс "." и "..";
 *   • opendir("/") показывает подкаталоги bin/res/sys (DT_DIR);
 *   • opendir несуществующего каталога возвращает NULL.
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif

/* Перечислить path, напечатать записи, вернуть число записей. Каждое имя из
 * want[] (NULL-терминированный массив) ищется; found[] выставляется. */
static int list_dir(const char *path, const char *const want[], int found[]) {
    DIR *d = opendir(path);
    if (!d) {
        printf("  opendir(%s) FAILED errno=%d\n", path, errno);
        return -1;
    }
    struct dirent *e;
    int n = 0;
    printf("  %s:", path);
    while ((e = readdir(d)) != NULL) {
        n++;
        if (n <= 40)
            printf(" %s%s", e->d_name, (e->d_type == DT_DIR ? "/" : ""));
        if (want) {
            for (int i = 0; want[i]; i++)
                if (strcmp(e->d_name, want[i]) == 0) found[i] = 1;
        }
    }
    printf("\n  (count=%d)\n", n);
    closedir(d);
    return n;
}

int main(void) {
    printf("dirtest: opendir/readdir/getdents64 6c-2\n");
    int ok = 1;

    /* /bin должен содержать stattest.elf, musltest.elf, "." и ".." */
    const char *const bin_want[] = { "stattest.elf", "musltest.elf", ".", "..", 0 };
    int bin_found[4] = { 0, 0, 0, 0 };
    int bn = list_dir("/bin", bin_want, bin_found);
    if (bn < 0) ok = 0;
    for (int i = 0; bin_want[i]; i++)
        if (!bin_found[i]) { printf("  FAIL: '%s' not found in /bin\n", bin_want[i]); ok = 0; }

    /* корень должен содержать каталоги bin, res, sys */
    const char *const root_want[] = { "bin", "res", "sys", 0 };
    int root_found[3] = { 0, 0, 0 };
    int rn = list_dir("/", root_want, root_found);
    if (rn < 0) ok = 0;
    for (int i = 0; root_want[i]; i++)
        if (!root_found[i]) { printf("  FAIL: dir '%s' not listed in /\n", root_want[i]); ok = 0; }

    /* несуществующий каталог -> opendir вернёт NULL */
    DIR *bad = opendir("/no_such_dir_xyz");
    if (bad) { printf("  FAIL: opendir(/no_such_dir_xyz) should have failed\n"); closedir(bad); ok = 0; }
    else printf("  opendir(/no_such_dir_xyz) correctly returned NULL\n");

    printf("dirtest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
