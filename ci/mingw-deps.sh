#!/usr/bin/env bash
# Builds third-party libraries for 64-bit Windows with MinGW-w64, as static
# libraries in a prefix, for the cross builds on Linux (CI and local):
#
#   ci/mingw-deps.sh <prefix> zlib openssl ...
#
# Each library is built once: a prefix that already has it is left alone.
# Sources are downloaded from their projects and checked against pinned
# SHA-256 sums.
set -euo pipefail

PREFIX=$(realpath -m "$1")
shift
HOST=x86_64-w64-mingw32
JOBS=$(nproc)
WORK=${MINGW_DEPS_WORK:-$(mktemp -d)}
mkdir -p "$PREFIX" "$WORK"

fetch() {  # url sha256 file
    local url=$1 sum=$2 file=$WORK/$3
    if [ ! -f "$file" ]; then curl -sSLf --retry 3 -o "$file" "$url"; fi
    echo "$sum  $file" | sha256sum -c --quiet -
}

have() { [ -f "$PREFIX/.built-$1" ]; }
done_() { touch "$PREFIX/.built-$1"; }

build_zlib() {
    have zlib && return
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
        9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib.tar.gz
    rm -rf "$WORK/zlib" && mkdir -p "$WORK/zlib" && tar -xzf "$WORK/zlib.tar.gz" -C "$WORK/zlib" --strip-components=1
    (cd "$WORK/zlib" && make -f win32/Makefile.gcc PREFIX=$HOST- -j"$JOBS" libz.a &&
        make -f win32/Makefile.gcc PREFIX=$HOST- \
            INCLUDE_PATH="$PREFIX/include" LIBRARY_PATH="$PREFIX/lib" BINARY_PATH="$PREFIX/bin" install)
    done_ zlib
}

build_openssl() {
    have openssl && return
    fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz \
        967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99 openssl.tar.gz
    rm -rf "$WORK/openssl" && mkdir -p "$WORK/openssl" && tar -xzf "$WORK/openssl.tar.gz" -C "$WORK/openssl" --strip-components=1
    (cd "$WORK/openssl" &&
        ./Configure mingw64 --cross-compile-prefix=$HOST- --prefix="$PREFIX" --libdir=lib \
            no-shared no-tests no-docs no-apps no-module &&
        make -j"$JOBS" build_libs >/dev/null && make install_dev >/dev/null)
    rm -rf "$WORK/openssl"
    done_ openssl
}

# A toolchain file for libraries built with CMake: MinGW-w64 compilers, and
# headers, libraries and packages only from the prefix and MinGW itself.
cmake_toolchain() {
    cat > "$WORK/mingw-toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER $HOST-gcc-posix)
set(CMAKE_CXX_COMPILER $HOST-g++-posix)
set(CMAKE_RC_COMPILER $HOST-windres)
set(CMAKE_FIND_ROOT_PATH "$PREFIX" /usr/$HOST)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
    echo "$WORK/mingw-toolchain.cmake"
}

# libjpeg only (no TurboJPEG library or tools): LibVNCClient brings its own
# TurboJPEG wrapper around the libjpeg API.
build_libjpeg_turbo() {
    have libjpeg_turbo && return
    fetch https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.2.0.tar.gz \
        980dd81f425082aa6d7c9e47fef27554ce7a9ffc8e2f6e863b97d263c5c50858 libjpeg-turbo.tar.gz
    rm -rf "$WORK/libjpeg-turbo" && mkdir -p "$WORK/libjpeg-turbo" &&
        tar -xzf "$WORK/libjpeg-turbo.tar.gz" -C "$WORK/libjpeg-turbo" --strip-components=1
    cmake -S "$WORK/libjpeg-turbo" -B "$WORK/libjpeg-turbo/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$(cmake_toolchain)" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF \
        -DWITH_TESTS=OFF -DWITH_SIMD=ON >/dev/null
    cmake --build "$WORK/libjpeg-turbo/build" -j"$JOBS" >/dev/null
    cmake --install "$WORK/libjpeg-turbo/build" >/dev/null
    rm -rf "$WORK/libjpeg-turbo"
    done_ libjpeg_turbo
}

# LibVNCClient (and LibVNCServer, which the same build makes) with zlib and
# libjpeg-turbo for the Tight, ZRLE and zlib encodings, and nothing that
# would pull in further libraries: no TLS, SASL, PNG, LZO or examples. Its
# CMakeLists asks for CMake 3.4, which CMake 4 refuses without the policy
# minimum.
build_libvncserver() {
    have libvncserver && return
    build_zlib
    build_libjpeg_turbo
    fetch https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-0.9.15.tar.gz \
        62352c7795e231dfce044beb96156065a05a05c974e5de9e023d688d8ff675d7 libvncserver.tar.gz
    rm -rf "$WORK/libvncserver" && mkdir -p "$WORK/libvncserver" &&
        tar -xzf "$WORK/libvncserver.tar.gz" -C "$WORK/libvncserver" --strip-components=1
    cmake -S "$WORK/libvncserver" -B "$WORK/libvncserver/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$(cmake_toolchain)" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DBUILD_SHARED_LIBS=OFF -DWITH_ZLIB=ON -DWITH_JPEG=ON -DWITH_PNG=OFF -DWITH_LZO=OFF \
        -DWITH_SDL=OFF -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_LIBSSHTUNNEL=OFF -DWITH_THREADS=OFF \
        -DWITH_GNUTLS=OFF -DWITH_OPENSSL=OFF -DWITH_GCRYPT=OFF -DWITH_SYSTEMD=OFF -DWITH_FFMPEG=OFF \
        -DWITH_TIGHTVNC_FILETRANSFER=OFF -DWITH_WEBSOCKETS=OFF -DWITH_SASL=OFF -DWITH_XCB=OFF \
        -DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF >/dev/null
    cmake --build "$WORK/libvncserver/build" -j"$JOBS" >/dev/null
    cmake --install "$WORK/libvncserver/build" >/dev/null
    rm -rf "$WORK/libvncserver"
    done_ libvncserver
}

for lib in "$@"; do
    "build_$lib"
done
