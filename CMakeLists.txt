cmake_minimum_required(VERSION 3.25)
project(AtomicRipper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output all binaries to a common location
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# vcpkg-provided packages
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(FLAC CONFIG REQUIRED)
find_package(taglib CONFIG REQUIRED)

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(tests)

# GUI is optional — skip if Qt6 not installed yet
find_package(Qt6 QUIET COMPONENTS Widgets Network)
if(Qt6_FOUND)
    message(STATUS "Qt6 found — building GUI")
    add_subdirectory(gui)
else()
    message(STATUS "Qt6 not found — skipping GUI (run: vcpkg install qt)")
endif()
