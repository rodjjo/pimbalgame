#pragma once

#include <SFML/Graphics.hpp>

namespace pimbalgame
{

class Textures;

// The pinball. Holds its dynamic state plus a render shape.
struct Ball
{
    sf::Vector2f position{};
    sf::Vector2f velocity{};
    float radius = 9.f;
    float restitution = 0.5f;   // energy retained when bouncing off walls (0..1)
    float maxSpeed = 1500.f;    // speed clamp to keep physics stable
    sf::Color color{196, 206, 255};

    sf::CircleShape shape;      // fallback rendering (procedural)

    Ball();

    // Applies gravity and advances the position for one sub-step.
    void integrate(float dt, float gravity);

    // Limits the speed to maxSpeed.
    void clampSpeed();

    // Draws the ball as the atlas "ball" texture, centred on `position`,
    // falling back to the procedural circle when the atlas is unavailable.
    void render(sf::RenderWindow& window, const Textures& tex) const;
};

} // namespace pimbalgame
