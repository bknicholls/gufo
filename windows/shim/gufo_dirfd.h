#ifndef GUFO_WINDOWS_SHIM_GUFO_DIRFD_H_
#define GUFO_WINDOWS_SHIM_GUFO_DIRFD_H_

// POSIX directory-descriptor file API (open O_DIRECTORY, openat, fstatat,
// renameat, unlinkat, utimensat) over Win32 paths. Include it after every
// other header of the one translation unit that needs it: it redefines
// `open`, `close`, `read`, `write` and `fsync` for the rest of that file.

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x01000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x02000000
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

namespace gufo_dirfd {

// Directory descriptors are handed out above the CRT's descriptor range so the
// two can never collide; the CRT aborts on a close of a descriptor it does not
// own, so every close is routed through CloseDescriptor below.
constexpr int kFirstDirectoryDescriptor = 0x40000000;

// The CRT takes 32-bit byte counts; larger requests are split by the callers'
// partial-transfer loops.
constexpr std::size_t kMaxTransferBytes = std::size_t{1} << 30;

struct Registry {
  std::mutex mutex;
  std::unordered_map<int, std::filesystem::path> paths;
  int next{kFirstDirectoryDescriptor};
};

inline Registry& Directories() {
  static Registry registry;
  return registry;
}

inline bool IsDirectoryDescriptor(int descriptor) {
  return descriptor >= kFirstDirectoryDescriptor;
}

inline bool DirectoryPath(int descriptor, std::filesystem::path* path) {
  auto& registry = Directories();
  const std::lock_guard lock(registry.mutex);
  const auto found = registry.paths.find(descriptor);
  if (found == registry.paths.end()) {
    errno = EBADF;
    return false;
  }
  *path = found->second;
  return true;
}

inline bool EntryPath(int directory, const char* name,
                      std::filesystem::path* path) {
  std::filesystem::path base;
  if (!DirectoryPath(directory, &base))
    return false;
  *path = base / std::filesystem::path(name);
  return true;
}

inline void SetErrnoFromLastError() {
  const DWORD error = GetLastError();
  errno =
      error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT
      : error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS  ? EEXIST
                                                                     : EACCES;
}

// Windows grants access through the directory ACL rather than mode bits, and
// MSVC copies the owner bits into the group and other positions, which POSIX
// callers read as a world-accessible file. Report owner-only bits, and report
// reparse points (symlinks, junctions) as links so no-follow checks still hold.
inline int StatPath(const std::filesystem::path& path, struct _stat64* status) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    SetErrnoFromLastError();
    return -1;
  }
  if (_wstat64(path.c_str(), status) != 0)
    return -1;
  unsigned short type = _S_IFREG;
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    type = S_IFLNK;
  else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    type = _S_IFDIR;
  unsigned short permissions = (attributes & FILE_ATTRIBUTE_READONLY) != 0
                                   ? S_IRUSR
                                   : (S_IRUSR | S_IWUSR);
  if (type == _S_IFDIR)
    permissions |= S_IXUSR;
  status->st_mode = static_cast<unsigned short>(type | permissions);
  status->st_uid = 0;
  return 0;
}

inline int OpenPath(const std::filesystem::path& path, int flags, int mode) {
  if ((flags & O_NOFOLLOW) != 0) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      errno = ELOOP;
      return -1;
    }
  }
  const int native_flags =
      (flags & ~(O_DIRECTORY | O_NOFOLLOW)) | _O_BINARY | _O_NOINHERIT;
  const int native_mode = ((mode & S_IRUSR) != 0 ? _S_IREAD : 0) |
                          ((mode & S_IWUSR) != 0 ? _S_IWRITE : 0);
  return _wopen(path.c_str(), native_flags,
                native_mode != 0 ? native_mode : (_S_IREAD | _S_IWRITE));
}

}  // namespace gufo_dirfd

inline unsigned geteuid() {
  return 0;
}

inline int lstat(const wchar_t* path, struct _stat64* status) {
  return gufo_dirfd::StatPath(path, status);
}

inline int chmod(const wchar_t* /*path*/, int /*mode*/) {
  return 0;
}

inline int gufo_dirfd_open(const wchar_t* path, int flags, int mode = 0) {
  if ((flags & O_DIRECTORY) == 0)
    return gufo_dirfd::OpenPath(path, flags, mode);
  const DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    gufo_dirfd::SetErrnoFromLastError();
    return -1;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      ((flags & O_NOFOLLOW) != 0 &&
       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
    errno = ENOTDIR;
    return -1;
  }
  auto& registry = gufo_dirfd::Directories();
  const std::lock_guard lock(registry.mutex);
  const int descriptor = registry.next++;
  registry.paths.emplace(descriptor, std::filesystem::path(path));
  return descriptor;
}

inline int openat(int directory, const char* name, int flags, int mode = 0) {
  std::filesystem::path path;
  if (!gufo_dirfd::EntryPath(directory, name, &path))
    return -1;
  return gufo_dirfd::OpenPath(path, flags, mode);
}

inline int fstatat(int directory, const char* name, struct _stat64* status,
                   int /*flags*/) {
  std::filesystem::path path;
  if (!gufo_dirfd::EntryPath(directory, name, &path))
    return -1;
  return gufo_dirfd::StatPath(path, status);
}

inline int unlinkat(int directory, const char* name, int /*flags*/) {
  std::filesystem::path path;
  if (!gufo_dirfd::EntryPath(directory, name, &path))
    return -1;
  return _wunlink(path.c_str());
}

inline int renameat(int from_directory, const char* from, int to_directory,
                    const char* to) {
  std::filesystem::path source;
  std::filesystem::path target;
  if (!gufo_dirfd::EntryPath(from_directory, from, &source) ||
      !gufo_dirfd::EntryPath(to_directory, to, &target))
    return -1;
  // Write-through makes the replacement durable when this returns, which is
  // what the POSIX caller's directory fsync after renameat provides.
  if (!MoveFileExW(source.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    gufo_dirfd::SetErrnoFromLastError();
    return -1;
  }
  return 0;
}

// Only the "set both times to now" form (null times) is supported.
inline int utimensat(int directory, const char* name, const void* /*times*/,
                     int /*flags*/) {
  std::filesystem::path path;
  if (!gufo_dirfd::EntryPath(directory, name, &path))
    return -1;
  HANDLE file = CreateFileW(
      path.c_str(), FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    gufo_dirfd::SetErrnoFromLastError();
    return -1;
  }
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const BOOL updated = SetFileTime(file, nullptr, &now, &now);
  CloseHandle(file);
  return updated ? 0 : -1;
}

inline int gufo_close(int descriptor) {
  if (gufo_dirfd::IsDirectoryDescriptor(descriptor)) {
    auto& registry = gufo_dirfd::Directories();
    const std::lock_guard lock(registry.mutex);
    return registry.paths.erase(descriptor) == 1 ? 0 : (errno = EBADF, -1);
  }
  return _close(descriptor);
}

inline int gufo_read(int descriptor, void* data, std::size_t size) {
  return _read(
      descriptor, data,
      static_cast<unsigned>(std::min(size, gufo_dirfd::kMaxTransferBytes)));
}

inline int gufo_write(int descriptor, const void* data, std::size_t size) {
  return _write(
      descriptor, data,
      static_cast<unsigned>(std::min(size, gufo_dirfd::kMaxTransferBytes)));
}

// Directory metadata is journaled by NTFS and renames above are write-through,
// so a directory sync has nothing further to flush.
inline int gufo_fsync(int descriptor) {
  if (gufo_dirfd::IsDirectoryDescriptor(descriptor))
    return 0;
  return _commit(descriptor);
}

// Directory descriptors exist only in this translation unit, so its open and
// descriptor calls route through the emulation above.
#define open(...) gufo_dirfd_open(__VA_ARGS__)
#define close(descriptor) gufo_close(descriptor)
#define read(descriptor, data, size) gufo_read(descriptor, data, size)
#define write(descriptor, data, size) gufo_write(descriptor, data, size)
#define fsync(descriptor) gufo_fsync(descriptor)

#endif  // GUFO_WINDOWS_SHIM_GUFO_DIRFD_H_
