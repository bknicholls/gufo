#ifndef GUFO_WINDOWS_SHIM_SYS_UTSNAME_H_
#define GUFO_WINDOWS_SHIM_SYS_UTSNAME_H_

#include <stdio.h>
#include <string.h>
#include <windows.h>

struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
};

// GetVersionEx reports the version the executable is manifested for, not the
// running one, so the kernel version comes from ntdll's RtlGetVersion.
inline int uname(struct utsname* name) {
  memset(name, 0, sizeof(*name));
  strcpy_s(name->sysname, "Windows");
  DWORD length = sizeof(name->nodename);
  if (!GetComputerNameA(name->nodename, &length))
    strcpy_s(name->nodename, "unknown");
  using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  const auto rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
    snprintf(name->release, sizeof(name->release), "%lu.%lu",
             version.dwMajorVersion, version.dwMinorVersion);
    snprintf(name->version, sizeof(name->version), "build %lu",
             version.dwBuildNumber);
  }
  strcpy_s(name->machine, "x86_64");
  return 0;
}

#endif  // GUFO_WINDOWS_SHIM_SYS_UTSNAME_H_
