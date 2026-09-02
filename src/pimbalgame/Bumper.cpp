#include "pimbalgame/Bumper.hpp"
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

void Bumper::render(sf::RenderWindow& window) const
{
    sf::CircleShape lit = shape;
    if (mFlashTimer > 0.0f)
    {
        lit.setFillColor(sf::Color(210, 255, 255));
    }
    window.draw(lit);
    window.draw(mRing);
}

} // namespace pimbalgame
