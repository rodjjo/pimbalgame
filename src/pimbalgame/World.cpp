#include "pimbalgame/World.hpp"
#include "pimbalgame/Physics.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace pimbalgame
{
namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;

    // ---- Table geometry (absolute pixels, designed for a 640x920 window) ----
    constexpr float kLeft = 50.f;
    constexpr float kRight = 590.f;
    constexpr float kChannelLeft = 540.f;
    constexpr float kChannelRight = 590.f;
    constexpr float kTop = 250.f;

    // Flipper pivots and angles (radians).
    constexpr sf::Vector2f kLeftPivot(200.f, 820.f);
    constexpr sf::Vector2f kRightPivot(440.f, 820.f);
    constexpr float kFlipperLength = 110.f;
    constexpr float kLeftRest = -15.0f * kDegToRad;
    constexpr float kLeftActive = -78.0f * kDegToRad;
    constexpr float kRightRest = 195.0f * kDegToRad;
    constexpr float kRightActive = 258.0f * kDegToRad;

    // Plunger (right-channel launcher).
    constexpr float kPlungerRestY = 835.f;
    constexpr float kPlungerMaxY = 865.f;
    constexpr float kDeppressSpeed = 320.f;   // px/s while held
    constexpr float kPlungerUpSpeed = 1000.f;  // px/s after release
    constexpr float kLaunchBase = 600.f;       // px/s
    constexpr float kLaunchExtra = 1400.f;     // px/s at full charge

    constexpr float kGravity = 1250.f;         // px/s^2
    constexpr float kFloorY = 885.f;           // ball drains below this
    // Minimum launch speed (px/s) imparted to the ball while it rests on an
    // active flipper, so it can never settle into the valley formed by the
    // flipper and the adjacent wall.
    constexpr float kMinLaunchSpeed = 300.f;
    constexpr float kBallSpawnX = 565.f;
    constexpr float kBallSpawnY = 340.f;

    constexpr float kWallTexW = 72.f;
    constexpr float kWallTexH = 16.f;
    constexpr float kWallThickness = 8.f;      // visual rail thickness (px)

    // A textured wall rail: origin at the left endpoint, scaled to the segment.
    void drawWall(sf::RenderWindow& window, const sf::Vector2f& a,
                  const sf::Vector2f& b, const Textures& tex)
    {
        const sf::Vector2f dir = b - a;
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len < 1e-3f)
        {
            return;
        }
        sf::Sprite s = tex.get("wall");
        s.setOrigin(sf::Vector2f(0.f, kWallTexH * 0.5f));
        s.setPosition(a);
        // setRotation takes an sf::Angle; the wall's geometric angle is atan2(dy, dx) in radians.
        s.setRotation(sf::radians(std::atan2(dir.y, dir.x)));
        s.setScale(sf::Vector2f(len / kWallTexW, kWallThickness / kWallTexH));
        window.draw(s);
    }

    constexpr float kPlungerTexW = 56.f;
    constexpr float kPlungerTexH = 26.f;
    constexpr float kPlungerDrawW = 42.f;
    constexpr float kPlungerDrawH = 20.f;
    constexpr float kChannelCenterX = (kChannelLeft + kChannelRight) * 0.5f;

    // A textured launch pad in the right channel; reddens as it is charged.
    void renderPlunger(sf::RenderWindow& window, float y, float charge, const Textures& tex)
    {
        sf::Sprite s = tex.get("plunger");
        s.setOrigin(sf::Vector2f(kPlungerTexW * 0.5f, kPlungerTexH * 0.5f));
        s.setPosition(sf::Vector2f(kChannelCenterX, y));
        s.setScale(sf::Vector2f(kPlungerDrawW / kPlungerTexW, kPlungerDrawH / kPlungerTexH));
        s.setColor(charge > 0.03f ? sf::Color(176, 74, 50) : sf::Color(255, 255, 255));
        window.draw(s);
    }
}

World::World(int /*windowWidth*/, int /*windowHeight*/)
{
    buildTable();
    mPlungerY = kPlungerRestY;

    // Decode the embedded atlas and wire the ball glow to it.
    if (mTextures.load())
    {
        // Pass the glow's own rectangle inside the atlas: the halo must draw
        // only the glow, not the whole atlas.
        mParticles.setGlowTexture(&mTextures.texture(), mTextures.rect("glow"));

        // Decorative skull watermark: centred on the bumper cluster.
        mSkull = mTextures.get("skull");
        mSkull->setOrigin(sf::Vector2f(150.f, 170.f));                 // centre of the 300x340 art
        mSkull->setPosition(sf::Vector2f(320.f, 402.f));
        mSkull->setScale(sf::Vector2f(0.80f, 0.80f));                  // ~240 px wide
        mSkull->setColor(sf::Color(255, 255, 255, 110)); // faint watermark
    }

    reset();
}

void World::reset()
{
    mScore = 0;
    mBalls = 3;
    mGameOver = false;
    resetBall();
}

void World::setLeftFlipper(bool active)
{
    mLeftFlipper = active;
    if (!mFlippers.empty())
    {
        mFlippers.front()->setActive(active);
    }
}

void World::setRightFlipper(bool active)
{
    mRightFlipper = active;
    if (mFlippers.size() > 1)
    {
        mFlippers.back()->setActive(active);
    }
}

void World::setPlungerHeld(bool held)
{
    mPlungerHeld = held;
}

void World::update(float dt)
{
    if (mGameOver)
    {
        for (auto& b : mBumpers)
        {
            b->update(dt);
        }
        return;
    }

    for (auto& f : mFlippers)
    {
        f->update(dt);
    }

    updatePlunger(dt);

    if (mPlungerCooldown > 0.0f)
    {
        mPlungerCooldown -= dt;
    }

    mBall.integrate(dt, kGravity);
    collideWalls();
    collideBumpers();
    collideFlippers();
    collidePlunger();
    checkDrain();

    for (auto& b : mBumpers)
    {
        b->update(dt);
    }

    // Feed the ball to the particle system and age the sparks (physics timestep,
    // so particle lifetimes are expressed in real seconds).
    mParticles.setBall(mBall.position, mBall.velocity, mBall.radius);
    mParticles.update(dt);
}

void World::render(sf::RenderWindow& window) const
{
    renderBackground(window);

    for (const auto& w : mWalls)
    {
        drawWall(window, w.a, w.b, mTextures);
    }

    // Plunger pad.
    renderPlunger(window, mPlungerY, mCharge, mTextures);

    for (auto& b : mBumpers)
    {
        b->render(window, mTextures);
    }
    for (auto& f : mFlippers)
    {
        f->render(window, mTextures);
    }

    // Ball halo (behind the ball), the ball itself, then trailing sparks.
    mParticles.renderGlow(window);
    mBall.render(window, mTextures);
    mParticles.renderEmbers(window);
}

void World::renderBackground(sf::RenderWindow& window) const
{
    if (mTextures.loaded() && mSkull)
    {
        window.draw(*mSkull);
    }
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------
void World::buildTable()
{
    // Rounded top via an arc.
    spawnArc(320.f, 362.5f, 292.5f, 202.6f, 337.4f, 20, 0.45f);

    // Containing walls and guides.
    addWall(kLeft, kTop, kLeft, 760.f);
    addWall(kLeft, 760.f, 155.f, 800.f);
    addWall(kChannelLeft, 480.f, kChannelLeft, 700.f);
    addWall(kChannelLeft, 700.f, 470.f, 800.f);
    addWall(kRight, kTop, kRight, 860.f);
    addWall(155.f, 800.f, kLeftPivot.x, kLeftPivot.y);
    addWall(470.f, 800.f, kRightPivot.x, kRightPivot.y);

    // Flippers.
    mFlippers.push_back(std::make_unique<Flipper>(
        Flipper::Side::Left, kLeftPivot, kFlipperLength, kLeftRest, kLeftActive));
    mFlippers.push_back(std::make_unique<Flipper>(
        Flipper::Side::Right, kRightPivot, kFlipperLength, kRightRest, kRightActive));

    // Bumpers.
    addBumper(sf::Vector2f(320.f, 300.f), 34.f, 100, 540.f);
    addBumper(sf::Vector2f(220.f, 430.f), 30.f, 100, 540.f);
    addBumper(sf::Vector2f(420.f, 430.f), 30.f, 100, 540.f);
    addBumper(sf::Vector2f(320.f, 500.f), 26.f, 150, 560.f);
}

void World::spawnArc(float cx, float cy, float r, float startDeg,
                     float endDeg, int segments, float restitution)
{
    sf::Vector2f prev;
    bool first = true;
    for (int i = 0; i <= segments; ++i)
    {
        const float a = (startDeg + (endDeg - startDeg) * i / segments) * kDegToRad;
        const sf::Vector2f p(cx + r * std::cos(a), cy + r * std::sin(a));
        if (!first)
        {
            addWall(prev.x, prev.y, p.x, p.y, restitution);
        }
        prev = p;
        first = false;
    }
}

void World::addWall(float ax, float ay, float bx, float by, float restitution)
{
    mWalls.push_back({sf::Vector2f(ax, ay), sf::Vector2f(bx, by), restitution});
}

void World::addBumper(sf::Vector2f position, float radius, int score, float kickSpeed)
{
    mBumpers.push_back(std::make_unique<Bumper>(position, radius, score, kickSpeed));
}

// ---------------------------------------------------------------------------
// Physics
// ---------------------------------------------------------------------------
void World::resolveSegment(const sf::Vector2f& a, const sf::Vector2f& b,
                           float restitution,
                           const std::optional<sf::Vector2f>& flipperVelocity)
{
    const sf::Vector2f closest = ClosestPointOnSegment(mBall.position, a, b);
    const sf::Vector2f diff = mBall.position - closest;
    const float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y);
    const float overlap = mBall.radius - dist;
    if (overlap <= 0.0f || dist < 1e-6f)
    {
        return;
    }

    const sf::Vector2f n = diff / dist;

    // Push the ball out of the surface.
    mBall.position += n * overlap;

    sf::Vector2f relative = mBall.velocity;
    if (flipperVelocity)
    {
        relative = mBall.velocity - *flipperVelocity;
    }

    const float vn = relative.x * n.x + relative.y * n.y;
    if (vn < 0.0f)
    {
        const float j = -(1.0f + restitution) * vn;
        relative += n * j;
    }

    mBall.velocity = flipperVelocity ? (relative + *flipperVelocity) : relative;
}

void World::collideWalls()
{
    for (const auto& w : mWalls)
    {
        resolveSegment(w.a, w.b, w.restitution);
    }
}

void World::collideBumpers()
{
    for (auto& b : mBumpers)
    {
        const sf::Vector2f diff = mBall.position - b->position();
        const float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        const float minDist = mBall.radius + b->radius();
        if (dist >= minDist || dist < 1e-6f)
        {
            continue;
        }

        const sf::Vector2f n = diff / dist;
        // Move the ball out of the bumper.
        mBall.position += n * (minDist - dist);

        // Reflect while applying a fixed radial kick (keeps tangential energy).
        const float vn = mBall.velocity.x * n.x + mBall.velocity.y * n.y;
        const sf::Vector2f vnVec = n * vn;
        const sf::Vector2f vt = mBall.velocity - vnVec;
        mBall.velocity = n * b->kickSpeed() + vt * 0.85f;

        if (!b->isFlashing())
        {
            b->hit();
            mScore += b->score();
            // Burst of sparks off the point where the ball meets the bumper.
            const sf::Vector2f contact = mBall.position - n * mBall.radius;
            mParticles.emitBurst(contact, 15, sf::Color(205, 255, 255),
                                 sf::Color(80, 205, 255), 220.f, 520.f, 0.5f, 2.f, 4.6f);
        }
    }
}

void World::collideFlippers()
{
    for (auto& f : mFlippers)
    {
        const sf::Vector2f closest = ClosestPointOnSegment(mBall.position, f->bodyA(), f->bodyB());
        resolveSegment(f->bodyA(), f->bodyB(), 0.55f, f->surfaceVelocity(closest));

        // Sparks when the ball slams into an active flipper at speed.
        const float speed = std::sqrt(mBall.velocity.x * mBall.velocity.x +
                                      mBall.velocity.y * mBall.velocity.y);
        if (f->isActive() && speed > 480.f)
        {
            mParticles.emitBurst(closest, 8, sf::Color(255, 215, 140),
                                 sf::Color(255, 150, 60), 120.f, 320.f, 0.4f, 1.6f, 3.4f);
        }

        // Anti-stick: when a flipper is held up the ball must never be able to
        // settle into the small valley formed by the flipper and the adjacent
        // wall (the pivot corner, where the flipper's linear velocity is ~0, so
        // swinging it would impart nothing). While the flipper is active, keep
        // the ball moving away from the surface at a minimum speed. This only
        // applies on genuine contact, so a flipper swinging nearby without
        // touching the ball never pushes it "from a distance".
        const sf::Vector2f diff = mBall.position - closest;
        const float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        if (f->isActive() && dist < mBall.radius && dist >= 1e-6f)
        {
            const sf::Vector2f n = diff / dist;
            const float vn = mBall.velocity.x * n.x + mBall.velocity.y * n.y;
            if (vn < kMinLaunchSpeed)
            {
                mBall.velocity += n * (kMinLaunchSpeed - vn);
            }
        }
    }
}

void World::collidePlunger()
{
    if (mPlungerCooldown > 0.0f)
    {
        return;
    }

    const float bx = mBall.position.x;
    if (bx <= kChannelLeft + 6.f || bx >= kChannelRight - 6.f)
    {
        return;
    }

    const float top = mPlungerY;
    if (mBall.position.y + mBall.radius > top)
    {
        const float penetration = mBall.position.y + mBall.radius - top;
        mBall.position.y -= penetration;
        if (mBall.velocity.y > 0.0f)
        {
            mBall.velocity.y = 0.0f; // rest on the pad
        }
    }
}

void World::updatePlunger(float dt)
{
    if (mPlungerHeld)
    {
        if (mPlungerY < kPlungerMaxY)
        {
            mPlungerY = std::min(kPlungerMaxY, mPlungerY + kDeppressSpeed * dt);
        }
        const float range = kPlungerMaxY - kPlungerRestY;
        mCharge = range > 0.0f ? (mPlungerY - kPlungerRestY) / range : 0.0f;
    }
    else
    {
        if (mPrevPlungerHeld && mCharge > 0.03f)
        {
            const float speed = kLaunchBase + mCharge * kLaunchExtra;
            mBall.velocity = sf::Vector2f(-150.f, -speed);
            mPlungerCooldown = 0.1f;
            // Kick up sparks from the channel on launch.
            mParticles.emitBurst(mBall.position, 10, sf::Color(255, 165, 85),
                                 sf::Color(255, 95, 45), 80.f, 260.f, 0.45f, 2.f, 4.f);
        }
        mCharge = 0.0f;
        if (mPlungerY > kPlungerRestY)
        {
            mPlungerY = std::max(kPlungerRestY, mPlungerY - kPlungerUpSpeed * dt);
        }
    }
    mPrevPlungerHeld = mPlungerHeld;
}

void World::checkDrain()
{
    if (mBall.position.y <= kFloorY)
    {
        return;
    }

    if (mBalls <= 1)
    {
        --mBalls;
        mGameOver = true;
    }
    else
    {
        --mBalls;
    }
    resetBall();
}

void World::resetBall()
{
    mBall.position = sf::Vector2f(kBallSpawnX, kBallSpawnY);
    mBall.velocity = sf::Vector2f(0.f, 0.f);
    mPlungerY = kPlungerRestY;
    mCharge = 0.0f;
}

} // namespace pimbalgame
