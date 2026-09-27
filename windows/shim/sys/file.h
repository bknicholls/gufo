#ifndef GUFO_WINDOWS_SHIM_SYS_FILE_H_
#define GUFO_WINDOWS_SHIM_SYS_FILE_H_

#include <io.h>
#include <windows.h>

#include <cerrno>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

inline int flock(int fd, int operation) {
  const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED whole_file{};
  if ((operation & LOCK_UN) != 0)
    return UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &whole_file) ? 0 : -1;
  DWORD flags = (operation & LOCK_EX) != 0 ? LOCKFILE_EXCLUSIVE_LOCK : 0;
  if ((operation & LOCK_NB) != 0)
    flags |= LOCKFILE_FAIL_IMMEDIATELY;
  if (!LockFileEx(file, flags, 0, MAXDWORD, MAXDWORD, &whole_file)) {
    errno = EWOULDBLOCK;
    return -1;
  }
  return 0;
}

#endif  // GUFO_WINDOWS_SHIM_SYS_FILE_H_
