cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------
project(PimBalGame
    VERSION 0.1.0
    DESCRIPTION "A pinball game built with SFML"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# SFML dependency (git submodule at dependencies/sfml)
# ---------------------------------------------------------------------------
# The submodule is expected to be present. If the source directory does not
# exist the submodule has not been checked out yet:
#
#     git submodule update --init --recursive
#
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/sfml/CMakeLists.txt")
    message(FATAL_ERROR
        "SFML submodule is not checked out at 'dependencies/sfml'.\n"
        "Run: git submodule update --init --recursive")
endif()

# The pinball game only needs the graphics/window/system components. Disabling
# the others keeps the build self-contained so host packages such as Libssh2
# (Network) or the Vorbis/gsm libraries (Audio) are not required. They can be
# re-enabled by flipping these cache variables before configuring.
set(SFML_BUILD_NETWORK OFF CACHE BOOL "Build the SFML network component" FORCE)
set(SFML_BUILD_AUDIO OFF CACHE BOOL "Build the SFML audio component" FORCE)

# Build SFML as part of the project (static, from the submodule source).
add_subdirectory(dependencies/sfml EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Box2D dependency (git submodule at dependencies/box2d, pinned to tag v3.1.1)
# ---------------------------------------------------------------------------
# A 2D physics engine used by the game's physics simulation. The submodule is
# expected to be present. If the source directory does not exist the submodule
# has not been checked out yet:
#
#     git submodule update --init --recursive
#
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/CMakeLists.txt")
    message(FATAL_ERROR
        "Box2D submodule is not checked out at 'dependencies/box2d'.\n"
        "Run: git submodule update --init --recursive")
endif()

# Box2D's top-level CMakeLists only exposes the `box2d` library when it is not
# the top-level project, so the samples, unit tests, benchmarks, docs and the
# enkiTS task system it would otherwise FetchContent are skipped. This makes the
# build self-contained with no network access at configure time.
add_subdirectory(dependencies/box2d EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Tools
#
# Developer tools (currently: svg2png) are built only when -DBUILD_TOOLS=ON.
# They rely on the CImg and lodepng submodules under dependencies/.
# ---------------------------------------------------------------------------
option(BUILD_TOOLS "Build developer tools (svg2png)" ON)
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Game targets
# ---------------------------------------------------------------------------
add_subdirectory(src)
