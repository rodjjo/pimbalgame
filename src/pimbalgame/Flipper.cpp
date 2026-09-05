#include "Flipper.hpp"
#include "Textures.hpp"
#include <algorithm>
#include <cmath>

namespace pimbalgame
{
namespace
{
    // How fast the flipper swings toward its target angle (rad/s).
    constexpr float kSwingSpeed = 12.0f;
    constexpr float kPi = 3.14159265358979323846f;

    // The flipper texture is drawn with the pivot (fat end) at this texel, so the
    // sprite origin is placed there to align the visual pivot with the physics
    // pivot. The tip sits ~110 texels away, matching the collision length.
    constexpr float kPivotTexX = 18.f;
    constexpr float kPivotTexY = 20.f;
}

Flipper::Flipper(Side side, sf::Vector2f pivot, float length,
                 float restAngleRad, float activeAngleRad)
    : mSide(side)
    , mPivot(pivot)
    , mLength(length)
    , mRestAngle(restAngleRad)
    , mActiveAngle(activeAngleRad)
    , mAngle(restAngleRad)
    , mTargetAngle(restAngleRad)
    , mActive(false)
{
    recompute();
}

void Flipper::setActive(bool active)
{
    if (mActive == active)
    {
        return;
    }
    mActive = active;
    mTargetAngle = active ? mActiveAngle : mRestAngle;
}

void Flipper::update(float dt)
{
    const float prev = mAngle;

    const float diff = mTargetAngle - mAngle;
    const float step = kSwingSpeed * dt;
    if (std::abs(diff) <= step)
    {
        mAngle = mTargetAngle;
    }
    else
    {
        mAngle += std::copysign(step, diff);
    }

    mAngularVelocity = dt > 0.0f ? (mAngle - prev) / dt : 0.0f;
    recompute();

    // When the flipper has settled at its target angle its own angular velocity
    // is zero -- but the kinematic Box2D body still carries the residual
    // velocity of the last moving frame. `b2Body_SetTargetTransform` bails out
    // (the requested velocity is below the sleep threshold) and leaves the body
    // velocity untouched, and kinematic bodies preserve their velocity across
    // steps (zero mass, no damping). That lingering spin would then smack the
    // ball on every contact, even with the flipper held still. Zero it so a
    // settled flipper acts as a genuine static wall. This only fires once the
    // swing is finished (mAngularVelocity == 0 only when prev == mAngle ==
    // mTargetAngle), so the momentum of an active swing is never lost.
    if (bodyId.index1 != 0 && mAngularVelocity == 0.0f)
    {
        b2Body_SetLinearVelocity(bodyId, b2Vec2_zero);
        b2Body_SetAngularVelocity(bodyId, 0.0f);
    }
}

sf::Vector2f Flipper::surfaceVelocity(const sf::Vector2f& contact) const
{
    const sf::Vector2f r = contact - mPivot;
    // 2D cross product of the scalar angular velocity with the radius vector:
    // v = omega x r = (-omega * r.y, omega * r.x). This is the velocity of the
    // flipper surface at the contact point and the momentum imparted to the ball.
    return sf::Vector2f(-mAngularVelocity * r.y, mAngularVelocity * r.x);
}

void Flipper::recompute()
{
    mTip = mPivot + sf::Vector2f(std::cos(mAngle), std::sin(mAngle)) * mLength;
}

void Flipper::render(sf::RenderWindow& window, const Textures& tex) const
{
    if (!tex.loaded())
    {
        // Fallback: a plain bar pivoted at mPivot.
        sf::RectangleShape body(sf::Vector2f(mLength, 14.f));
        body.setOrigin(sf::Vector2f(0.f, 7.f));
        body.setPosition(mPivot);
        body.setRotation(sf::radians(mAngle));
        body.setFillColor(sf::Color(240, 180, 40));
        window.draw(body);
        return;
    }

    sf::Sprite s = tex.get("flipper");
    s.setOrigin(sf::Vector2f(kPivotTexX, kPivotTexY));
    s.setPosition(mPivot);

    float rotation = mAngle;
    float scaleX = 1.0f;
    if (mSide == Side::Left)
    {
        // Mirror horizontally: the right flipper's local +x maps to screen -x,
        // which is equivalent to rotating by -(mAngle) then flipping.
        scaleX = -1.0f;
        rotation -= kPi;
    }
    s.setRotation(sf::radians(rotation));
    s.setScale(sf::Vector2f(scaleX, 1.0f));

    window.draw(s);
}

} // namespace pimbalgame
