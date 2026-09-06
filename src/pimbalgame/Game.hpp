#pragma once

#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <memory>
#include <optional>

#include "pimbalgame/Menu.hpp"

namespace pimbalgame
{
class World;
class Music;
class SoundEffect;

// Top-level application screen.
enum class Screen
{
    Menu,    // main menu / options (full-screen menu, no gameplay)
    Paused,  // gameplay frozen behind the pause overlay
    Playing, // active pinball playfield
};

// Container that owns the render window (centered on the desktop) and drives
// the main loop: input, fixed-timestep physics update and rendering.
class Game
{
public:
    Game();
    ~Game();

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Runs the main loop. Returns the process exit code.
    int run();

private:
    Menu::Request processEvents();
    void render();
    void renderMenu();
    void updateHud();

    void centerWindow();
    bool loadFont();

    // Push the current AudioSettings onto the music/sfx playback sources so the
    // menu's volume sliders have a real, audible effect.
    void applyAudioVolumes();

    static constexpr int kWindowWidth = 640;
    static constexpr int kWindowHeight = 920;
    static constexpr int kFontSize = 24;
    static constexpr float kPhysicsTimestep = 1.0f / 120.0f;

    sf::RenderWindow mWindow;
    std::unique_ptr<World> mWorld;
    std::unique_ptr<Music> mMusic;
    std::shared_ptr<SoundEffect> mSoundEffect;
    std::unique_ptr<Menu> mMenu;

    // Which screen is active. Starts on the main menu.
    Screen mScreen = Screen::Menu;

    // Master audio levels (0..1). The menu reads and edits these in place; the
    // game applies them to the playback sources.
    AudioSettings mAudio;

    // Fixed-timestep accumulator (member so it can be zeroed on every screen
    // transition: pausing, resuming, starting a game or leaving to the menu).
    // Zeroing it keeps a resumed game from catching up on the time spent paused.
    float mAccumulator = 0.0f;

    sf::Font mFont;
    bool mFontLoaded = false;
    std::optional<sf::Text> mScoreText;
    std::optional<sf::Text> mBallsText;
    std::optional<sf::Text> mStatusText;
};

} // namespace pimbalgame
