# Cross-compiling for 64-bit Windows with MinGW-w64 (GCC, POSIX threads),
# e.g. on Linux:
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
# Tests then run under Wine (ctest uses the emulator below).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32)
find_program(CMAKE_C_COMPILER NAMES ${MINGW_PREFIX}-gcc-posix ${MINGW_PREFIX}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${MINGW_PREFIX}-g++-posix ${MINGW_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${MINGW_PREFIX}-windres REQUIRED)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(WINE NAMES wine wine64)
if(WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE})
endif()
