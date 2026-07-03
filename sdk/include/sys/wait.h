#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

/*
 * EquinoxOS — <sys/wait.h>  (Этап 1: процессная модель)
 *
 * Минимальный POSIX-интерфейс ожидания потомков. Реализация — в
 * sdk/lib/posix.c поверх syscalls SYS_WAITPID (52) / SYS_FORK (51).
 *
 * Кодировка status (как у обычного wait): младший байт — номер сигнала
 * (0, если процесс завершился через exit), следующий байт — код возврата.
 * Сейчас ядро доставляет только код выхода; сигналы появятся на этапе 4.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* options для waitpid (WNOHANG пока игнорируется — всегда блокирующий) */
#define WNOHANG    1
#define WUNTRACED  2

#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0 && ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status)    ((status) & 0x7F)
#define WIFSTOPPED(status)  (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)    WEXITSTATUS(status)

pid_t fork(void);
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
