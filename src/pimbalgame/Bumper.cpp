#include "Bumper.hpp"
#include "Textures.hpp"
#include <algorithm>

namespace pimbalgame
{
namespace
{
    // How long the bumper stays "active" after a hit (prevents double scoring
    // and gives a visual pulse).
    constexpr float kFlashDuration = 0.35f;
}

Bumper::Bumper(sf::Vector2f position, float radius, int score, float kickSpeed)
    : mPosition(position)
    , mRadius(radius)
    , mScore(score)
    , mKickSpeed(kickSpeed)
    , mFlashTimer(0.0f)
{
    shape.setRadius(radius);
    shape.setOrigin(sf::Vector2f(radius, radius));
    shape.setFillColor(sf::Color(60, 200, 220));

    mRing.setRadius(radius + 6.f);
    mRing.setOrigin(sf::Vector2f(radius + 6.f, radius + 6.f));
    mRing.setFillColor(sf::Color::Transparent);
    mRing.setOutlineColor(sf::Color(120, 240, 255));
    mRing.setOutlineThickness(3.f);
    mRing.setPosition(position);
}

void Bumper::hit()
{
    mFlashTimer = kFlashDuration;
}

void Bumper::update(float dt)
{
    mFlashTimer = std::max(0.0f, mFlashTimer - dt);
    recompute();
}

void Bumper::recompute()
{
    shape.setPosition(mPosition);
    mRing.setPosition(mPosition);
}

void Bumper::render(sf::RenderWindow& window, const Textures& tex) const
{
    if (!tex.loaded())
    {
        // Fallback: plain disc + ring (should not happen with the atlas).
        sf::CircleShape lit = shape;
        if (mFlashTimer > 0.0f)
        {
            lit.setFillColor(sf::Color(210, 255, 255));
        }
        window.draw(lit);
        window.draw(mRing);
        return;
    }

    // The atlas "bumper" disc body has radius 36 texels, which maps to the
    // largest (reference) bumper; scale every bumper so its disc edge equals
    // its physics radius.
    const float scale = mRadius / 36.f;
    sf::Sprite s = tex.get("bumper");
    s.setOrigin(sf::Vector2f(50.f, 50.f));
    s.setPosition(mPosition);
    s.setScale(sf::Vector2f(scale, scale));
    s.setColor(mFlashTimer > 0.0f ? sf::Color(230, 255, 255) : sf::Color(255, 255, 255));
    window.draw(s);

    // Glowing halo while flashing.
    if (mFlashTimer > 0.0f)
    {
        sf::Sprite glow = tex.get("glow");
        const float glowScale = (mRadius + 16.f) / 20.f;
        glow.setOrigin(sf::Vector2f(32.f, 32.f));
        glow.setPosition(mPosition);
        glow.setScale(sf::Vector2f(glowScale, glowScale));
        glow.setColor(sf::Color(190, 255, 255, 150));

        // Additive glow: SFML 3.1 exposes the blend mode via RenderStates on
        // draw() (applyBlendMode() is private).
        window.draw(glow, sf::RenderStates(sf::BlendAdd));
    }
}

} // namespace pimbalgame
