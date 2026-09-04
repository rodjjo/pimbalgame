# ---------------------------------------------------------------------------
# Game executable
# ---------------------------------------------------------------------------
add_executable(pimbalgame
    main.cpp
    pimbalgame/Game.cpp
    pimbalgame/World.cpp
    pimbalgame/Ball.cpp
    pimbalgame/Flipper.cpp
    pimbalgame/Bumper.cpp
    pimbalgame/Textures.cpp
    pimbalgame/Particles.cpp
    pimbalgame/Music.cpp
)

target_include_directories(pimbalgame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# TinySoundFont is a single-header dependency (tsf.h + tml.h) with no build
# system of its own; expose its root as an include path so the game can #include
# it. The implementations are pulled in by defining TSF_IMPLEMENTATION /
# TML_IMPLEMENTATION inside pimbalgame/Music.cpp (the one TU that owns them).
target_include_directories(pimbalgame PRIVATE
    "${CMAKE_SOURCE_DIR}/dependencies/tiny-sound-font"
)

# SFML is built in-tree from the submodule, which exposes the namespaced alias
# targets SFML::System, SFML::Window, SFML::Graphics and SFML::Audio. Audio is
# required to feed the cached PCM (rendered by TinySoundFont) to the device.
# SFML's Audio module is built against the system Vorbis/FLAC/Ogg libraries
# (SFML_USE_SYSTEM_DEPS), so no extra in-tree codec build is needed.
target_link_libraries(pimbalgame PRIVATE
    SFML::System
    SFML::Window
    SFML::Graphics
    SFML::Audio
)

# Box2D is built in-tree from the submodule (see the root CMakeLists.txt), which
# exposes the namespaced alias target box2d::box2d. It is linked here now so the
# game can use it once the physics code is ported over; a static library that no
# code references contributes nothing to the link.
target_link_libraries(pimbalgame PRIVATE
    box2d::box2d
)

# Keep the console window on Windows when launched from an IDE/Explorer.
if(WIN32)
    set_target_properties(pimbalgame PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# ---------------------------------------------------------------------------
# Embedded texture atlas.
#
# The game's art lives in assets/*.svg (the only art that is version-controlled).
# At build time the svg2png tool (see tools/) rasterises every SVG to a PNG
# inside the build tree and packs all of them into a single embeddable C++
# header (textures.cxxpng) that the game #includes to embed the atlas bytes.
# This keeps the game self-contained: no image files ship with the executable.
# ---------------------------------------------------------------------------
set(ART_DIR "${CMAKE_CURRENT_BINARY_DIR}/art")
set(ATLAS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/textures.cxxpng")
set(ATLAS_PNG "${ART_DIR}/texture_atlas.png")

file(GLOB GAME_SVG "${CMAKE_SOURCE_DIR}/assets/*.svg")
list(SORT GAME_SVG)

# Created at configure time so the per-SVG rasterisation commands have a place
# to write their PNG output.
file(MAKE_DIRECTORY "${ART_DIR}")

if(TARGET svg2png)
    set(SVG2PNG "$<TARGET_FILE:svg2png>")

    # Rasterise each SVG into its own PNG.  Each conversion is its own
    # add_custom_command (one OUTPUT per PNG) so CMake tracks every file
    # independently and runs each SVG as a separate process.  (Collapsing the
    # conversions into a single COMMAND list would hand svg2png one argv it
    # cannot interpret.)
    set(SVG_PNGS "")
    foreach(svg ${GAME_SVG})
        get_filename_component(name "${svg}" NAME_WE)
        set(png "${ART_DIR}/${name}.png")
        list(APPEND SVG_PNGS "${png}")
        add_custom_command(
            OUTPUT "${png}"
            DEPENDS "${svg}" "${SVG2PNG}"
            COMMAND "${SVG2PNG}" "png"
                   "--svg-path" "${svg}"
                   "--save-path" "${png}"
                   "--transparent"
            COMMENT "[svg2png] rasterising ${name}.svg"
            VERBATIM
        )
    endforeach()

    # Pack all PNGs into the embeddable atlas header.  The argument list is a
    # single flat list of tokens (no embedded COMMAND keyword) so CMake expands
    # it into one clean argv for svg2png.
    set(TEXTURE_CMD "${SVG2PNG}" "texture"
        "--output-path" "${ATLAS_HEADER}"
        "--padding" "3"
        "--shelf-width" "0"
        "--atlas-path" "${ATLAS_PNG}")
    foreach(png ${SVG_PNGS})
        list(APPEND TEXTURE_CMD "--png-path" "${png}")
    endforeach()

    add_custom_command(
        OUTPUT "${ATLAS_HEADER}"
        BYPRODUCTS ${SVG_PNGS} "${ATLAS_PNG}"
        DEPENDS ${SVG_PNGS} "${SVG2PNG}"
        COMMAND ${TEXTURE_CMD}
        COMMENT "[svg2png] packing PNGs into an embeddable texture atlas"
        VERBATIM
    )

    add_custom_target(BuildAtlas DEPENDS ${ATLAS_HEADER})
    add_dependencies(pimbalgame BuildAtlas)
else()
    # No art tool: provide an empty placeholder so the game still compiles and
    # falls back to procedural shapes. See the README 'Release notes'.
    file(WRITE "${ATLAS_HEADER}"
"// Texture atlas header.
//
// The project was configured without the svg2png art tool, so this empty
// placeholder is used and the game falls back to procedural shapes. Build with
// -DBUILD_TOOLS=ON (the default) to generate the atlas from assets/*.svg.
#ifndef SVG2PNG_ATLAS_FALLBACK_H_
#define SVG2PNG_ATLAS_FALLBACK_H_
#include <array>
#include <cstdint>
#include <map>
#include <string>
namespace texture_atlas {
    struct coordinate_t {
        std::uint32_t x = 0, y = 0, width = 0, height = 0;
    };
    static const std::array<std::uint8_t, 0> atlas_png = {};
    inline const std::map<std::string, coordinate_t>& textures() {
        static const std::map<std::string, coordinate_t> m = {};
        return m;
    }
}
#endif // SVG2PNG_ATLAS_FALLBACK_H_
")
endif()

# The generated header (textures.cxxpng) is emitted into the build tree and must
# be findable by `#include "textures.cxxpng"`.
target_include_directories(pimbalgame PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Copy the bundled HUD font next to the executable so it can be found at
# runtime (see Game::loadFont).
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/DejaVuSans.ttf
     DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts)

# Copy the background music assets (SoundFont + MIDI) next to the executable so
# they can be found at runtime (see Game's resolveSoundFile).
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../assets/sounds/sound_file.sf2
     ${CMAKE_CURRENT_SOURCE_DIR}/../assets/sounds/texas_e_pacific_boogie_woogie_bass.mid
      DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sounds)
