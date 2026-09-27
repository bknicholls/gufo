#ifndef GUFO_WINDOWS_SHIM_SYS_STAT_H_
#define GUFO_WINDOWS_SHIM_SYS_STAT_H_

#include_next <sys/stat.h>

// The CRT's struct stat reports a 32-bit st_size, which cannot describe model
// files over 2 GiB. Map the POSIX names onto the 64-bit variants.
#define stat _stat64
#define fstat _fstat64

// std::filesystem::path::c_str() is a wide string on Windows.
inline int _stat64(const wchar_t* path, struct _stat64* status) {
  return _wstat64(path, status);
}

#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define S_ISLNK(mode) (((mode) & _S_IFMT) == S_IFLNK)
#endif

#endif  // GUFO_WINDOWS_SHIM_SYS_STAT_H_
