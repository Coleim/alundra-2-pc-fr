# Cross-compile toolchain: macOS host (Homebrew mingw-w64) -> Windows x86_64.
#
#   brew install mingw-w64
#   cmake -S . -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-macos.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "MinGW target triple")

set(CMAKE_C_COMPILER   "${MINGW_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${MINGW_TRIPLE}-g++")
set(CMAKE_RC_COMPILER  "${MINGW_TRIPLE}-windres")
set(CMAKE_RANLIB       "${MINGW_TRIPLE}-ranlib")
set(CMAKE_AR           "${MINGW_TRIPLE}-ar")
set(CMAKE_STRIP        "${MINGW_TRIPLE}-strip")

execute_process(COMMAND "${MINGW_TRIPLE}-gcc" -print-sysroot
                OUTPUT_VARIABLE _mingw_sysroot
                OUTPUT_STRIP_TRAILING_WHITESPACE)

# Keep pkg-config away from the host's Homebrew .pc files.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${_mingw_sysroot}/${MINGW_TRIPLE}/lib/pkgconfig")

set(CMAKE_FIND_ROOT_PATH "${_mingw_sysroot}/${MINGW_TRIPLE}" "${_mingw_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Large generated TUs can exceed COFF section limits.
string(APPEND CMAKE_C_FLAGS_INIT " -Wa,-mbig-obj")
string(APPEND CMAKE_CXX_FLAGS_INIT " -Wa,-mbig-obj")
