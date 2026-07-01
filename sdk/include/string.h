#ifndef LIBC_STRING_H
#define LIBC_STRING_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
void* memmove(void* dest, const void* src, size_t n);

size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);

int memcmp(const void* s1, const void* s2, size_t n);
void* memchr(const void* s, int c, size_t n);
char* strstr(const char* haystack, const char* needle);
char* strcat(char* dest, const char* src);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strdup(const char* s);

void itoa_hex(uint64_t num, char* buffer);
char* itoa(int num, char* buffer, int base);
int rand(void);
char *strerror(int errnum);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
int strcoll(const char *s1, const char *s2);

#ifdef __cplusplus
}
#endif
#endif