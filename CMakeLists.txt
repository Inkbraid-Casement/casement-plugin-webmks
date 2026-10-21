cmake_minimum_required(VERSION 3.24)
project(casement-plugin-webmks VERSION 0.1.0 LANGUAGES CXX)

include(cmake/casement.cmake)
enable_testing()

# pugixml, built with the project's settings.
add_library(pugixml STATIC third_party/pugixml/pugixml.cpp)
target_include_directories(pugixml PUBLIC third_party/pugixml)
target_link_libraries(pugixml PRIVATE casement_options)

add_library(vmware STATIC
    src/vmware/soap.cpp)
target_include_directories(vmware PUBLIC src)
target_link_libraries(vmware PUBLIC casement pugixml)

casement_test(unit_tests unit
    tests/unit/main.cpp
    tests/unit/soap_tests.cpp)
target_link_libraries(unit_tests PRIVATE vmware)
