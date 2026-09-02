#include "pimbalgame/Ball.hpp"
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

} // namespace pimbalgame
