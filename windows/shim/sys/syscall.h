#ifndef GUFO_WINDOWS_SHIM_SYS_SYSCALL_H_
#define GUFO_WINDOWS_SHIM_SYS_SYSCALL_H_

#include <errno.h>

// Linux system calls have no Windows counterpart. Callers treat the failure
// as an unavailable feature.
#define SYS_memfd_create 319
#define MFD_CLOEXEC 1

inline long syscall(long /*number*/, ...) {
  errno = ENOSYS;
  return -1;
}

#endif  // GUFO_WINDOWS_SHIM_SYS_SYSCALL_H_
