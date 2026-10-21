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

for lib in "$@"; do
    "build_$lib"
done
