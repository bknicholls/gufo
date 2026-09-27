#ifndef GUFO_WINDOWS_SHIM_UNISTD_H_
#define GUFO_WINDOWS_SHIM_UNISTD_H_

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#define _SC_PAGESIZE 30
#define _SC_AVPHYS_PAGES 86

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

using ssize_t = std::ptrdiff_t;

// Positional reads and writes through the handle, which unlike lseek plus
// read leave the descriptor's shared offset alone, so concurrent callers do
// not race. A synchronous handle given an OVERLAPPED offset performs the I/O
// at that offset.
inline ssize_t pread(int fd, void* data, std::size_t size, off_t offset) {
  const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED position{};
  position.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFLL);
  position.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD count = 0;
  const auto request =
      static_cast<DWORD>(size < 0x7FFFFFFFULL ? size : 0x7FFFFFFFULL);
  if (!ReadFile(file, data, request, &count, &position)) {
    if (GetLastError() == ERROR_HANDLE_EOF)
      return 0;
    errno = EIO;
    return -1;
  }
  return static_cast<ssize_t>(count);
}

inline ssize_t pwrite(int fd, const void* data, std::size_t size,
                      off_t offset) {
  const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED position{};
  position.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFLL);
  position.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD count = 0;
  const auto request =
      static_cast<DWORD>(size < 0x7FFFFFFFULL ? size : 0x7FFFFFFFULL);
  if (!WriteFile(file, data, request, &count, &position)) {
    errno = EIO;
    return -1;
  }
  return static_cast<ssize_t>(count);
}

inline int ftruncate(int fd, off_t length) {
  const errno_t result = _chsize_s(fd, length);
  if (result != 0) {
    errno = result;
    return -1;
  }
  return 0;
}

inline int dprintf(int fd, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int length = std::vsnprintf(nullptr, 0, format, arguments);
  va_end(arguments);
  if (length < 0)
    return -1;
  std::string text(static_cast<std::size_t>(length), '\0');
  va_start(arguments, format);
  (void)std::vsnprintf(text.data(), text.size() + 1, format, arguments);
  va_end(arguments);
  return _write(fd, text.data(), static_cast<unsigned>(text.size()));
}

inline int fsync(int fd) {
  return _commit(fd);
}

inline int mkstemp(char* name_template) {
  if (_mktemp_s(name_template, std::strlen(name_template) + 1) != 0) {
    errno = EINVAL;
    return -1;
  }
  return _open(name_template, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
               _S_IREAD | _S_IWRITE);
}

inline char* mkdtemp(char* name_template) {
  if (_mktemp_s(name_template, std::strlen(name_template) + 1) != 0 ||
      _mkdir(name_template) != 0) {
    return nullptr;
  }
  return name_template;
}

inline long sysconf(int name) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  if (name == _SC_PAGESIZE)
    return static_cast<long>(info.dwPageSize);
  if (name == _SC_AVPHYS_PAGES) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
      return -1;
    return static_cast<long>(status.ullAvailPhys / info.dwPageSize);
  }
  return -1;
}

#endif  // GUFO_WINDOWS_SHIM_UNISTD_H_
