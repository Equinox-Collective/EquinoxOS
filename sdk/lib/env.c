/* sdk/lib/env.c — Этап 3: переменные окружения (environ + POSIX API).
 *
 * Модель: ядро передаёт envp в rdx при execve; crt0 вызывает
 * __libc_init_env(envp), которая ставит глобальный `environ`. До первой
 * модификации `environ` указывает на массив, подготовленный ядром (в env-
 * странице процесса). При первом setenv/putenv/unsetenv массив указателей
 * лениво копируется в кучу (env_vec) — строки самого окружения остаются
 * валидными на всё время жизни процесса.
 *
 * fork наследует окружение автоматически (адресное пространство копируется).
 * execve передаёт явный envp; execv/execvp передают текущий `environ`. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char **environ = NULL;

static char  *empty_environ[1] = { NULL };
static char **env_vec   = NULL;   /* массив-владелец в куче (после ensure_owned) */
static int    env_count = 0;
static int    env_cap   = 0;
static int    env_owned = 0;

/* Вызывается crt0 при старте процесса. */
void __libc_init_env(char **envp) {
    environ   = envp ? envp : empty_environ;
    env_vec   = NULL;
    env_count = 0;
    env_cap   = 0;
    env_owned = 0;
}

/* entry имеет вид "NAME=VALUE"? Сравниваем NAME длиной nlen. */
static int name_matches(const char *entry, const char *name, int nlen) {
    for (int i = 0; i < nlen; i++) {
        if (entry[i] == '\0' || entry[i] != name[i]) return 0;
    }
    return entry[nlen] == '=';
}

char *getenv(const char *name) {
    if (!name || !environ) return NULL;
    int nlen = (int)strlen(name);
    if (nlen == 0) return NULL;
    for (char **e = environ; *e; e++) {
        if (name_matches(*e, name, nlen)) return *e + nlen + 1;
    }
    return NULL;
}

/* Лениво переносим массив указателей в кучу, чтобы можно было менять. */
static int ensure_owned(void) {
    if (env_owned) return 1;
    int n = 0;
    if (environ) while (environ[n]) n++;
    env_cap = n + 8;
    env_vec = (char **)malloc(sizeof(char *) * (env_cap + 1));
    if (!env_vec) return 0;
    for (int i = 0; i < n; i++) env_vec[i] = environ[i];
    env_count = n;
    env_vec[n] = NULL;
    environ = env_vec;
    env_owned = 1;
    return 1;
}

static int env_grow(void) {
    if (env_count + 1 < env_cap) return 1;
    int ncap = env_cap * 2 + 8;
    char **nv = (char **)malloc(sizeof(char *) * (ncap + 1));
    if (!nv) return 0;
    for (int i = 0; i <= env_count; i++) nv[i] = env_vec[i];
    free(env_vec);
    env_vec = nv;
    env_cap = ncap;
    environ = env_vec;
    return 1;
}

static char *make_kv(const char *name, const char *value) {
    int nl = (int)strlen(name);
    int vl = (int)strlen(value);
    char *s = (char *)malloc((size_t)nl + 1 + (size_t)vl + 1);
    if (!s) return NULL;
    memcpy(s, name, nl);
    s[nl] = '=';
    memcpy(s + nl + 1, value, vl);
    s[nl + 1 + vl] = '\0';
    return s;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || !value) return -1;
    for (const char *p = name; *p; p++) if (*p == '=') return -1;
    if (!ensure_owned()) return -1;
    int nlen = (int)strlen(name);
    for (int i = 0; i < env_count; i++) {
        if (name_matches(env_vec[i], name, nlen)) {
            if (!overwrite) return 0;
            char *s = make_kv(name, value);
            if (!s) return -1;
            env_vec[i] = s;        /* старую строку оставляем (может быть не из кучи) */
            return 0;
        }
    }
    if (!env_grow()) return -1;
    char *s = make_kv(name, value);
    if (!s) return -1;
    env_vec[env_count++] = s;
    env_vec[env_count] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name) return -1;
    if (!environ) return 0;
    if (!ensure_owned()) return -1;
    int nlen = (int)strlen(name);
    for (int i = 0; i < env_count; i++) {
        if (name_matches(env_vec[i], name, nlen)) {
            for (int j = i; j < env_count; j++) env_vec[j] = env_vec[j + 1];
            env_count--;
            i--;
        }
    }
    return 0;
}

/* putenv: строка "NAME=VALUE" становится ЧАСТЬЮ окружения (не копируется). */
int putenv(char *string) {
    if (!string) return -1;
    int nlen = 0;
    while (string[nlen] && string[nlen] != '=') nlen++;
    if (string[nlen] != '=') return -1;
    if (!ensure_owned()) return -1;
    for (int i = 0; i < env_count; i++) {
        if (name_matches(env_vec[i], string, nlen)) { env_vec[i] = string; return 0; }
    }
    if (!env_grow()) return -1;
    env_vec[env_count++] = string;
    env_vec[env_count] = NULL;
    return 0;
}

int clearenv(void) {
    if (!ensure_owned()) return -1;
    env_count = 0;
    if (env_vec) env_vec[0] = NULL;
    return 0;
}
