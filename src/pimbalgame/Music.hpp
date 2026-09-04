#pragma once

#include <SFML/Audio.hpp>
#include <filesystem>
#include <memory>
#include <vector>

namespace pimbalgame
{
// Continuous background music.
//
// The track is synthesised entirely at load time: the MIDI file is replayed
// against the SoundFont using TinySoundFont and the rendered result is cached in
// a std::vector of 16-bit interleaved-stereo PCM samples (member `mPcm`). That
// cached PCM is then handed to SFML (via an sf::SoundBuffer wrapped by an
// sf::Sound) and played on loop. Rendering once up front keeps the audio thread
// during gameplay free of synthesis work; the trade-off is that the whole track
// is held in memory, which is fine for typical background-music lengths.
class Music
{
public:
    Music();
    ~Music();

    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    // Load a SoundFont (.sf2) and a MIDI file (.mid), render the whole track to
    // PCM, cache it, and wire it up to SFML. Returns false if either file cannot
    // be loaded or rendered. Playback starts with play().
    bool load(const std::filesystem::path& soundFontPath,
              const std::filesystem::path& midiPath);

    // Start / pause / stop playback. Safe to call; ignored when invalid.
    void play();
    void pause();
    void stop();
    [[nodiscard]] bool isPlaying() const;

    // True once load() has succeeded.
    [[nodiscard]] bool isValid() const { return mValid; }

    // Current PCM cache: 16-bit interleaved-stereo samples at kSampleRate. Kept
    // as the authoritative rendered result; SFML owns its own copy inside the
    // sound buffer. Exposed for inspection / debugging.
    [[nodiscard]] const std::vector<std::int16_t>& pcmData() const { return mPcm; }

private:
    static constexpr unsigned int kSampleRate = 44100;
    static constexpr unsigned int kChannels = 2;

    bool mValid = false;
    std::vector<std::int16_t> mPcm;             // cached rendered PCM (interleaved stereo)
    sf::SoundBuffer mBuffer;                    // SFML buffer wrapping a copy of mPcm
    std::unique_ptr<sf::Sound> mSound;          // SFML playback source (built in load())
};
} // namespace pimbalgame
