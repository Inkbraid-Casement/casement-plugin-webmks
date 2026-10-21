cmake_minimum_required(VERSION 3.24)
project(casement-plugin-webmks VERSION 0.1.0 LANGUAGES CXX)

# MSVC builds link vcpkg's static libraries (triplet x64-windows-static),
# so the C runtime is static too: one exe, no runtime DLLs to ship.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/casement.cmake)
enable_testing()

# Cross builds look for libraries only below CMAKE_FIND_ROOT_PATH; the
# prefix with the MinGW builds of LibVNCClient and its libraries is one.
if(CMAKE_CROSSCOMPILING)
    list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
endif()
find_package(LibVNCServer CONFIG REQUIRED)

# pugixml, built with the project's settings.
add_library(pugixml STATIC third_party/pugixml/pugixml.cpp)
target_include_directories(pugixml PUBLIC third_party/pugixml)
target_link_libraries(pugixml PRIVATE casement_options)

add_library(vmware STATIC
    src/vmware/certificate.cpp
    src/vmware/console.cpp
    src/vmware/fingerprint.cpp
    src/vmware/https.cpp
    src/vmware/keyboard.cpp
    src/vmware/rfb_wire.cpp
    src/vmware/session.cpp
    src/vmware/soap.cpp
    src/vmware/socket_pair.cpp
    src/vmware/target.cpp
    src/vmware/vsphere.cpp)
target_include_directories(vmware PUBLIC src)
target_link_libraries(vmware PUBLIC casement pugixml LibVNCServer::vncclient winhttp crypt32 bcrypt ws2_32)

casement_plugin_executable(casement-vmware src/main.cpp)
target_link_libraries(casement-vmware PRIVATE vmware)
target_compile_definitions(casement-vmware PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")

casement_test(unit_tests unit
    tests/unit/main.cpp
    tests/unit/fingerprint_tests.cpp
    tests/unit/keyboard_tests.cpp
    tests/unit/rfb_wire_tests.cpp
    tests/unit/soap_tests.cpp
    tests/unit/socket_pair_tests.cpp
    tests/unit/target_tests.cpp
    tests/unit/vsphere_tests.cpp)
target_link_libraries(unit_tests PRIVATE vmware)

casement_test(e2e_tests e2e tests/e2e/vmware_tests.cpp)
target_link_libraries(e2e_tests PRIVATE casement_sim casement winhttp crypt32)
target_compile_definitions(e2e_tests PRIVATE PLUGIN_EXE=L"$<TARGET_FILE_NAME:casement-vmware>")
add_dependencies(e2e_tests casement-vmware)
