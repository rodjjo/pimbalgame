#pragma once

#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <memory>
#include <optional>

namespace pimbalgame
{
class World;

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
    void processEvents();
    void render();
    void updateHud();

    void centerWindow();
    bool loadFont();

    static constexpr int kWindowWidth = 640;
    static constexpr int kWindowHeight = 920;
    static constexpr int kFontSize = 24;
    static constexpr float kPhysicsTimestep = 1.0f / 120.0f;

    sf::RenderWindow mWindow;
    std::unique_ptr<World> mWorld;

    sf::Font mFont;
    bool mFontLoaded = false;
    std::optional<sf::Text> mScoreText;
    std::optional<sf::Text> mBallsText;
    std::optional<sf::Text> mStatusText;
};

} // namespace pimbalgame
