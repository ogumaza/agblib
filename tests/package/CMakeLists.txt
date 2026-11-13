# SPDX-License-Identifier: MIT

# A project that uses agblib, to check the CMake package. AGBLIB_ROUTE selects how the project gets agblib:
# "subdirectory" adds the source folder, "fetchcontent" declares it with FetchContent, and "find_package" finds an
# installed copy. In the first two, the project adds xsflib and ntdlib from agblib's submodule first. A project that
# uses the three libraries does the same. To check a route, configure this folder with -DAGBLIB_ROUTE=<route>, build it
# and run ctest.
cmake_minimum_required(VERSION 3.20)
project(agblib_package_test LANGUAGES C CXX)

set(AGBLIB_ROUTE "find_package" CACHE STRING "How to get agblib: subdirectory, fetchcontent or find_package")
set(AGBLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "agblib's source folder")
set(AGBLIB_NTDLIB_DIR "${AGBLIB_SOURCE_DIR}/externals/ntdlib")

if (AGBLIB_ROUTE STREQUAL "subdirectory")
    add_subdirectory(${AGBLIB_NTDLIB_DIR}/externals/xsflib xsflib EXCLUDE_FROM_ALL)
    add_subdirectory(${AGBLIB_NTDLIB_DIR} ntdlib EXCLUDE_FROM_ALL)
    add_subdirectory(${AGBLIB_SOURCE_DIR} agblib EXCLUDE_FROM_ALL)
elseif (AGBLIB_ROUTE STREQUAL "fetchcontent")
    include(FetchContent)
    FetchContent_Declare(xsflib SOURCE_DIR ${AGBLIB_NTDLIB_DIR}/externals/xsflib)
    FetchContent_Declare(ntdlib SOURCE_DIR ${AGBLIB_NTDLIB_DIR})
    FetchContent_Declare(agblib SOURCE_DIR ${AGBLIB_SOURCE_DIR})
    FetchContent_MakeAvailable(xsflib ntdlib agblib)
elseif (AGBLIB_ROUTE STREQUAL "find_package")
    find_package(agblib 1.0 REQUIRED)
else()
    message(FATAL_ERROR "AGBLIB_ROUTE is ${AGBLIB_ROUTE}: use subdirectory, fetchcontent or find_package")
endif()

add_executable(package_test main.cpp)
target_link_libraries(package_test PRIVATE agblib::agb xsflib::testing)
set_target_properties(package_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME package_test COMMAND package_test)
