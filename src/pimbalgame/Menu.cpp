#include "pimbalgame/Menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pimbalgame
{
namespace
{
    // Window geometry the menu is designed for (matches Game).
    constexpr int kW = 640;
    constexpr int kH = 920;

    constexpr int kTitleSize = 44;
    constexpr int kPauseTitleSize = 50;
    constexpr int kItemSize = 30;
    constexpr int kHintSize = 18;

    // Colours.
    const sf::Color kBackground(18, 20, 28);
    const sf::Color kText(230, 230, 255);
    const sf::Color kAccent(255, 210, 90);
    const sf::Color kPanel(12, 14, 22, 210);
    const sf::Color kTrack(70, 76, 100);
    const sf::Color kTrackEdge(40, 44, 62);
    const sf::Color kKnob(240, 242, 255);
    const sf::Color kHintText(170, 176, 200);
    const sf::Color kBackdrop(10, 12, 18, 200);  // translucent overlay backdrop

    // Left-aligned, vertically centred text.
    sf::Text aligned(sf::Font& font, const std::string& str, unsigned size)
    {
        sf::Text text(font, str, size);
        const auto bounds = text.getLocalBounds();
        text.setOrigin(sf::Vector2f(0.0f, bounds.size.y / 2.0f));
        return text;
    }

    // A text centred on its origin so it can be positioned by its centre point.
    sf::Text centred(sf::Font& font, const std::string& str, unsigned size)
    {
        sf::Text text(font, str, size);
        const auto bounds = text.getLocalBounds();
        text.setOrigin(sf::Vector2f(bounds.position.x + bounds.size.x / 2.0f,
                                    bounds.position.y + bounds.size.y / 2.0f));
        return text;
    }

    // Clamp and snap a raw level into the usable 0..1 range, quantised to `grid`.
    float normalize(float v, float grid)
    {
        v = std::max(0.0f, std::min(1.0f, v));
        // Round to the step grid for tidy, predictable values.
        return std::lround(v / grid) * grid;
    }
}

Menu::Menu(sf::Font& font, AudioSettings& audio)
    : mFont(font), mAudio(audio),
      mTitle(centred(mFont, "Options", kTitleSize)),
      mMarker(centred(mFont, ">", kItemSize)),  // selection marker
      mBackItem(centred(mFont, "Back", kItemSize)),
      mHint(centred(mFont, "", kHintSize))
{
    mMarker.setFillColor(kAccent);
    mHint.setFillColor(kHintText);

    // Main-menu items (left-aligned).
    const char* mainLabels[3] = {"New Game", "Options", "Exit"};
    mMainItems.reserve(3);
    for (const char* label : mainLabels)
    {
        mMainItems.push_back(aligned(mFont, label, kItemSize));
    }
    mMainItemPos = {sf::Vector2f(200.f, 378.f),
                    sf::Vector2f(200.f, 442.f),
                    sf::Vector2f(200.f, 506.f)};

    // Pause-overlay items.
    const char* pauseLabels[4] = {"Resume", "Volume", "Main Menu", "Quit"};
    mPauseItems.reserve(4);
    for (const char* label : pauseLabels)
    {
        mPauseItems.push_back(aligned(mFont, label, kItemSize));
    }
    mPauseItemPos = {sf::Vector2f(200.f, 372.f),
                     sf::Vector2f(200.f, 428.f),
                     sf::Vector2f(200.f, 484.f),
                     sf::Vector2f(200.f, 540.f)};

    // Volume sliders (label, pointer into AudioSettings).
    const char* labels[3] = {"General", "Music", "SFX"};
    float* values[3] = {&mAudio.general, &mAudio.music, &mAudio.sfx};
    mSliders.reserve(3);
    for (int i = 0; i < 3; ++i)
    {
        Slider s;
        s.label = labels[i];
        s.value = values[i];
        mSliders.push_back(s);
    }

    mSliderLabels.reserve(3);
    for (const auto& label : mSliders)
    {
        sf::Text t(mFont, label.label, kItemSize);
        t.setPosition(sf::Vector2f(64.f, 0.f));
        mSliderLabels.push_back(std::move(t));
    }

    recomputeSliders();
}

void Menu::recomputeSliders()
{
    const float trackLeft = 250.f;
    const float trackWidth = 310.f;
    const float rowHeight = 80.f;
    const float firstY = 270.f;

    for (std::size_t i = 0; i < mSliders.size(); ++i)
    {
        Slider& s = mSliders[i];
        const float cy = firstY + i * rowHeight;

        s.track = sf::Rect<float>(sf::Vector2f(trackLeft, cy - 6.f), sf::Vector2f(trackWidth, 12.f));
        const float w = static_cast<float>(s.track.size.x) * std::max(0.0f, std::min(1.0f, *s.value));
        s.knobCenter = sf::Vector2f(trackLeft + w, cy);

        // Label baseline-aligned to the row; the readout sits past the track
        // (its position is recomputed in render()).
        mSliderLabels[i].setPosition(sf::Vector2f(64.f, cy - 14.f));
    }

    mTitle.setPosition(sf::Vector2f(kW / 2.f, 110.f));
    mBackItem.setPosition(sf::Vector2f(kW / 2.f, 560.f));
    mHint.setPosition(sf::Vector2f(kW / 2.f, 860.f));
}

int Menu::mainItemAt(sf::Vector2f pos) const
{
    for (std::size_t i = 0; i < mMainItemPos.size(); ++i)
    {
        const auto& p = mMainItemPos[i];
        if (std::abs(pos.x - p.x) < 200.f && std::abs(pos.y - p.y) < 30.f)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Menu::pauseItemId(sf::Vector2f pos) const
{
    for (std::size_t i = 0; i < mPauseItemPos.size(); ++i)
    {
        const auto& p = mPauseItemPos[i];
        if (std::abs(pos.x - p.x) < 200.f && std::abs(pos.y - p.y) < 30.f)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Menu::sliderAt(sf::Vector2f pos) const
{
    for (std::size_t i = 0; i < mSliders.size(); ++i)
    {
        const auto& t = mSliders[i].track;
        // Widen the hit area vertically so bars are easy to aim at, then use
        // the rectangle's point-in-rectangle test.
        const sf::Rect<float> hit(sf::Vector2f(t.position.x - 40.f, t.position.y - 18.f),
                                  sf::Vector2f(t.size.x + 80.f, t.size.y + 36.f));
        if (hit.contains(pos))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Menu::isBackAt(sf::Vector2f pos) const
{
    return std::abs(pos.x - (kW / 2.f)) < 90.f && std::abs(pos.y - 560.f) < 28.f;
}

void Menu::setValue(int index, float v)
{
    if (index < 0 || index >= static_cast<int>(mSliders.size()))
    {
        return;
    }
    *mSliders[index].value = normalize(v, kStep);
}

void Menu::adjust(int index, float delta)
{
    if (index < 0 || index >= static_cast<int>(mSliders.size()))
    {
        return;
    }
    setValue(index, *mSliders[index].value + delta);
}

void Menu::scrollOverSlider(float delta)
{
    const int idx = sliderAt(sf::Vector2f(static_cast<float>(mMouse.x),
                                          static_cast<float>(mMouse.y)));
    if (idx < 0)
    {
        return;
    }
    float notches = std::round(std::abs(delta) / kWheelScale);
    if (notches < 1.0f)
    {
        notches = 1.0f;
    }
    adjust(idx, std::copysign(kStep * notches, delta));
}

Menu::Request Menu::handleMenu(const sf::Event& event)
{
    if (const auto* e = event.getIf<sf::Event::MouseMoved>())
    {
        mMouse = e->position;
        return Request::None;
    }

    if (const auto* e = event.getIf<sf::Event::MouseWheelScrolled>())
    {
        mMouse = e->position;
        if (mView == View::Options)
        {
            // Scroll over a volume bar to change it (the cursor must be over one
            // of the bars; see scrollOverSlider()).
            scrollOverSlider(e->delta);
        }
        else
        {
            // Cycle the main-menu selection.
            const int dir = (e->delta > 0) ? 1 : -1;
            mSelected = (mSelected + dir + static_cast<int>(mMainItemPos.size())) %
                        static_cast<int>(mMainItemPos.size());
        }
        return Request::None;
    }

    if (const auto* e = event.getIf<sf::Event::MouseButtonPressed>())
    {
        if (e->button == sf::Mouse::Button::Left)
        {
            const sf::Vector2f pos(static_cast<float>(e->position.x),
                                   static_cast<float>(e->position.y));

            if (mView == View::Options)
            {
                const int idx = sliderAt(pos);
                if (idx >= 0)
                {
                    // Jump the slider to the clicked position along the track.
                    const float t = (pos.x - mSliders[idx].track.position.x) /
                                    mSliders[idx].track.size.x;
                    setValue(idx, t);
                    mFocus = idx;
                    return Request::None;
                }
                if (isBackAt(pos))
                {
                    mView = View::Main;
                    mFocus = 0;
                    return Request::None;
                }
            }
            else
            {
                const int idx = mainItemAt(pos);
                if (idx == 0)
                {
                    return Request::StartGame;
                }
                if (idx == 1)
                {
                    mView = View::Options;
                    mFocus = 0;
                    return Request::None;
                }
                if (idx == 2)
                {
                    return Request::Quit;
                }
            }
        }
        return Request::None;
    }

    if (const auto* e = event.getIf<sf::Event::KeyPressed>())
    {
        if (mView == View::Options)
        {
            switch (e->code)
            {
                case sf::Keyboard::Key::Escape:
                    mView = View::Main;
                    mFocus = 0;
                    break;
                case sf::Keyboard::Key::Up:
                    mFocus = mFocus <= 0 ? static_cast<int>(mSliders.size()) - 1 : mFocus - 1;
                    break;
                case sf::Keyboard::Key::Down:
                    mFocus = mFocus >= static_cast<int>(mSliders.size()) - 1 ? -1 : mFocus + 1;
                    break;
                case sf::Keyboard::Key::Left:
                    adjust(mFocus, -kStep);
                    break;
                case sf::Keyboard::Key::Right:
                    adjust(mFocus, kStep);
                    break;
                default:
                    break;
            }
        }
        else
        {
            switch (e->code)
            {
                case sf::Keyboard::Key::Up:
                case sf::Keyboard::Key::Left:
                    mSelected = (mSelected - 1 + static_cast<int>(mMainItemPos.size())) %
                                static_cast<int>(mMainItemPos.size());
                    break;
                case sf::Keyboard::Key::Down:
                case sf::Keyboard::Key::Right:
                    mSelected = (mSelected + 1) % static_cast<int>(mMainItemPos.size());
                    break;
                case sf::Keyboard::Key::Enter:
                case sf::Keyboard::Key::Space:
                    if (mSelected == 0) return Request::StartGame;
                    if (mSelected == 1) { mView = View::Options; mFocus = 0; }
                    else if (mSelected == 2) return Request::Quit;
                    break;
                case sf::Keyboard::Key::Escape:
                    return Request::Quit;
                default:
                    break;
            }
        }
    }

    return Request::None;
}

Menu::Request Menu::handlePause(const sf::Event& event)
{
    if (const auto* e = event.getIf<sf::Event::MouseMoved>())
    {
        mMouse = e->position;
        return Request::None;
    }

    if (const auto* e = event.getIf<sf::Event::MouseWheelScrolled>())
    {
        mMouse = e->position;
        if (mView == View::PauseOptions)
        {
            scrollOverSlider(e->delta);
        }
        else
        {
            const int dir = (e->delta > 0) ? 1 : -1;
            mPauseSel = (mPauseSel + dir + static_cast<int>(mPauseItemPos.size())) %
                        static_cast<int>(mPauseItemPos.size());
        }
        return Request::None;
    }

    if (const auto* e = event.getIf<sf::Event::MouseButtonPressed>())
    {
        if (e->button == sf::Mouse::Button::Left)
        {
            const sf::Vector2f pos(static_cast<float>(e->position.x),
                                   static_cast<float>(e->position.y));

            if (mView == View::PauseOptions)
            {
                const int idx = sliderAt(pos);
                if (idx >= 0)
                {
                    const float t = (pos.x - mSliders[idx].track.position.x) /
                                    mSliders[idx].track.size.x;
                    setValue(idx, t);
                    mFocus = idx;
                    return Request::None;
                }
                if (isBackAt(pos))
                {
                    mView = View::Pause;
                    mFocus = 0;
                    return Request::None;
                }
            }
            else
            {
                const int id = pauseItemId(pos);
                switch (id)
                {
                    case 0: return Request::Resume;
                    case 1: mView = View::PauseOptions; return Request::None;
                    case 2: return Request::ToMainMenu;
                    case 3: return Request::Quit;
                    default: break;
                }
            }
        }
        return Request::None;
    }

    if (const auto* e = event.getIf<sf::Event::KeyPressed>())
    {
        if (mView == View::PauseOptions)
        {
            switch (e->code)
            {
                case sf::Keyboard::Key::Escape:
                    mView = View::Pause;
                    break;
                case sf::Keyboard::Key::Up:
                    mFocus = mFocus <= 0 ? static_cast<int>(mSliders.size()) - 1 : mFocus - 1;
                    break;
                case sf::Keyboard::Key::Down:
                    mFocus = mFocus >= static_cast<int>(mSliders.size()) - 1 ? -1 : mFocus + 1;
                    break;
                case sf::Keyboard::Key::Left:
                    adjust(mFocus, -kStep);
                    break;
                case sf::Keyboard::Key::Right:
                    adjust(mFocus, kStep);
                    break;
                default:
                    break;
            }
            return Request::None;
        }

        switch (e->code)
        {
            case sf::Keyboard::Key::Escape:
                // Escape always closes the overlay and resumes the game.
                return Request::Resume;
            case sf::Keyboard::Key::Up:
            case sf::Keyboard::Key::Left:
                mPauseSel = (mPauseSel - 1 + static_cast<int>(mPauseItemPos.size())) %
                            static_cast<int>(mPauseItemPos.size());
                break;
            case sf::Keyboard::Key::Down:
            case sf::Keyboard::Key::Right:
                mPauseSel = (mPauseSel + 1) % static_cast<int>(mPauseItemPos.size());
                break;
            case sf::Keyboard::Key::Enter:
            case sf::Keyboard::Key::Space:
                switch (mPauseSel)
                {
                    case 0: return Request::Resume;
                    case 1: mView = View::PauseOptions; break;
                    case 2: return Request::ToMainMenu;
                    case 3: return Request::Quit;
                    default: break;
                }
                break;
            default:
                break;
        }
    }

    return Request::None;
}

Menu::Request Menu::handle(const sf::Event& event)
{
    // While paused the menu is an overlay drawn over frozen gameplay.
    if (mOverlay)
    {
        return handlePause(event);
    }
    return handleMenu(event);
}

void Menu::render(sf::RenderWindow& window)
{
    if (mOverlay)
    {
        // Translucent backdrop over the frozen game frame.
        sf::RectangleShape backdrop(sf::Vector2f(kW, kH));
        backdrop.setOrigin(sf::Vector2f(kW / 2.f, kH / 2.f));
        backdrop.setPosition(sf::Vector2f(kW / 2.f, kH / 2.f));
        backdrop.setFillColor(kBackdrop);
        window.draw(backdrop);

        if (mView == View::PauseOptions)
        {
            renderSlidersOverlay(window);
        }
        else
        {
            renderPause(window);
        }
    }
    else
    {
        window.clear(kBackground);
        if (mView == View::Options)
        {
            renderOptions(window);
        }
        else
        {
            renderMain(window);
        }
    }
}

void Menu::renderMain(sf::RenderWindow& window)
{
    mTitle.setString("PimBalGame");
    mTitle.setFillColor(kAccent);
    mTitle.setPosition(sf::Vector2f(kW / 2.f, 110.f));
    window.draw(mTitle);

    // A subtle panel behind the items.
    sf::RectangleShape panel(sf::Vector2f(360.f, 200.f));
    panel.setPosition(sf::Vector2f(kW / 2.f, 442.f));
    panel.setOrigin(sf::Vector2f(180.f, 100.f));
    panel.setFillColor(kPanel);
    window.draw(panel);

    for (std::size_t i = 0; i < mMainItems.size(); ++i)
    {
        mMainItems[i].setFillColor(i == static_cast<std::size_t>(mSelected) ? kAccent : kText);
        mMainItems[i].setPosition(mMainItemPos[i]);
        window.draw(mMainItems[i]);
    }

    // Marker next to the selected item.
    mMarker.setPosition(sf::Vector2f(176.f, mMainItemPos[mSelected].y));
    window.draw(mMarker);

    mHint.setString("Arrow keys / scroll to navigate - Enter or click to select");
    window.draw(mHint);
}

void Menu::renderOptions(sf::RenderWindow& window)
{
    mTitle.setString("Options");
    mTitle.setFillColor(kText);
    mTitle.setPosition(sf::Vector2f(kW / 2.f, 110.f));
    window.draw(mTitle);

    renderSliders(window);

    mBackItem.setFillColor(kText);
    mBackItem.setPosition(sf::Vector2f(kW / 2.f, 560.f));
    window.draw(mBackItem);

    mHint.setString("Scroll over a bar to change it - click a bar - arrow keys adjust the focused bar");
    window.draw(mHint);
}

void Menu::renderPause(sf::RenderWindow& window)
{
    mTitle.setString("Paused");
    mTitle.setFillColor(kAccent);
    mTitle.setPosition(sf::Vector2f(kW / 2.f, 290.f));
    window.draw(mTitle);

    for (std::size_t i = 0; i < mPauseItems.size(); ++i)
    {
        mPauseItems[i].setFillColor(i == static_cast<std::size_t>(mPauseSel) ? kAccent : kText);
        mPauseItems[i].setPosition(mPauseItemPos[i]);
        window.draw(mPauseItems[i]);
    }

    mMarker.setPosition(sf::Vector2f(176.f, mPauseItemPos[mPauseSel].y));
    window.draw(mMarker);

    mHint.setString("Enter / click to choose - Escape to resume");
    window.draw(mHint);
}

void Menu::renderSlidersOverlay(sf::RenderWindow& window)
{
    mTitle.setString("Volume");
    mTitle.setFillColor(kAccent);
    mTitle.setPosition(sf::Vector2f(kW / 2.f, 180.f));
    window.draw(mTitle);

    renderSliders(window);

    mBackItem.setFillColor(kText);
    mBackItem.setPosition(sf::Vector2f(kW / 2.f, 560.f));
    window.draw(mBackItem);

    mHint.setString("Scroll over a bar to change it - click a bar - Arrow keys adjust - Escape back");
    window.draw(mHint);
}

void Menu::renderSliders(sf::RenderWindow& window)
{
    for (std::size_t i = 0; i < mSliders.size(); ++i)
    {
        const Slider& s = mSliders[i];
        const float cy = s.track.position.y + 6.f;

        // Focus ring around the focused slider.
        if (static_cast<int>(i) == mFocus)
        {
            sf::RectangleShape ring(s.track.size);
            ring.setPosition(s.track.position);
            ring.setFillColor(sf::Color(0, 0, 0, 0));
            ring.setOutlineColor(kAccent);
            ring.setOutlineThickness(3.f);
            window.draw(ring);
        }

        window.draw(mSliderLabels[i]);

        // Track.
        sf::RectangleShape track(s.track.size);
        track.setPosition(s.track.position);
        track.setFillColor(kTrack);
        track.setOutlineColor(kTrackEdge);
        track.setOutlineThickness(2.f);
        window.draw(track);

        // Filled portion.
        const float filled = static_cast<float>(s.track.size.x) *
                             std::max(0.0f, std::min(1.0f, *s.value));
        sf::RectangleShape fill(sf::Vector2f(filled, 12.f));
        fill.setPosition(s.track.position);
        fill.setFillColor(kAccent);
        window.draw(fill);

        // Knob sitting at the end of the filled portion.
        sf::RectangleShape knob(sf::Vector2f(18.f, 26.f));
        knob.setPosition(sf::Vector2f(s.knobCenter.x - 9.f, cy - 13.f));
        knob.setFillColor(kKnob);
        knob.setOutlineColor(kTrackEdge);
        knob.setOutlineThickness(2.f);
        window.draw(knob);

        // "0%" ... "100%" readout, right-aligned to the window edge so it never
        // runs off the right side.
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(*s.value * 100.0f)));
        sf::Text out(mFont, buf, kItemSize);
        out.setFillColor(kAccent);
        const auto bounds = out.getLocalBounds();
        out.setOrigin(sf::Vector2f(bounds.size.x, bounds.size.y));
        out.setPosition(sf::Vector2f(kW - 16.f, s.track.position.y + 6.f));
        window.draw(out);
    }
}

} // namespace pimbalgame
