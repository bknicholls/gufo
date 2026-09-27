#ifndef GUFO_WINDOWS_SHIM_SYS_MMAN_H_
#define GUFO_WINDOWS_SHIM_SYS_MMAN_H_

#include <io.h>
// clang-format off
// psapi.h needs the types windows.h declares, so the order is fixed.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <cerrno>
#include <cstddef>
#include <vector>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED (reinterpret_cast<void*>(-1))

#define MADV_NORMAL 0
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_HUGEPAGE 14
#define MADV_POPULATE_READ 22

inline void* mmap(void* /*address*/, std::size_t length, int protection,
                  int flags, int fd, long long offset) {
  if (length == 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if ((flags & MAP_ANONYMOUS) != 0) {
    const DWORD page_protection =
        (protection & PROT_WRITE) != 0 ? PAGE_READWRITE : PAGE_READONLY;
    void* const memory = VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT,
                                      page_protection);
    if (memory == nullptr) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    return memory;
  }

  const auto file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return MAP_FAILED;
  }
  // A private writable file mapping is copy-on-write, matching MAP_PRIVATE.
  const bool writable = (protection & PROT_WRITE) != 0;
  const DWORD page_protection = writable ? PAGE_WRITECOPY : PAGE_READONLY;
  const DWORD view_access = writable ? FILE_MAP_COPY : FILE_MAP_READ;
  HANDLE const mapping =
      CreateFileMappingW(file, nullptr, page_protection, 0, 0, nullptr);
  if (mapping == nullptr) {
    errno = EACCES;
    return MAP_FAILED;
  }
  const auto unsigned_offset = static_cast<unsigned long long>(offset);
  void* const view = MapViewOfFile(
      mapping, view_access, static_cast<DWORD>(unsigned_offset >> 32),
      static_cast<DWORD>(unsigned_offset & 0xFFFFFFFFULL), length);
  // The view holds its own reference to the section.
  CloseHandle(mapping);
  if (view == nullptr) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return view;
}

inline int munmap(void* address, std::size_t /*length*/) {
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(address, &info, sizeof(info)) == 0) {
    errno = EINVAL;
    return -1;
  }
  const BOOL released = info.Type == MEM_MAPPED
                            ? UnmapViewOfFile(address)
                            : VirtualFree(address, 0, MEM_RELEASE);
  if (!released) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

inline int madvise(void* address, std::size_t length, int advice) {
  if (advice == MADV_POPULATE_READ || advice == MADV_WILLNEED) {
    WIN32_MEMORY_RANGE_ENTRY range{address, length};
    (void)PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
    // PrefetchVirtualMemory is only a hint; callers of MADV_POPULATE_READ
    // expect every page resident when this returns, so fault them in.
    constexpr std::size_t kPageBytes = 4096;
    const auto* bytes = static_cast<const volatile unsigned char*>(address);
    for (std::size_t offset = 0; offset < length; offset += kPageBytes)
      (void)bytes[offset];
    return 0;
  }
  if (advice == MADV_DONTNEED) {
    // On unlocked pages VirtualUnlock trims them from the working set without
    // discarding contents, the closest Windows analogue for a read-only view.
    (void)VirtualUnlock(address, length);
    return 0;
  }
  return 0;
}

// Page residency from the process working set: a page is resident when the
// working set holds a valid entry for it.
inline int mincore(void* address, std::size_t length, unsigned char* states) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const std::size_t page = info.dwPageSize;
  const std::size_t pages = (length + page - 1) / page;
  std::vector<PSAPI_WORKING_SET_EX_INFORMATION> entries(pages);
  for (std::size_t index = 0; index < pages; ++index)
    entries[index].VirtualAddress = static_cast<char*>(address) + index * page;
  if (!QueryWorkingSetEx(
          GetCurrentProcess(), entries.data(),
          static_cast<DWORD>(entries.size() * sizeof(entries[0])))) {
    errno = ENOMEM;
    return -1;
  }
  for (std::size_t index = 0; index < pages; ++index)
    states[index] = entries[index].VirtualAttributes.Valid ? 1 : 0;
  return 0;
}

#endif  // GUFO_WINDOWS_SHIM_SYS_MMAN_H_
