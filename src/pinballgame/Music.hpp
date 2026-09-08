#pragma once

#include <SFML/Audio.hpp>
#include <filesystem>
#include <memory>

namespace pinballgame
{
// Continuous background music.
//
// The Ogg Vorbis track (pinball_pirates.ogg) is loaded directly into an
// sf::SoundBuffer and played on loop through an sf::Sound. We rely on SFML's
// built-in Vorbis codec rather than synthesising audio from a MIDI file and a
// SoundFont, which keeps the implementation small and cheap at load time.
class Music
{
public:
    Music();
    ~Music();

    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    // Load an Ogg Vorbis audio file (.ogg) and wire it up to SFML. Playback
    // starts with play(); the track loops. Returns false if the file cannot be
    // loaded.
    bool load(const std::filesystem::path& audioPath);

    // Start / pause / stop playback. Safe to call; ignored when invalid.
    void play();
    void pause();
    void stop();
    [[nodiscard]] bool isPlaying() const;

    // Master volume (0..100+, SFML convention). Safe to call; ignored when
    // invalid. Used by the menu's Music/General sliders.
    void setVolume(float volume);

    // True once load() has succeeded.
    [[nodiscard]] bool isValid() const { return mValid; }

private:
    bool mValid = false;
    sf::SoundBuffer mBuffer;                    // SFML buffer holding the audio
    std::unique_ptr<sf::Sound> mSound;          // SFML playback source (built in load())
};
} // namespace pinballgame
