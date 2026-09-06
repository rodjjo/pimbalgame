#pragma once

#include <SFML/Graphics.hpp>

#include <string>
#include <vector>

namespace pimbalgame
{
// Shared audio settings. The game owns one of these and applies it to the real
// music/sfx playback sources; the menu reads and mutates it in place so a volume
// change made in the Options view takes effect immediately.
//
// The three sliders map onto each other as: the applied level is the product of
// the master `general` slider and the individual `music`/`sfx` slider (both in
// 0..1). That gives a genuine master volume on top of the per-category sliders
// without needing an audio mixer.
struct AudioSettings
{
    float general = 1.0f;  // master
    float music = 1.0f;
    float sfx = 1.0f;
};

// The menu subsystem: the main menu (New Game / Options / Exit), the Options
// view (General / Music / SFX volume sliders) and a pause overlay shown on top
// of frozen gameplay. It owns no audio of its own -- it reads and mutates the
// shared AudioSettings that Game applies to the playback sources, and returns
// high-level requests (start game / quit / resume / ...) for the caller to act
// on.
//
// Volume sliders are operated primarily with the mouse scroll wheel (while the
// cursor is over the bar) and by clicking on the bar to jump to a level; they
// can also be nudged with the keyboard when a slider is focused.
class Menu
{
public:
    // What the caller should do with the outcome of an event.
    enum class Request
    {
        None,
        StartGame,
        Quit,
        // Pause-overlay actions.
        Resume,          // close the overlay and resume gameplay
        ToMainMenu,      // leave gameplay back to the main menu
    };

    // Which sub-view the menu is showing.
    enum class View
    {
        Main,         // main menu
        Options,      // full-screen volume options
        Pause,        // pause overlay (Resume / Volume / Main Menu / Quit)
        PauseOptions, // volume sliders shown as a pause-overlay sub-view
    };

    Menu() = delete;
    explicit Menu(sf::Font& font, AudioSettings& audio);

    // Whether the menu is drawn as an overlay on top of frozen gameplay
    // (translucent backdrop) rather than as a full-screen menu (opaque clear).
    // Game flips this as the player pauses / resumes.
    void setOverlay(bool overlay) { mOverlay = overlay; }
    [[nodiscard]] bool isOverlay() const { return mOverlay; }

    // Enter the pause overlay: freeze gameplay behind a translucent backdrop and
    // show the pause menu (Resume / Volume / Main Menu / Quit). The world itself
    // is already frozen by the caller (Game stops the physics loop while paused),
    // so this only flips the rendering / navigation state and re-centres the
    // pause menu selection.
    void enterPause()
    {
        mOverlay = true;
        mView = View::Pause;
        mPauseSel = 0;
        mFocus = 0;
    }

    // Process a single event and mutate internal selection / volume state.
    // Returns a request the caller should act on.
    Request handle(const sf::Event& event);

    // Render the currently selected view onto `window` (updates per-frame text
    // such as volume readouts and the highlighted selection).
    void render(sf::RenderWindow& window);

    // Reset to the main-menu view; used when returning from gameplay.
    void resetToMain()
    {
        mView = View::Main;
        mSelected = 0;
        mPauseSel = 0;
    }

private:
    // A single volume slider: a label, a pointer into AudioSettings, and the
    // geometry of its track / filled bar / knob.
    struct Slider
    {
        std::string label;
        float* value = nullptr;  // pointer into AudioSettings (0..1)
        sf::Rect<float> track;   // full horizontal track
        sf::Vector2f knobCenter;  // centre of the sliding knob
        float knobSize = 20.f;
    };

    // Event handling split by context.
    Request handleMenu(const sf::Event& event);
    Request handlePause(const sf::Event& event);

    // Hit tests.
    int mainItemAt(sf::Vector2f pos) const;     // main-menu item under a point
    int pauseItemId(sf::Vector2f pos) const;    // pause-overlay item under a point
    int sliderAt(sf::Vector2f pos) const;       // slider under a point
    bool isBackAt(sf::Vector2f pos) const;      // back button under a point

    // Rendering split by view.
    void renderMain(sf::RenderWindow& window);
    void renderOptions(sf::RenderWindow& window);
    void renderPause(sf::RenderWindow& window);
    void renderSlidersOverlay(sf::RenderWindow& window);

    // Shared slider renderer used by both the full-screen Options view and the
    // pause-overlay volume sub-view.
    void renderSliders(sf::RenderWindow& window);

    void recomputeSliders();
    void setValue(int index, float v);
    void adjust(int index, float delta);
    void scrollOverSlider(float delta);

    sf::Font& mFont;
    AudioSettings& mAudio;

    std::vector<Slider> mSliders;

    // Visual elements.
    sf::Text mTitle;          // "PimBalGame" (main) / "Options" / "Paused"
    sf::Text mMarker;         // ">" selection marker
    std::vector<sf::Text> mMainItems;
    std::vector<sf::Vector2f> mMainItemPos;
    std::vector<sf::Text> mPauseItems;
    std::vector<sf::Vector2f> mPauseItemPos;
    std::vector<sf::Text> mSliderLabels;
    sf::Text mBackItem;
    sf::Text mHint;

    // Navigation state.
    bool mOverlay = false;
    View mView = View::Main;
    int mSelected = 0;      // main-menu selection index
    int mPauseSel = 0;      // pause-overlay selection index
    int mFocus = 0;         // option-view focus index (-1 == back button)

    // Last mouse position, used for hover / wheel-over-bar hit tests.
    sf::Vector2i mMouse;

    // Step applied per scroll notch / keyboard nudge (fraction of full scale).
    static constexpr float kStep = 0.05f;
    static constexpr float kWheelScale = 120.0f;  // SFML wheel delta per notch
};

} // namespace pimbalgame
