# ---------------------------------------------------------------------------
# svg2png
#
# Converts an .svg document to a .png file.  It rasterizes the SVG itself and
# uses the two image libraries (both git submodules under dependencies/):
#   * CImg   -> the anti-aliased supersample image buffer / downsample
#   * lodepng -> PNG encoding
#
# CImg is header-only, so it is consumed purely via its include path.  lodepng
# has no CMake target of its own, so lodepng.cpp is compiled directly into the
# executable.
# ---------------------------------------------------------------------------

set(SVG2PNG_LODEPNG_CPP "${CMAKE_SOURCE_DIR}/dependencies/lodepng/lodepng.cpp")

add_executable(svg2png
    main.cpp
    "${SVG2PNG_LODEPNG_CPP}"
)

target_include_directories(svg2png PRIVATE
    "${CMAKE_SOURCE_DIR}/dependencies/CImg"
    "${CMAKE_SOURCE_DIR}/dependencies/lodepng"
)
