#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* access() mode flags */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* lseek / seek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* POSIX wrappers backed by EquinoxOS fd syscalls (90-96) */

int     open(const char *path, int flags, ...);
int     close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t   lseek(int fd, off_t offset, int whence);
int     access(const char *pathname, int mode);
int     unlink(const char *pathname);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     isatty(int fd);
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);
/* Этап 3: глобальное окружение процесса (реализовано в env.c). */
extern char **environ;
pid_t   getpid(void);
pid_t   getppid(void);
/* Этап 1: процессная модель. fork() также объявлена в <sys/wait.h>. */
pid_t   fork(void);

/* Этап 1b: execve и обёртки. При успехе НЕ возвращаются. */
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);

#endif /* _UNISTD_H */
