# ---------------------------------------------------------------------------
# text2mid
#
# Synthesises a Standard MIDI File (.mid) from a melody written in the "mel"
# note language (see tools/text2mid/main.cpp and tools/text2mid.md). The tool
# only renders bytes to disk, so it has no external dependencies: it links
# against the C++ standard library alone.
# ---------------------------------------------------------------------------

add_executable(text2mid
    main.cpp
)
