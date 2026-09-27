#ifndef GUFO_WINDOWS_SHIM_FCNTL_H_
#define GUFO_WINDOWS_SHIM_FCNTL_H_

#include <errno.h>
#include_next <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#define O_CLOEXEC _O_NOINHERIT
// Windows' unbuffered I/O needs sector-aligned buffers and offsets that POSIX
// O_DIRECT callers do not guarantee, so direct reads fall back to buffered.
#define O_DIRECT 0

#define F_GETFD 1
#define F_SETFD 2
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

// The subset of fcntl the POSIX code uses: duplicating a descriptor, and the
// close-on-exec flag, which Windows descriptors already carry as
// non-inheritance.
inline int fcntl(int descriptor, int command, int /*argument*/ = 0) {
  switch (command) {
    case F_DUPFD_CLOEXEC:
      return _dup(descriptor);
    case F_GETFD:
      return FD_CLOEXEC;
    case F_SETFD:
      return 0;
    default:
      errno = EINVAL;
      return -1;
  }
}

#define POSIX_FADV_NORMAL 0
#define POSIX_FADV_RANDOM 1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED 3
#define POSIX_FADV_DONTNEED 4
#define POSIX_FADV_NOREUSE 5

// Access-pattern advice has no Windows equivalent for an open descriptor.
inline int posix_fadvise(int /*descriptor*/, off_t /*offset*/, off_t /*length*/,
                         int /*advice*/) {
  return 0;
}

// std::filesystem::path::c_str() is a wide string on Windows.
inline int open(const wchar_t* path, int flags,
                int mode = _S_IREAD | _S_IWRITE) {
  return _wopen(path, flags, mode);
}

#endif  // GUFO_WINDOWS_SHIM_FCNTL_H_
