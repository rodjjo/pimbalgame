#pragma once

#include <SFML/Graphics.hpp>

namespace pimbalgame
{
class Textures;

// A rotating flipper. It pivots around a fixed point and swings between a
// resting angle and an active angle. Its angular velocity is used to impart
// speed to the ball on contact.
class Flipper
{
public:
    enum class Side { Left, Right };

    Flipper(Side side, sf::Vector2f pivot, float length,
            float restAngleRad, float activeAngleRad);

    void setActive(bool active);
    bool isActive() const { return mActive; }
    void update(float dt);

    const sf::Vector2f& pivot() const { return mPivot; }
    const sf::Vector2f& tip() const { return mTip; }
    float angle() const { return mAngle; }
    float angularVelocity() const { return mAngularVelocity; }
    const sf::Vector2f& bodyA() const { return mPivot; }
    const sf::Vector2f& bodyB() const { return mTip; }

    // Linear velocity of the flipper surface at `contact`.
    sf::Vector2f surfaceVelocity(const sf::Vector2f& contact) const;

    // Draws the flipper as the atlas "flipper" texture, pivoted so its tip
    // coincides with bodyB(). Left and right flippers share one texture; the
    // left one is mirrored. Falls back to a plain bar if the atlas is absent.
    void render(sf::RenderWindow& window, const Textures& tex) const;

private:
    void recompute();

    Side mSide;
    sf::Vector2f mPivot;
    float mLength;
    float mRestAngle;
    float mActiveAngle;
    float mAngle;
    float mTargetAngle;
    float mAngularVelocity;
    bool mActive;

    sf::Vector2f mTip;
};

} // namespace pimbalgame
