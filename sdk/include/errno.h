#ifndef _ERRNO_H
#define _ERRNO_H

extern int errno;

#define ENOENT  2   // No such file or directory
#define EIO     5   // I/O error
#define EAGAIN  11  // Try again
#define ENOMEM  12  // Out of memory
#define EACCES  13  // Permission denied
#define EEXIST  17  // File exists
#define EISDIR  21  // Is a directory  <-- ДОБАВЬ ВОТ ЭТО
#define EINVAL  22  // Invalid argument
#define ENOSPC  28  // No space left on device
#define EDOM 33     // Domain error (аргумент вне области определения)
#define ERANGE 34
#define ETIMEDOUT 110  // Connection / wait timed out (used by QuickJS)

#endif