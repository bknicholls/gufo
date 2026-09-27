@echo off
rem Configures and builds gufo natively on Windows into build-windows\ at the
rem repository root. Needs Visual Studio with the 14.44 MSVC toolset (newer
rem STLs break the HIP headers) and the AMD HIP SDK. Dependency prefixes come
rem from the CMAKE_PREFIX_PATH environment variable; other CMake cache
rem settings can be passed as arguments on the first configure.
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" -vcvars_ver=14.44 >nul || exit /b 1
if not defined HIP_PATH set "HIP_PATH=C:\Program Files\AMD\ROCm\7.2"
set "PATH=%HIP_PATH%\bin;%PATH%"
set "CMAKE_PREFIX_PATH=%HIP_PATH%;%CMAKE_PREFIX_PATH%"
set "SRC=%~dp0.."
set "OUT=%~dp0..\build-windows"
if not exist "%OUT%\build.ninja" (
  cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER="%HIP_PATH%/bin/clang.exe" ^
    -DCMAKE_CXX_COMPILER="%HIP_PATH%/bin/clang++.exe" ^
    -DCMAKE_HIP_COMPILER="%HIP_PATH%/bin/clang++.exe" ^
    -DGUFO_ENABLE_WARNINGS=OFF %* || exit /b 1
)
cmake --build "%OUT%"
