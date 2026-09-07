#pragma once

#include <SFML/Graphics.hpp>

#include "box2d/box2d.h"

namespace pimbalgame
{
class Textures;

// A pickup coin that randomly appears on the open playfield and lasts a few
// seconds before disappearing. On contact with the ball it awards points,
// redirects the ball to a random direction at the flipper-tip speed, and
// briefly flashes to give visual feedback.
//
// The coin is a static Box2D circle so the ball genuinely collides with it and
// can be hit; the spawn/despawn timing and the contact effects are driven by
// World (see World.cpp). This class only owns the coin's own state and rendering.
class Coin
{
public:
    Coin() = default;
    Coin(sf::Vector2f position, float radius);

    ~Coin();

    // Create the Box2D collision body at the current position. Called once,
    // after the position is chosen, so the ball can collide with the coin.
    void create(b2WorldId world);

    // Tag the Box2D body with `data` (set once, after create) so contact
    // handling can identify the coin independently of the ball/flipper/bumper.
    void setUserData(void* data);

    // Destroy the Box2D body. Safe to call more than once.
    void destroy();

    // Age the life + flash timers by `dt`. Returns true once the coin has lived
    // its full duration and must be despawned.
    void update(float dt);

    // Register a ball hit: flash briefly and mark the coin collected so it is
    // despawned on the next update (a collected coin cannot be hit again).
    void hit();

    void render(sf::RenderWindow& window, const Textures& tex) const;

    bool active() const { return mBody.index1 != 0; }
    const sf::Vector2f& position() const { return mPosition; }
    float radius() const { return mRadius; }
    bool isFlashing() const { return mFlash > 0.0f; }
    // True once the coin has lived its full duration and is ready to despawn.
    bool isExpired() const { return mLife <= 0.0f; }
    // True once the coin has been collected by the ball and must be despawned.
    bool isCollected() const { return mCollected; }
    // Set how long the coin stays alive before it disappears (seconds).
    void setLife(float seconds) { mLife = seconds; }

    // Box2D body carrying the coin's collision circle. World creates/destroys it
    // (the coin does not own the world), but reads its pose back from here.
    b2BodyId bodyId() const { return mBody; }
    void setBodyId(b2BodyId id) { mBody = id; }

    sf::CircleShape shape;

private:
    sf::Vector2f mPosition;
    float mRadius;
    float mLife = 0.0f;       // seconds remaining before the coin disappears
    float mFlash = 0.0f;      // hit-flash timer (s)
    bool mCollected = false;  // set on hit so the coin is despawned next update

    b2BodyId mBody{b2_nullBodyId};
};

} // namespace pimbalgame
