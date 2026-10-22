# The Casement plugin protocol layer (src/casement), the casement-sim test
# harness (tools/sim) and the build settings every target shares.

# Windows 10 1809 or later: ConPTY and other newer APIs are declared.
add_library(casement_options INTERFACE)
target_compile_features(casement_options INTERFACE cxx_std_20)
target_compile_definitions(casement_options INTERFACE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)
if(MSVC)
    target_compile_options(casement_options INTERFACE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
else()
    target_compile_options(casement_options INTERFACE -Wall -Wextra -Wno-missing-field-initializers)
    # One self-contained exe: no MinGW runtime DLLs to ship.
    target_link_options(casement_options INTERFACE -static -static-libgcc -static-libstdc++)
endif()

set(CASEMENT_COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(casement STATIC
    ${CASEMENT_COMMON_DIR}/src/casement/address.cpp
    ${CASEMENT_COMMON_DIR}/src/casement/display.cpp
    ${CASEMENT_COMMON_DIR}/src/casement/log.cpp
    ${CASEMENT_COMMON_DIR}/src/casement/plugin.cpp
    ${CASEMENT_COMMON_DIR}/src/casement/protocol.cpp
    ${CASEMENT_COMMON_DIR}/src/casement/text.cpp)
target_include_directories(casement PUBLIC
    ${CASEMENT_COMMON_DIR}/src
    ${CASEMENT_COMMON_DIR}/third_party)
target_link_libraries(casement PUBLIC casement_options)

add_library(casement_sim STATIC ${CASEMENT_COMMON_DIR}/tools/sim/sim.cpp)
target_include_directories(casement_sim PUBLIC
    ${CASEMENT_COMMON_DIR}/tools/sim
    ${CASEMENT_COMMON_DIR}/third_party)
target_link_libraries(casement_sim PUBLIC casement_options)

add_executable(casement-sim ${CASEMENT_COMMON_DIR}/tools/sim/main.cpp)
target_link_libraries(casement-sim PRIVATE casement_sim)

# A plugin executable: built for the GUI subsystem, so starting it never
# opens a console window; its redirected standard handles work as usual.
function(casement_plugin_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE casement)
    if(MSVC)
        target_link_options(${target} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(${target} PRIVATE -mwindows)
    endif()
endfunction()

# A test program using tests/check.h, registered with ctest under a label:
# "unit" tests run anywhere, "e2e" tests need the servers ci/e2e.sh starts.
function(casement_test target label)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${CASEMENT_COMMON_DIR}/tests)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS ${label})
endfunction()
