# Network Monitoring Dashboard - CMake build (alternative to the Makefile).
#
#   cmake -S . -B build && cmake --build build -j && ./build/netmon
#
# Sections: project and language standard, build type, Raylib, executable,
# platform libraries, warnings, asset copy.

cmake_minimum_required(VERSION 3.14)
project(network_monitoring_dashboard C)

# ---- Language standard ----

# Strict C99 without compiler extensions.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---- Build type ----

# Default to an optimised build for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Raylib ----

# Use an installed Raylib when available, otherwise fetch and build it.
# Examples and games are switched off to keep the fetched build small.
set(RAYLIB_VERSION 5.5)
find_package(raylib ${RAYLIB_VERSION} QUIET)
if(NOT raylib_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG ${RAYLIB_VERSION}
        GIT_SHALLOW ON
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

# ---- Targets ----

# The dashboard application, linked against Raylib.
add_executable(netmon
    src/main.c
    src/theme.c
)
target_link_libraries(netmon PRIVATE raylib)

# ---- Platform libraries ----

# libm is separate from libc on Unix-like systems.
if(UNIX)
    target_link_libraries(netmon PRIVATE m)
endif()

# ---- Warnings ----

# MSVC: /W4 and no deprecation warnings for the C runtime. Other
# compilers: the same strict set as the Makefile.
if(MSVC)
    target_compile_options(netmon PRIVATE /W4)
    target_compile_definitions(netmon PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(netmon PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# ---- Assets ----

# Fonts are loaded at runtime from assets/fonts next to the executable.
add_custom_command(TARGET netmon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:netmon>/assets
)
