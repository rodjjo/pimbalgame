#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>

namespace pimbalgame
{
// Returns the point on the segment [a, b] closest to `p`.
inline sf::Vector2f ClosestPointOnSegment(const sf::Vector2f& p,
                                          const sf::Vector2f& a,
                                          const sf::Vector2f& b)
{
    const sf::Vector2f ab = b - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    if (len2 < 1e-9f)
    {
        return a;
    }

    float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    return a + ab * t;
}

} // namespace pimbalgame
