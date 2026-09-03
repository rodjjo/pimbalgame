#include "pimbalgame/Ball.hpp"
#include "pimbalgame/Textures.hpp"
#include <cmath>

namespace pimbalgame
{
Ball::Ball()
{
    shape.setRadius(radius);
    shape.setOrigin(sf::Vector2f(radius, radius));
    shape.setFillColor(color);
}

void Ball::integrate(float dt, float gravity)
{
    velocity.y += gravity * dt;
    position += velocity * dt;
    clampSpeed();
}

void Ball::clampSpeed()
{
    const float sq = velocity.x * velocity.x + velocity.y * velocity.y;
    const float limit = maxSpeed * maxSpeed;
    if (sq > limit)
    {
        const float scale = maxSpeed / std::sqrt(sq);
        velocity *= scale;
    }
}

void Ball::render(sf::RenderWindow& window, const Textures& tex) const
{
    if (!tex.loaded())
    {
        sf::CircleShape s = shape;
        s.setPosition(position);
        window.draw(s);
        return;
    }

    sf::Sprite s = tex.get("ball");
    s.setOrigin(sf::Vector2f(24.f, 24.f));
    s.setPosition(position);
    const float scale = (2.f * radius) / 48.f;
    s.setScale(sf::Vector2f(scale, scale));
    s.setColor(sf::Color(255, 255, 255));
    window.draw(s);
}

} // namespace pimbalgame
