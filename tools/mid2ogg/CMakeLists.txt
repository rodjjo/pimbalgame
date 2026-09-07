# ---------------------------------------------------------------------------
# mid2ogg
#
# Renders a Standard MIDI File (.mid) into an Ogg Vorbis audio file (.ogg) by
# playing the MIDI back against a SoundFont (.sf2) with TinySoundFont and then
# encoding the captured PCM with SFML's Ogg Vorbis writer (see
# tools/mid2ogg/main.cpp).
#
# TinySoundFont is a single-header dependency (tsf.h + tml.h) with no build
# system of its own; expose its root as an include path. The implementations are
# pulled in by defining TSF_IMPLEMENTATION / TML_IMPLEMENTATION in main.cpp.
# SFML::Audio provides sf::SoundBuffer::saveToFile, which writes the .ogg.
# ---------------------------------------------------------------------------

add_executable(mid2ogg
    main.cpp
)

target_include_directories(mid2ogg PRIVATE
    "${CMAKE_SOURCE_DIR}/dependencies/tiny-sound-font"
)

target_link_libraries(mid2ogg PRIVATE
    SFML::Audio
)
