# Network Monitoring Dashboard - CMake build (alternative to the Makefile).
#
#   cmake -S . -B build && cmake --build build -j && ./build/netmon
#
# Sections: project and language standard, build type, Raylib, simulation
# library, executable, platform libraries, warnings, asset copy, tests.

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

# Simulation layer as its own library: it has no Raylib dependency, so the
# test executable can link it without a graphics stack.
add_library(netmon_data STATIC src/data.c)
target_include_directories(netmon_data PUBLIC src)

# The dashboard application: shell, panels, the Overview, Devices and Traffic
# pages, UI toolkit, charts and theme, plus the simulation library and
# Raylib.
add_executable(netmon
    src/main.c
    src/dashboard.c
    src/shell.c
    src/panels.c
    src/page_overview.c
    src/page_devices.c
    src/page_traffic.c
    src/ui.c
    src/charts.c
    src/theme.c
)
target_link_libraries(netmon PRIVATE netmon_data raylib)

# ---- Platform libraries ----

# libm is separate from libc on Unix-like systems.
if(UNIX)
    target_link_libraries(netmon PRIVATE m)
    target_link_libraries(netmon_data PUBLIC m)
endif()

# ---- Warnings ----

# MSVC: /W4 and no deprecation warnings for the C runtime. Other
# compilers: the same strict set as the Makefile.
if(MSVC)
    target_compile_options(netmon PRIVATE /W4)
    target_compile_definitions(netmon PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(netmon_data PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(netmon PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    target_compile_options(netmon_data PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# ---- Assets ----

# Fonts are loaded at runtime from assets/fonts next to the executable.
add_custom_command(TARGET netmon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:netmon>/assets
)

# ---- Tests ----

# Headless simulation tests, run with `ctest` from the build directory.
enable_testing()
add_executable(test_data tests/test_data.c)
target_link_libraries(test_data PRIVATE netmon_data)
add_test(NAME data COMMAND test_data)
