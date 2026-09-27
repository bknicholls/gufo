// Checks the POSIX shims the Windows build compiles gufo against, where a
// wrong mapping would fail silently: file offsets and sizes past 4 GiB, binary
// I/O, wide paths, and the process, time and memory queries.

#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <winioctl.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL windows_shim_test: " << message << '\n';
    std::exit(1);
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "gufo-shim-XXXXXX").string();
    Expect(mkdtemp(pattern.data()) != nullptr, "mkdtemp creates a directory");
    path_ = pattern;
    Expect(std::filesystem::is_directory(path_), "mkdtemp's directory exists");
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

void TestOffsetsAndSizesPastFourGiB(const TemporaryDirectory& directory) {
  static_assert(sizeof(off_t) == 8, "off_t must hold offsets past 2 GiB");
  const auto path = directory.path() / "sparse.bin";
  const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  Expect(fd >= 0, "wide-path open creates the file");
  // Sparse, so the far write does not allocate gigabytes of zeros.
  DWORD returned = 0;
  Expect(DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(fd)),
                         FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                         nullptr) != 0,
         "the test file becomes sparse");
  constexpr off_t kOffset = 5LL << 30;
  constexpr std::string_view kMarker = "far marker";
  Expect(pwrite(fd, kMarker.data(), kMarker.size(), kOffset) ==
             static_cast<ssize_t>(kMarker.size()),
         "pwrite writes past 4 GiB");
  std::array<char, kMarker.size()> read_back{};
  Expect(pread(fd, read_back.data(), read_back.size(), kOffset) ==
                 static_cast<ssize_t>(read_back.size()) &&
             std::string_view(read_back.data(), read_back.size()) == kMarker,
         "pread reads back past 4 GiB");
  Expect(pread(fd, read_back.data(), read_back.size(), kOffset + 64) == 0,
         "pread past the end reports end of file");
  const off_t expected_size = kOffset + static_cast<off_t>(kMarker.size());
  struct stat by_descriptor{};
  Expect(
      fstat(fd, &by_descriptor) == 0 && by_descriptor.st_size == expected_size,
      "fstat reports sizes past 4 GiB");
  Expect(ftruncate(fd, 4096) == 0 && fstat(fd, &by_descriptor) == 0 &&
             by_descriptor.st_size == 4096,
         "ftruncate shrinks the file");
  close(fd);
  struct stat by_path{};
  Expect(stat(path.c_str(), &by_path) == 0 && by_path.st_size == 4096 &&
             S_ISREG(by_path.st_mode),
         "wide-path stat reports the file");
}

void TestBinaryDescriptorsAndWidePaths(const TemporaryDirectory& directory) {
  const auto path = directory.path() / L"gufo-été.bin";
  const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  Expect(fd >= 0, "open accepts a non-ASCII wide path");
  constexpr std::string_view kText =
      "a\nb\x1a"
      "c";
  Expect(write(fd, kText.data(), static_cast<unsigned>(kText.size())) ==
             static_cast<int>(kText.size()),
         "write stores the bytes");
  const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  Expect(copy >= 0 && copy != fd, "F_DUPFD_CLOEXEC duplicates the descriptor");
  close(copy);
  Expect(dprintf(fd, "%d-%s", 42, "x") == 4, "dprintf writes formatted text");
  close(fd);
  const auto size = std::filesystem::file_size(path);
  Expect(size == kText.size() + 4,
         "descriptors are binary: no newline translation or end-of-file byte");
  Expect(posix_fadvise(0, 0, 0, POSIX_FADV_DONTNEED) == 0,
         "posix_fadvise is accepted");
}

void TestTime() {
  timespec first{};
  timespec second{};
  Expect(clock_gettime(CLOCK_MONOTONIC, &first) == 0 &&
             clock_gettime(CLOCK_MONOTONIC, &second) == 0,
         "the monotonic clock reads");
  Expect(second.tv_sec > first.tv_sec ||
             (second.tv_sec == first.tv_sec && second.tv_nsec >= first.tv_nsec),
         "the monotonic clock does not go backwards");
  Expect(first.tv_nsec >= 0 && first.tv_nsec < 1000000000,
         "nanoseconds stay within a second");
  timespec now{};
  Expect(
      clock_gettime(CLOCK_REALTIME, &now) == 0 &&
          std::llabs(static_cast<long long>(now.tv_sec - time(nullptr))) <= 1,
      "the realtime clock matches time()");
  const time_t epoch = 0;
  tm calendar{};
  Expect(gmtime_r(&epoch, &calendar) == &calendar && calendar.tm_year == 70 &&
             calendar.tm_mday == 1,
         "gmtime_r converts in the POSIX argument order");
}

void Ignore(int /*signal*/) {}

void TestSignalsAndProcessQueries() {
  struct sigaction handler{};
  handler.sa_handler = Ignore;
  sigemptyset(&handler.sa_mask);
  struct sigaction previous{};
  Expect(sigaction(SIGPIPE, &handler, &previous) == 0,
         "a SIGPIPE handler is accepted");
  Expect(sigaction(SIGINT, &handler, &previous) == 0,
         "a SIGINT handler installs");
  struct sigaction current{};
  Expect(sigaction(SIGINT, &previous, &current) == 0 &&
             current.sa_handler == Ignore,
         "restoring returns the installed handler");

  rusage usage{};
  Expect(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0,
         "getrusage reports the peak working set");
  utsname name{};
  Expect(uname(&name) == 0 && std::strcmp(name.sysname, "Windows") == 0 &&
             name.release[0] != '\0',
         "uname reports Windows and its version");
  Expect(syscall(SYS_memfd_create, "x", MFD_CLOEXEC) == -1 && errno == ENOSYS,
         "Linux system calls report ENOSYS");
  Expect(strcasecmp("GuFo", "gufo") == 0 && strncasecmp("abcX", "ABCy", 3) == 0,
         "case-insensitive comparisons");
}

void TestMemoryResidency() {
  const long page = sysconf(_SC_PAGESIZE);
  Expect(page > 0, "sysconf reports the page size");
  const std::size_t length = static_cast<std::size_t>(page) * 4;
  void* memory = mmap(nullptr, length, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Expect(memory != MAP_FAILED, "anonymous mmap allocates");
  static_cast<unsigned char*>(memory)[0] = 1;
  std::array<unsigned char, 4> resident{};
  Expect(mincore(memory, length, resident.data()) == 0 && resident[0] == 1,
         "mincore reports a touched page as resident");
  Expect(munmap(memory, length) == 0, "munmap releases the mapping");
}

}  // namespace

int main() {
  TemporaryDirectory directory;
  TestOffsetsAndSizesPastFourGiB(directory);
  TestBinaryDescriptorsAndWidePaths(directory);
  TestTime();
  TestSignalsAndProcessQueries();
  TestMemoryResidency();
  std::cout << "windows shim tests passed\n";
  return 0;
}
