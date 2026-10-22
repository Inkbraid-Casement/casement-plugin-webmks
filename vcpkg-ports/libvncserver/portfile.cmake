# LibVNCServer 0.9.15 for the MSVC build, configured like build_libvncserver
# in ci/mingw-deps.sh: zlib and libjpeg-turbo, no TLS, SASL, PNG, LZO,
# threads or examples.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO LibVNC/libvncserver
    REF LibVNCServer-0.9.15
    SHA512 3ad7e14eef3b591574714e320257ac465778e05bd157ddff09e48b990f35890bfa6883ce4ac027fcb08dccd96f721117d56aaee681482f7643cfee9adc59804b
    HEAD_REF master
    PATCHES
        # Without threads, MUTEX() expands to nothing and leaves a bare ";"
        # in struct _rfbClient, which MSVC's C compiler rejects.
        client-mutex-without-threads.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # The project asks for CMake 3.4, which CMake 4 no longer accepts.
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DWITH_ZLIB=ON
        -DWITH_JPEG=ON
        -DWITH_PNG=OFF
        -DWITH_LZO=OFF
        -DWITH_SDL=OFF
        -DWITH_GTK=OFF
        -DWITH_QT=OFF
        -DWITH_LIBSSHTUNNEL=OFF
        -DWITH_THREADS=OFF
        -DWITH_GNUTLS=OFF
        -DWITH_OPENSSL=OFF
        -DWITH_GCRYPT=OFF
        -DWITH_SYSTEMD=OFF
        -DWITH_FFMPEG=OFF
        -DWITH_TIGHTVNC_FILETRANSFER=OFF
        -DWITH_WEBSOCKETS=OFF
        -DWITH_SASL=OFF
        -DWITH_XCB=OFF
        -DWITH_EXAMPLES=OFF
        -DWITH_TESTS=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/LibVNCServer)
# The plugin finds it through CMake only.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/lib/pkgconfig" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
