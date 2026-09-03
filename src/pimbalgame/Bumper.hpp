#pragma once

#include <SFML/Graphics.hpp>

#include "box2d/box2d.h"

namespace pimbalgame
{
class Textures;

// A static, circular bumper. On contact with the ball it applies a fixed
// radial kick and awards points. It briefly flashes when hit.
class Bumper
{
public:
    Bumper(sf::Vector2f position, float radius, int score, float kickSpeed);

    void hit();   // register a ball contact (starts the flash/cooldown)
    void update(float dt);
    void render(sf::RenderWindow& window, const Textures& tex) const;

    const sf::Vector2f& position() const { return mPosition; }
    float radius() const { return mRadius; }
    int score() const { return mScore; }
    float kickSpeed() const { return mKickSpeed; }
    bool isFlashing() const { return mFlashTimer > 0.0f; }

    sf::CircleShape shape;

    // Box2D static body carrying the bumper's collision circle. The ball
    // contacts it and the World applies the radial kick on contact events.
    b2BodyId bodyId = b2_nullBodyId;

private:
    void recompute();

    sf::Vector2f mPosition;
    float mRadius;
    int mScore;
    float mKickSpeed;
    float mFlashTimer;

    sf::CircleShape mRing;
};

} // namespace pimbalgame
