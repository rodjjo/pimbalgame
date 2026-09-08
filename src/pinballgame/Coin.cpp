#include "Coin.hpp"
#include "Textures.hpp"
#include <algorithm>
#include <cmath>

namespace pinballgame
{
namespace
{
    // Pixel <-> meter scale (shared with World; kept local so Coin.cpp is
    // self-contained).
    constexpr float kPpm = 100.0f;

    // The coin SVG's outer rim radius in texels. The sprite is scaled so that
    // this rim maps onto the physics radius, i.e. the drawn coin edge == radius.
    constexpr float kOuterTex = 27.f;
    constexpr float kHalfTex = 30.f;  // half of the 60x60 art side (origin centre)

    constexpr float kFlashDuration = 0.35f;
}

Coin::Coin(sf::Vector2f position, float radius)
    : mPosition(position)
    , mRadius(radius)
{
    shape.setRadius(radius);
    shape.setOrigin(sf::Vector2f(radius, radius));
    shape.setFillColor(sf::Color(218, 165, 32));
}

Coin::~Coin()
{
    destroy();
}

void Coin::create(b2WorldId world)
{
    if (mBody.index1 != 0)
    {
        return;
    }
    // Static: the coin must stay put on the table until it expires, so it can
    // never drift, get knocked into a wall or drain like a dynamic body would.
    b2BodyDef def = b2DefaultBodyDef();
    def.type = b2_staticBody;
    def.position = b2Vec2{ mPosition.x / kPpm, mPosition.y / kPpm };
    b2BodyId body = b2CreateBody(world, &def);
    mBody = body;

    b2ShapeDef sdef = b2DefaultShapeDef();
    sdef.material.friction = 0.0f;
    sdef.material.restitution = 0.0f;  // the redirect (not restitution) launches the ball
    b2Circle circle;
    circle.center = b2Vec2_zero;
    circle.radius = mRadius / kPpm;
    b2CreateCircleShape(body, &sdef, &circle);
}

void Coin::setUserData(void* data)
{
    if (mBody.index1 != 0)
    {
        b2Body_SetUserData(mBody, data);
    }
}

void Coin::destroy()
{
    if (mBody.index1 != 0)
    {
        b2DestroyBody(mBody);
        mBody = b2_nullBodyId;
    }
}

void Coin::update(float dt)
{
    mLife = std::max(0.0f, mLife - dt);
    mFlash = std::max(0.0f, mFlash - dt);
}

void Coin::hit()
{
    mFlash = kFlashDuration;
    mCollected = true;
}

void Coin::render(sf::RenderWindow& window, const Textures& tex) const
{
    // Additive glow behind the coin while it just got hit.
    if (mFlash > 0.0f && tex.loaded())
    {
        sf::Sprite glow = tex.get("glow");
        const float glowScale = (mRadius + 14.f) / 20.f;
        glow.setOrigin(sf::Vector2f(32.f, 32.f));
        glow.setPosition(mPosition);
        glow.setScale(sf::Vector2f(glowScale, glowScale));
        glow.setColor(sf::Color(255, 215, 120, 160));
        window.draw(glow, sf::RenderStates(sf::BlendAdd));
    }

    if (!tex.loaded())
    {
        // Procedural fallback: a gold disc with a darker rim.
        window.draw(shape);
        return;
    }

    const float scale = mRadius / kOuterTex;
    sf::Sprite s = tex.get("coin");
    s.setOrigin(sf::Vector2f(kHalfTex, kHalfTex));
    s.setPosition(mPosition);
    s.scale(sf::Vector2f(scale, scale));
    // Brighten briefly on hit.
    if (mFlash > 0.0f)
    {
        s.setColor(sf::Color(255, 245, 200));
    }
    window.draw(s);
}

} // namespace pinballgame
