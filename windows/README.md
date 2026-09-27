# gufo on native Windows

gufo builds on Windows 11 with the AMD HIP SDK for gfx1151 (Ryzen AI Max /
Radeon 8060S), from the same CMake project as Linux.

## Build

Requirements:

- Visual Studio with the **14.44** MSVC toolset. Newer STLs conflict with the
  clang HIP headers.
- AMD HIP SDK 7.2 at `C:\Program Files\AMD\ROCm\7.2` (or set `HIP_PATH`).
- A prebuilt ICU for MSVC x64.
- Static libpng, libjpeg-turbo, curl, OpenSSL and zlib built against the
  dynamic CRT. With vcpkg, and a triplet that pins
  `VCPKG_PLATFORM_TOOLSET_VERSION 14.44`:

  ```
  vcpkg install libpng libjpeg-turbo "curl[core,ssl]" openssl --triplet <x64-windows-static-md triplet>
  ```

  On Windows, curl's `ssl` feature selects Schannel.

```
set CMAKE_PREFIX_PATH=<vcpkg>\installed\<triplet>
windows\build.cmd -DICU_ROOT=<icu> -DZLIB_LIBRARY=<vcpkg>\installed\<triplet>\lib\zs.lib
```

vcpkg names its static zlib `zs.lib`, which CMake's zlib search does not look
for, hence `ZLIB_LIBRARY`. The build produces `build-windows\gufo.exe`. To run
it, add `C:\Program Files\AMD\ROCm\7.2\bin` and ICU's `bin64` to `PATH`, and
FFmpeg's `bin` for video generation.

Pass `-DBUILD_TESTING=ON` on the first configure for the tests that run on
Windows: the shim checks, the continuation disk store and FFmpeg muxing
(which needs FFmpeg on `PATH`). Run them with
`ctest --test-dir build-windows`. The rest of the suite relies on POSIX
process APIs and builds only on Linux.

## What differs from Linux

- **POSIX shims.** `shim/` maps the POSIX APIs gufo uses onto Win32 and the
  CRT, so the sources build unchanged.
  - `gufo_windows_prelude.h`, included ahead of every file, makes `off_t`
    64-bit, and `sys/stat.h` maps `stat` onto `_stat64`: the CRT's 32-bit
    forms truncate model files over 2 GiB.
  - `unistd.h`, `fcntl.h`, `time.h`, `signal.h`, `strings.h`, `sched.h`,
    `sys/file.h`, `sys/resource.h`, `sys/syscall.h` and `sys/utsname.h`
    supply the missing calls, including `pread`, `sigaction`,
    `clock_gettime` and wide-path `open` and `stat`.
  - `sys/mman.h`: `mmap`, `madvise` and `mincore` onto file views,
    `VirtualAlloc` and the working set.
  - `gufo_winsock.h` and the socket headers: BSD sockets onto Winsock.
  - `gufo_dirfd.h`: directory-descriptor file calls (`openat`, `renameat`,
    and similar) for the continuation disk store only.
- **Binary I/O.** Executables link `binmode.obj`, so CRT descriptors default to
  binary as POSIX descriptors do.
- **Weights live in device memory** (`GUFO_DEVICE_WEIGHTS=0` reverts to
  registered host memory). Windows HIP counts `hipHostRegister`ed memory twice
  in `hipMemGetInfo`, which makes the server's admission check refuse to load
  although the capacity exists.
- **Weights upload from the file**, read into pinned staging buffers and
  overlapped with the device copies. This is about 2.5× faster than copying
  from the mapped view, which faults in every 4 KiB page.
- **The listening socket closes in `stop()`**, because `shutdown()` does not
  wake `accept()` on Windows. The build also skips `SO_REUSEADDR`, which on
  Windows allows another socket to take over a port that is still in use.
- **FFmpeg runs through named pipes.** Video muxing passes audio to FFmpeg
  through a named pipe instead of an inherited descriptor.
- **`gufo diagnose`** reads Linux sysfs and procfs, so on Windows it reports
  the GPU but not the CPU or host memory.

Verified on hardware: the Qwen3.8 27B server, and FFmpeg muxing with its
test. DeepSeek V4 Flash and Qwen3.8 Flash-Next need more memory than a 64 GB
machine has; speech, image and video generation build but are not yet
verified on Windows.

## Memory

- Set `GPU_RESOURCE_CACHE_SIZE=0`. Otherwise HIP's Windows runtime keeps freed
  device memory cached, so each request's snapshot buffers ratchet the
  process up by several GB. With it set, the server stays at its post-load
  size plus one retained snapshot.
