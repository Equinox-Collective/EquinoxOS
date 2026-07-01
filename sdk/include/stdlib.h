#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void exit(int status);
int abs(int j);
int rand(void);
void srand(unsigned int seed);
char* itoa(int value, char* str, int base);
int atoi(const char* s);
double atof(const char* s);
int system(const char* command);
char *getenv(const char *name);
/* Этап 3: модификация окружения. */
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);
int   clearenv(void);
double strtod(const char *nptr, char **endptr);
void abort(void);

/* QuickJS uses alloca() for a few small scratch buffers. Provide the
 * declaration here so callers that include <stdlib.h> (as glibc users
 * do) see it without pulling in <alloca.h> explicitly. */
#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#ifdef __cplusplus
}
#endif
#endif