#include "pimbalgame/Game.hpp"
#include "pimbalgame/Music.hpp"
#include "pimbalgame/SoundEffect.hpp"
#include "pimbalgame/World.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pimbalgame
{
namespace
{
    // Left flipper controls: A, Z or the Left arrow.
    bool isLeftFlipperPressed()
    {
        return sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A) ||
               sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Z) ||
               sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Left);
    }

    // Right flipper controls: D or the Right arrow.
    bool isRightFlipperPressed()
    {
        return sf::Keyboard::isKeyPressed(sf::Keyboard::Key::D) ||
               sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Right);
    }

    std::filesystem::path executableDir()
    {
#if defined(_WIN32)
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n == MAX_PATH)
        {
            return {};
        }
        return std::filesystem::path(path).parent_path();
#else
        char buffer[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (n <= 0)
        {
            return {};
        }
        buffer[n] = '\0';
        return std::filesystem::path(buffer).parent_path();
#endif
    }

    const char* kFontName = "fonts/DejaVuSans.ttf";
    const char* kSourceFontName = "assets/fonts/DejaVuSans.ttf";

    // Background music asset: an Ogg Vorbis track played on loop. It lives in the
    // game's sound assets and is copied next to the executable at build time (see
    // src/CMakeLists.txt).
    const char* kMusicOggName = "sounds/pinball_pirates.ogg";
    const char* kSourceMusicOggName = "assets/sounds/pinball_pirates.ogg";

    // Find a sound asset, preferring the copy next to the executable and falling
    // back to the source-tree location (useful when run from the project root).
    std::filesystem::path resolveSoundFile(const char* exeRelativeName, const char* sourceName)
    {
        std::vector<std::filesystem::path> candidates;
        if (const std::filesystem::path exeDir = executableDir(); !exeDir.empty())
        {
            candidates.push_back(exeDir / exeRelativeName);
        }
        candidates.push_back(std::filesystem::path(sourceName));
        for (const auto& path : candidates)
        {
            std::ifstream probe(path);
            if (probe.good())
            {
                return path;
            }
        }
        return {};
    }
}

Game::Game()
    : mWindow(sf::VideoMode({kWindowWidth, kWindowHeight}), "PimBalGame",
              sf::Style::Titlebar | sf::Style::Close)
{
    mWindow.setFramerateLimit(60);
    centerWindow();

    mFontLoaded = loadFont();
    if (mFontLoaded)
    {
        mScoreText.emplace(mFont, "", kFontSize);
        mBallsText.emplace(mFont, "", kFontSize);
        mStatusText.emplace(mFont, "", kFontSize * 2);
    }

    mWorld = std::make_unique<World>(kWindowWidth, kWindowHeight);

    // Procedural sound-effects bank (short, synthesized blips for game events).
    // Audio failure is non-fatal: the game remains fully playable with muted
    // audio if the audio device cannot be initialised.
    mSoundEffect = std::make_shared<SoundEffect>();
    mWorld->setSound(mSoundEffect);

    // The menu subsystem. It needs the font to be loaded (see loadFont above);
    // without a font the game still runs but the menu draws nothing.
    if (mFontLoaded)
    {
        mMenu = std::make_unique<Menu>(mFont, mAudio);
    }

    // Apply the (default) audio settings to the playback sources.
    applyAudioVolumes();

    // Load and play the background music. Audio failure is non-fatal: the game
    // remains fully playable with muted audio if the assets cannot be loaded.
    const std::filesystem::path musicPath = resolveSoundFile(kMusicOggName, kSourceMusicOggName);
    if (!musicPath.empty())
    {
        mMusic = std::make_unique<Music>();
        if (!mMusic->load(musicPath))
        {
            std::cerr << "Failed to load background music (" << musicPath << ")\n";
        }
        else
        {
            mMusic->play();
        }
    }

    updateHud();
}

Game::~Game() = default;

void Game::centerWindow()
{
    const sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
    const int x = static_cast<int>((desktop.size.x - kWindowWidth) / 2);
    const int y = static_cast<int>((desktop.size.y - kWindowHeight) / 2);
    mWindow.setPosition(sf::Vector2i(std::max(0, x), std::max(0, y)));
}

bool Game::loadFont()
{
    const std::error_code ec;
    std::vector<std::filesystem::path> candidates;

    if (const std::filesystem::path exeDir = executableDir(); !exeDir.empty())
    {
        candidates.push_back(exeDir / kFontName);
        candidates.push_back(exeDir / "assets" / "fonts" / "DejaVuSans.ttf");
    }
    candidates.push_back(std::filesystem::path(kSourceFontName));

    for (const auto& path : candidates)
    {
        std::ifstream probe(path);
        if (!probe.good())
        {
            continue;
        }
        sf::Font font;
        if (font.openFromFile(path))
        {
            mFont = std::move(font);
            return true;
        }
    }

    return false;
}

int Game::run()
{
    sf::Clock clock;

    // If the font failed to load we could not build a menu, so fall back to
    // direct gameplay to keep the game reachable.
    if (mScreen == Screen::Menu && !mMenu)
    {
        mScreen = Screen::Playing;
    }

    while (mWindow.isOpen())
    {
        float frameTime = clock.restart().asSeconds();
        // Clamp large frames (e.g. after a focus switch) to avoid the
        // physics "spiral of death".
        if (frameTime > 0.25f)
        {
            frameTime = 0.25f;
        }
        mAccumulator += frameTime;

        // Route events: menu screens hand every event to the menu; gameplay
        // keeps the Escape-to-pause and R-to-restart shortcuts.
        const Menu::Request request = processEvents();

        if (request == Menu::Request::StartGame)
        {
            // New Game: leave the menu and drop into a fresh game.
            if (mMenu)
            {
                mMenu->setOverlay(false);
            }
            mAccumulator = 0.0f;
            mScreen = Screen::Playing;
            mWorld->reset();
        }
        else if (request == Menu::Request::ToMainMenu)
        {
            // Leave gameplay and show the main menu.
            if (mMenu)
            {
                mMenu->setOverlay(false);
            }
            mAccumulator = 0.0f;
            mScreen = Screen::Menu;
        }
        else if (request == Menu::Request::Resume)
        {
            // Close the pause overlay and resume gameplay.
            if (mMenu)
            {
                mMenu->setOverlay(false);
            }
            mAccumulator = 0.0f;
            mScreen = Screen::Playing;
        }
        else if (request == Menu::Request::Quit)
        {
            // Exit: close the window; the loop condition exits on the next check.
            mWindow.close();
            continue;
        }

        // Keep the applied volumes in sync with whatever the menu just edited.
        applyAudioVolumes();

        if (mScreen == Screen::Playing)
        {
            // Continuous keyboard state drives the flippers and plunger.
            mWorld->setLeftFlipper(isLeftFlipperPressed());
            mWorld->setRightFlipper(isRightFlipperPressed());
            mWorld->setPlungerHeld(sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Space));

            while (mAccumulator >= kPhysicsTimestep)
            {
                mWorld->update(kPhysicsTimestep);
                mAccumulator -= kPhysicsTimestep;
            }

            updateHud();
            render();
        }
        else
        {
            // Menu or Paused: the world is frozen and the menu owns the render.
            renderMenu();
        }
    }

    return 0;
}

Menu::Request Game::processEvents()
{
    Menu::Request request = Menu::Request::None;

    while (auto event = mWindow.pollEvent())
    {
        if (event && event->is<sf::Event::Closed>())
        {
            mWindow.close();
            return Menu::Request::Quit;
        }

        if (mScreen == Screen::Playing)
        {
            if (auto* keyEvent = event->getIf<sf::Event::KeyPressed>())
            {
                if (keyEvent->code == sf::Keyboard::Key::Escape)
                {
                    // Pause: freeze gameplay behind the pause overlay and hand
                    // input to the menu. Escape (or Resume) from the overlay
                    // returns to the game and resumes it.
                    if (mMenu)
                    {
                        mMenu->enterPause();
                        mAccumulator = 0.0f;
                        mScreen = Screen::Paused;
                    }
                }
                else if (keyEvent->code == sf::Keyboard::Key::R && mWorld->gameOver())
                {
                    mWorld->reset();
                    updateHud();
                }
            }
            continue;
        }

        // Menu screen: hand every event to the menu.
        request = mMenu->handle(*event);
    }

    return request;
}

void Game::render()
{
    mWindow.clear(sf::Color(18, 20, 28));
    mWorld->render(mWindow);

    // Head-up display. Only drawn when a font was loaded.
    if (mFontLoaded)
    {
        mWindow.draw(*mScoreText);
        mWindow.draw(*mBallsText);
        if (mWorld->gameOver())
        {
            mWindow.draw(*mStatusText);
        }
    }

    mWindow.display();
}

void Game::renderMenu()
{
    // The menu renders (and clears) the whole window itself.
    mMenu->render(mWindow);
    mWindow.display();
}

void Game::applyAudioVolumes()
{
    // The applied level is the master `general` slider multiplied by the
    // individual slider, scaled to SFML's 0..100+ volume convention.
    if (mMusic)
    {
        const float volume = std::min(1.0f, mAudio.general * mAudio.music) * 100.0f;
        mMusic->setVolume(volume);
    }
    if (mSoundEffect)
    {
        const float volume = std::min(1.0f, mAudio.general * mAudio.sfx) * 100.0f;
        mSoundEffect->setSfxVolume(volume);
    }
}

void Game::updateHud()
{
    if (!mFontLoaded)
    {
        return;
    }

    (*mScoreText).setFillColor(sf::Color(230, 230, 255));
    (*mScoreText).setString("Score: " + std::to_string(mWorld->score()));
    (*mScoreText).setPosition(sf::Vector2f(16.f, 12.f));

    if (mWorld->gameOver())
    {
        (*mBallsText).setFillColor(sf::Color(230, 230, 255));
        (*mBallsText).setString("Game Over");
        (*mStatusText).setFillColor(sf::Color(255, 210, 90));
        (*mStatusText).setString("Press R to restart");
    }
    else
    {
        (*mBallsText).setFillColor(sf::Color(230, 230, 255));
        (*mBallsText).setString("Balls: " + std::to_string(mWorld->balls()));
    }
    (*mBallsText).setPosition(sf::Vector2f(16.f, 44.f));

    (*mStatusText).setPosition(sf::Vector2f(kWindowWidth / 2 - 160,
                                            kWindowHeight / 2 - 20.f));
}

} // namespace pimbalgame
