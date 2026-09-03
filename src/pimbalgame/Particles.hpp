#pragma once

#include <SFML/Graphics.hpp>
#include <memory>
#include <vector>

namespace pimbalgame
{
// A lightweight additive-blended particle system used for the ball's visual
// flair: a soft halo that glows brighter as the ball speeds up, a comet-like
// trail while moving fast, and short bursts of sparks on bumper/flipper/hit and
// on the plunger launch.
//
// Rendering order matters: drawGlow() (the halo) must be issued BEFORE the ball
// sprite, and renderEmbers() (the sparks) AFTER it.
class Particles
{
public:
    Particles() = default;

    // Bind the glow sprite from the atlas. `rect` is the glow's rectangle inside
    // the atlas: passing the whole atlas here would draw every texture in the
    // atlas as the ball's halo, so callers must request the glow's own rect.
    // nullptr disables the halo.
    void setGlowTexture(const sf::Texture* tex, const sf::IntRect& rect)
    {
        if (tex)
        {
            const auto s = tex->getSize();
            (void)s;
            mGlow = std::make_unique<sf::Sprite>(*tex, sf::IntRect(sf::Vector2i(rect.position.x, rect.position.y),
                                                                   sf::Vector2i(rect.size.x, rect.size.y)));
            mGlow->setOrigin(sf::Vector2f(rect.size.x * 0.5f, rect.size.y * 0.5f));
            mHasTexture = true;
        }
        else
        {
            mGlow.reset();
            mHasTexture = false;
        }
    }

    // Feed the ball's state once per frame (drives the halo and trail).
    // `velocity` is used both for the trail direction and the glow intensity.
    void setBall(sf::Vector2f position, sf::Vector2f velocity, float radius)
    {
        mBallPos = position;
        mBallRadius = radius;
        mBallVel = velocity;
        mBallSpeed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
    }

    // Emit an instantaneous burst of sparks at `pos`.
    void emitBurst(sf::Vector2f pos, int count, const sf::Color& a, const sf::Color& b,
                   float minSpeed, float maxSpeed, float life, float minRadius, float maxRadius);

    // Advance one frame. `dt` is the render timestep.
    void update(float dt);

    void renderGlow(sf::RenderWindow& window) const;
    void renderEmbers(sf::RenderWindow& window) const;

    bool hasEmitters() const { return mHasTexture; }

private:
    struct Particle {
        sf::Vector2f pos{};
        sf::Vector2f vel{};
        float life = 0.f;
        float maxLife = 1.f;
        float radius = 2.f;
        sf::Color color{};
    };

    bool mHasTexture = false;
    sf::Vector2f mBallPos{};
    sf::Vector2f mBallVel{};
    float mBallSpeed = 0.f;
    float mBallRadius = 9.f;
    std::unique_ptr<sf::Sprite> mGlow;
    std::vector<Particle> mParticles;
};

} // namespace pimbalgame
