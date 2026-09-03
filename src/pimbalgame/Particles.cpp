#include "Particles.hpp"
#include <algorithm>
#include <random>

namespace pimbalgame
{
namespace
{
    // Speed (px/s) above which the ball starts leaving a trail.
    constexpr float kTrailSpeed = 260.f;
    // Above this fraction of max speed the halo is fully bright.
    constexpr float kGlowSaturation = 900.f;
    std::mt19937& rng()
    {
        static thread_local std::mt19937 g{std::random_device{}()};
        return g;
    }
}

void Particles::emitBurst(sf::Vector2f pos, int count, const sf::Color& a, const sf::Color& b,
                          float minSpeed, float maxSpeed, float life,
                          float minRadius, float maxRadius)
{
    if (count <= 0)
    {
        return;
    }
    std::uniform_real_distribution<float> distAngle(0.0f, 2.0f * 3.14159265f);
    std::uniform_real_distribution<float> distSpeed(minSpeed, maxSpeed);
    std::uniform_real_distribution<float> distLife(0.6f, 1.0f);
    std::uniform_real_distribution<float> distSize(minRadius, maxRadius);
    std::uniform_int_distribution<int> distMix(0, 1);

    for (int i = 0; i < count; ++i)
    {
        const float angle = distAngle(rng());
        const float speed = distSpeed(rng());
        Particle p;
        p.pos = pos;
        p.vel = sf::Vector2f(std::cos(angle) * speed, std::sin(angle) * speed);
        p.maxLife = life * distLife(rng());
        p.life = p.maxLife;
        p.radius = distSize(rng());
        // Blend the two colours for a little variation.
        const int m = distMix(rng());
        p.color = sf::Color(a.r + (b.r - a.r) * (m * 0.5f),
                            a.g + (b.g - a.g) * (m * 0.5f),
                            a.b + (b.b - a.b) * (m * 0.5f),
                            a.a + (b.a - a.a) * (m * 0.5f));
        mParticles.push_back(std::move(p));
    }
}

void Particles::update(float dt)
{
    // Comet trail: emit a few sparks just behind the ball when it moves fast.
    if (mHasTexture && mBallSpeed > kTrailSpeed)
    {
        const float n = 1.0f + mBallSpeed / 240.0f;
        const int count = static_cast<int>(std::min<float>(n, 4.0f));
        const sf::Vector2f dir =
            mBallSpeed > 1e-4f ? mBallVel / mBallSpeed : sf::Vector2f{0.f, 0.f};

        std::uniform_real_distribution<float> distOffset(0.3f, 1.0f);
        std::uniform_real_distribution<float> distSpeed(30.f, 150.f);
        std::uniform_real_distribution<float> distLife(0.25f, 0.5f);
        std::uniform_real_distribution<float> distSize(1.4f, 3.0f);
        std::uniform_real_distribution<float> distStray(0.0f, 1.0f);

        for (int i = 0; i < count; ++i)
        {
            Particle p;
            const float off = distOffset(rng()) * mBallRadius;
            p.pos = mBallPos - dir * off;
            // A little lateral drift, mostly trailing the motion.
            p.vel = dir * distSpeed(rng()) +
                    sf::Vector2f(distStray(rng()) * 2.f - 1.f,
                                 distStray(rng()) * 2.f - 1.f) * 40.0f;
            p.maxLife = distLife(rng());
            p.life = p.maxLife;
            p.radius = distSize(rng());
            // Warm ember: from hot yellow to deep orange.
            const float t = distSize(rng()) / 3.0f;
            p.color = sf::Color(255,
                                static_cast<unsigned char>(180 - 90 * t),
                                static_cast<unsigned char>(60 - 40 * std::min(1.f, t)),
                                220);
            mParticles.push_back(std::move(p));
        }
    }

    // Integrate + age existing particles.
    for (auto& p : mParticles)
    {
        // Gentle drag so sparks settle.
        p.vel *= 1.0f / (1.0f + 1.6f * dt);
        p.pos += p.vel * dt;
        p.pos.y += 40.0f * dt;        // faint buoyant drift for embers
        p.life -= dt;
    }

    mParticles.erase(
        std::remove_if(mParticles.begin(), mParticles.end(),
                       [](const Particle& p) { return p.life <= 0.0f; }),
        mParticles.end());
}

void Particles::renderGlow(sf::RenderWindow& window) const
{
    if (!mHasTexture)
    {
        return;
    }
    const float f = std::min(1.0f, mBallSpeed / kGlowSaturation);
    // Halo grows and brightens with speed.
    const float scale = 1.05f + 0.75f * f;
    const unsigned char a = static_cast<unsigned char>(120 + 95 * f);
    sf::Sprite halo = *mGlow;
    halo.setPosition(mBallPos);
    halo.setScale(sf::Vector2f(scale, scale));
    halo.setColor(sf::Color(255, 255, 255, a));

    // Additive halo via RenderStates (applyBlendMode() is private in SFML 3.1).
    window.draw(halo, sf::RenderStates(sf::BlendAdd));
}

void Particles::renderEmbers(sf::RenderWindow& window) const
{
    if (mParticles.empty())
    {
        return;
    }
    // Additive embers via RenderStates (applyBlendMode() is private in SFML 3.1).
    sf::RenderStates additive(sf::BlendAdd);
    for (const auto& p : mParticles)
    {
        const float k = std::max(0.0f, p.life / p.maxLife);
        sf::CircleShape circle(std::max(0.1f, p.radius * (0.35f + 0.65f * k)));
        circle.setPosition(p.pos);
        const unsigned char a = static_cast<unsigned char>(p.color.a * k);
        circle.setFillColor(sf::Color(p.color.r, p.color.g, p.color.b, a));
        window.draw(circle, additive);
    }
}

} // namespace pimbalgame
