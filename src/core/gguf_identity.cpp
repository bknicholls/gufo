#include "src/core/gguf_identity.hpp"

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/core/crypto/sha256.hpp"

namespace gufo::core {
namespace {

void HashString(crypto::Sha256Hasher& hash, std::string_view value) {
  hash.Update(
      {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

#if defined(_WIN32)

HANDLE FileHandle(int fd) {
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE)
    throw std::runtime_error("GGUF file changed or cannot be inspected");
  return handle;
}

std::uint64_t FileTimeTicks(const FILETIME& time) {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
         time.dwLowDateTime;
}

// Volume serial plus file index identify the file the way st_dev plus st_ino
// do; the write and change times detect in-place modification.
std::string FileStamp(int fd, std::size_t size) {
  const HANDLE handle = FileHandle(fd);
  BY_HANDLE_FILE_INFORMATION information{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandle(handle, &information) ||
      !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                    sizeof(basic)) ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    throw std::runtime_error("GGUF file changed or cannot be inspected");
  }
  const auto file_size =
      (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
      information.nFileSizeLow;
  if (file_size != size)
    throw std::runtime_error("GGUF file changed or cannot be inspected");
  const auto file_index =
      (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
      information.nFileIndexLow;
  return std::to_string(information.dwVolumeSerialNumber) + ':' +
         std::to_string(file_index) + ':' + std::to_string(file_size) + ':' +
         std::to_string(FileTimeTicks(information.ftLastWriteTime)) + ':' +
         std::to_string(basic.ChangeTime.QuadPart);
}

// Local AppData is already private to the user, which is what the POSIX
// owner and permission-bit checks establish for the cache directory.
std::filesystem::path CacheDirectory() {
  const char* base = std::getenv("LOCALAPPDATA");
  if (base == nullptr || *base == '\0')
    return {};
  auto path = std::filesystem::path(base) / "gufo" / "gguf-sha256-v1";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return error ? std::filesystem::path{} : path;
}

std::string ReadDigest(const std::filesystem::path& directory,
                       const std::string& key) {
  if (directory.empty())
    return {};
  const auto path = directory / key;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) ||
      std::filesystem::file_size(path, error) != 65 || error)
    return {};
  std::ifstream input(path, std::ios::binary);
  std::array<char, 65> bytes{};
  if (!input.read(bytes.data(), bytes.size()) || bytes.back() != '\n' ||
      !std::all_of(bytes.begin(), bytes.end() - 1, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    return {};
  return {bytes.data(), bytes.size() - 1};
}

void StoreDigest(const std::filesystem::path& directory, const std::string& key,
                 const std::string& digest) {
  if (directory.empty())
    return;
  static std::atomic<unsigned long> serial{0};
  const auto temporary =
      directory / ("." + key + "." + std::to_string(_getpid()) + "." +
                   std::to_string(serial.fetch_add(1)));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << digest << '\n';
    if (!output.flush())
      return;
  }
  std::error_code error;
  std::filesystem::rename(temporary, directory / key, error);
  std::filesystem::remove(temporary, error);
}

void HashFile(crypto::Sha256Hasher& hash, int fd, std::size_t size) {
  constexpr std::size_t kChunk = 8 * 1024 * 1024;
  const HANDLE handle = FileHandle(fd);
  // Positional reads leave the shared descriptor's file pointer irrelevant,
  // and one read in flight overlaps disk time with hashing.
  const auto read_chunk = [handle](std::uint8_t* buffer, std::size_t offset,
                                   std::size_t length) {
    std::size_t done = 0;
    while (done < length) {
      OVERLAPPED position{};
      const auto at = static_cast<std::uint64_t>(offset + done);
      position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFULL);
      position.OffsetHigh = static_cast<DWORD>(at >> 32);
      DWORD count = 0;
      if (!ReadFile(handle, buffer + done, static_cast<DWORD>(length - done),
                    &count, &position) ||
          count == 0)
        throw std::runtime_error("cannot read complete GGUF for identity");
      done += count;
    }
  };
  std::array<std::vector<std::uint8_t>, 2> buffers{
      std::vector<std::uint8_t>(kChunk), std::vector<std::uint8_t>(kChunk)};
  std::size_t offset = 0;
  std::size_t current_length = std::min(kChunk, size);
  if (current_length != 0)
    read_chunk(buffers[0].data(), 0, current_length);
  for (std::size_t index = 0; current_length != 0; index ^= 1) {
    const std::size_t next_offset = offset + current_length;
    const std::size_t next_length = std::min(kChunk, size - next_offset);
    std::future<void> next_read;
    if (next_length != 0)
      next_read =
          std::async(std::launch::async, read_chunk, buffers[index ^ 1].data(),
                     next_offset, next_length);
    hash.Update({buffers[index].data(), current_length});
    if (next_read.valid())
      next_read.get();
    offset = next_offset;
    current_length = next_length;
  }
}

std::string RegionDigest(const GgufMappedRegion& region) {
  std::string stamp;
  if (region.file_descriptor >= 0)
    stamp = FileStamp(region.file_descriptor, region.size);
  const auto cache = stamp.empty() ? std::filesystem::path{} : CacheDirectory();
  crypto::Sha256Hasher key_hash;
  HashString(key_hash, stamp);
  const auto key = key_hash.FinishHex();
  std::string digest = ReadDigest(cache, key);
  const bool cached = !digest.empty();
  if (!cached) {
    crypto::Sha256Hasher hash;
    constexpr std::size_t chunk = 8 * 1024 * 1024;
    if (region.file_descriptor >= 0) {
      HashFile(hash, region.file_descriptor, region.size);
    } else {
      const auto* bytes = static_cast<const std::uint8_t*>(region.data);
      for (std::size_t offset = 0; offset < region.size;) {
        const auto length = std::min(chunk, region.size - offset);
        hash.Update({bytes + offset, length});
        offset += length;
      }
    }
    digest = hash.FinishHex();
  }
  if (!stamp.empty() &&
      FileStamp(region.file_descriptor, region.size) != stamp) {
    throw std::runtime_error("GGUF file changed during identity lookup");
  }
  if (!cached)
    StoreDigest(cache, key, digest);
  return digest;
}

#else

class FileDescriptor {
public:
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0)
      close(fd_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  [[nodiscard]] int get() const { return fd_; }

private:
  int fd_;
};

std::string FileStamp(int fd, std::size_t size) {
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      static_cast<std::size_t>(st.st_size) != size) {
    throw std::runtime_error("GGUF file changed or cannot be inspected");
  }
  return std::to_string(st.st_dev) + ':' + std::to_string(st.st_ino) + ':' +
         std::to_string(st.st_size) + ':' + std::to_string(st.st_mtim.tv_sec) +
         ':' + std::to_string(st.st_mtim.tv_nsec) + ':' +
         std::to_string(st.st_ctim.tv_sec) + ':' +
         std::to_string(st.st_ctim.tv_nsec);
}

int OpenCacheDirectory() {
  const char* base = std::getenv("XDG_CACHE_HOME");
  std::filesystem::path path;
  if (base && *base)
    path = base;
  else {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
      return -1;
    path = std::filesystem::path(home) / ".cache";
  }
  path /= "gufo";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error)
    return -1;
  path /= "gguf-sha256-v1";
  (void)mkdir(path.c_str(), 0700);
  int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return -1;
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_uid != geteuid() ||
      (st.st_mode & 0077) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

std::string ReadDigest(int directory, const std::string& key) {
  if (directory < 0)
    return {};
  FileDescriptor file(
      openat(directory, key.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat st{};
  if (file.get() < 0 || fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0022) != 0 || st.st_size != 65)
    return {};
  std::array<char, 65> bytes{};
  if (read(file.get(), bytes.data(), bytes.size()) !=
          static_cast<ssize_t>(bytes.size()) ||
      bytes.back() != '\n' ||
      !std::all_of(bytes.begin(), bytes.end() - 1, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    return {};
  return {bytes.data(), bytes.size() - 1};
}

void StoreDigest(int directory, const std::string& key,
                 const std::string& digest) {
  if (directory < 0)
    return;
  static std::atomic<unsigned long> serial{0};
  const std::string temp = "." + key + "." + std::to_string(getpid()) + "." +
                           std::to_string(serial.fetch_add(1));
  FileDescriptor file(
      openat(directory, temp.c_str(),
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (file.get() < 0)
    return;
  const std::string contents = digest + '\n';
  if (write(file.get(), contents.data(), contents.size()) ==
      static_cast<ssize_t>(contents.size())) {
    (void)renameat(directory, temp.c_str(), directory, key.c_str());
  }
  (void)unlinkat(directory, temp.c_str(), 0);
}

void HashFile(crypto::Sha256Hasher& hash, int fd, std::size_t size) {
  constexpr std::size_t kChunk = 8 * 1024 * 1024;
  constexpr std::size_t kAlignment = 4096;
  constexpr std::size_t kReaders = 4;
  // Preserve the open inode and avoid filling the remaining UMA with a second
  // copy of weights after GPU loading. Hashing stays in exact file order.
  FileDescriptor direct(
      size >= kChunk ? open(("/proc/self/fd/" + std::to_string(fd)).c_str(),
                            O_RDONLY | O_CLOEXEC | O_DIRECT)
                     : -1);
  struct Slot {
    std::unique_ptr<std::uint8_t, decltype(&std::free)> buffer{nullptr,
                                                               std::free};
    std::future<void> read;
    std::size_t size{0};
  };
  std::array<Slot, kReaders> slots;
  std::size_t next = 0;
  const auto queue = [&](Slot& slot) {
    if (next == size)
      return;
    if (!slot.buffer) {
      slot.buffer.reset(
          static_cast<std::uint8_t*>(std::aligned_alloc(kAlignment, kChunk)));
      if (!slot.buffer)
        throw std::bad_alloc();
    }
    const auto offset = next;
    slot.size = std::min(kChunk, size - next);
    next += slot.size;
    slot.read = std::async(std::launch::async, [&, offset] {
      std::size_t done = 0;
      bool buffered = direct.get() < 0;
      while (done < slot.size) {
        const auto remaining = slot.size - done;
        const auto requested =
            buffered ? remaining
                     : (remaining + kAlignment - 1) / kAlignment * kAlignment;
        const auto count =
            pread(buffered ? fd : direct.get(), slot.buffer.get() + done,
                  requested, static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR)
          continue;
        if (count < 0 && !buffered &&
            (errno == EINVAL || errno == EOPNOTSUPP)) {
          buffered = true;
          continue;
        }
        if (count <= 0)
          throw std::runtime_error("cannot read complete GGUF for identity");
        done += std::min(static_cast<std::size_t>(count), remaining);
        if (done % kAlignment != 0)
          buffered = true;
      }
    });
  };
  for (auto& slot : slots)
    queue(slot);
  for (std::size_t index = 0; slots[index].read.valid();
       index = (index + 1) % slots.size()) {
    auto& slot = slots[index];
    slot.read.get();
    hash.Update({slot.buffer.get(), slot.size});
    queue(slot);
  }
}

std::string RegionDigest(const GgufMappedRegion& region) {
  std::string stamp;
  if (region.file_descriptor >= 0)
    stamp = FileStamp(region.file_descriptor, region.size);
  FileDescriptor cache(stamp.empty() ? -1 : OpenCacheDirectory());
  crypto::Sha256Hasher key_hash;
  HashString(key_hash, stamp);
  const auto key = key_hash.FinishHex();
  std::string digest = ReadDigest(cache.get(), key);
  const bool cached = !digest.empty();
  if (!cached) {
    crypto::Sha256Hasher hash;
    constexpr std::size_t chunk = 8 * 1024 * 1024;
    if (region.file_descriptor >= 0) {
      HashFile(hash, region.file_descriptor, region.size);
    } else {
      const auto* bytes = static_cast<const std::uint8_t*>(region.data);
      for (std::size_t offset = 0; offset < region.size;) {
        const auto length = std::min(chunk, region.size - offset);
        hash.Update({bytes + offset, length});
        offset += length;
      }
    }
    digest = hash.FinishHex();
  }
  if (!stamp.empty() &&
      FileStamp(region.file_descriptor, region.size) != stamp) {
    throw std::runtime_error("GGUF file changed during identity lookup");
  }
  if (!cached)
    StoreDigest(cache.get(), key, digest);
  return digest;
}

#endif

}  // namespace

std::string GgufIdentityHex(const GgufReader& reader) {
  std::vector<std::string> stamps;
  for (const auto& region : reader.GetMappedRegions())
    stamps.push_back(region.file_descriptor < 0
                         ? std::string{}
                         : FileStamp(region.file_descriptor, region.size));
  crypto::Sha256Hasher hash;
  HashString(hash, kGgufIdentityScheme);
  for (const auto& region : reader.GetMappedRegions()) {
    HashString(hash, ":" + std::to_string(region.size) + ":");
    HashString(hash, RegionDigest(region));
  }
  for (std::size_t index = 0; index < stamps.size(); ++index) {
    const auto& region = reader.GetMappedRegions()[index];
    if (!stamps[index].empty() &&
        stamps[index] != FileStamp(region.file_descriptor, region.size))
      throw std::runtime_error("GGUF artifact changed during identity lookup");
  }
  return hash.FinishHex();
}

}  // namespace gufo::core
