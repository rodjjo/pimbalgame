#include "pimbalgame/Flipper.hpp"
#include <algorithm>
#include <cmath>

namespace pimbalgame
{
namespace
{
    // How fast the flipper swings toward its target angle (rad/s).
    constexpr float kSwingSpeed = 12.0f;
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
    mBody.setSize(sf::Vector2f(mLength, 14.f));
    mBody.setOrigin(sf::Vector2f(0.f, 7.f));
    mBody.setFillColor(sf::Color(240, 180, 40));

    mCap.setRadius(8.f);
    mCap.setOrigin(sf::Vector2f(8.f, 8.f));
    mCap.setFillColor(sf::Color(220, 160, 30));

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
    mBody.setPosition(mPivot);
    mBody.setRotation(sf::radians(mAngle));
    mCap.setPosition(mPivot);
}

void Flipper::render(sf::RenderWindow& window) const
{
    window.draw(mBody);
    window.draw(mCap);
}

} // namespace pimbalgame
