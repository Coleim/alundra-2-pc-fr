# Cross-compile toolchain: Linux host (Debian/Ubuntu mingw-w64 packages) -> Windows x86_64.
#
#   sudo apt-get install mingw-w64
#   cmake -S . -B build-win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "MinGW target triple")
set(CMAKE_C_COMPILER   "${MINGW_TRIPLE}-gcc-posix")
set(CMAKE_CXX_COMPILER "${MINGW_TRIPLE}-g++-posix")
if(NOT EXISTS "/usr/bin/${CMAKE_C_COMPILER}")
    set(CMAKE_C_COMPILER   "${MINGW_TRIPLE}-gcc")
    set(CMAKE_CXX_COMPILER "${MINGW_TRIPLE}-g++")
endif()
set(CMAKE_RC_COMPILER "${MINGW_TRIPLE}-windres")
set(CMAKE_FIND_ROOT_PATH "/usr/${MINGW_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
