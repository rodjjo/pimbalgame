#include "pinballgame/World.hpp"
#include "pinballgame/Physics.hpp"
#include "box2d/box2d.h"
#include "box2d/math_functions.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace pinballgame
{
namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;

    // Scale between the game's pixel space and Box2D's meter space. Box2D is
    // tuned for objects roughly 0.1..10 m moving at a few tens of m/s; mapping
    // the ~640x920 px table onto ~6.4x9.2 m keeps the ball, flippers and
    // bumpers in that comfortable range and makes the solver stable.
    constexpr float kPpm = 100.0f;

    // The pinball's top speed (px/s), mirrored into the world's velocity cap.
    constexpr float kBallMaxSpeed = 1500.f;

    // Marker stored as the ball's Box2D user data so contact events can tell the
    // ball apart from bumpers (whose user data is the Bumper* itself).
    static const int kBallTag = 0;

    // ---- Table geometry (absolute pixels, designed for a 640x920 window) ----
    constexpr float kLeft = 50.f;
    constexpr float kRight = 590.f;
    constexpr float kChannelLeft = 540.f;
    constexpr float kChannelRight = 590.f;
    constexpr float kTop = 250.f;

    // Flipper pivots and angles (radians). Each pivot sits a little BELOW the
    // end of its adjacent wall: the wall keeps its endpoint at kFlipperWallEndY
    // (the height the pivots used to share), while the pivot drops kFlipperPivotDrop
    // further down. That leaves the wall end overhanging the flipper, so a ball
    // rolling down the wall drops onto the TOP of the resting flipper body
    // instead of wedging into the pivot/wall corner -- where the flipper's linear
    // velocity is ~0 and it could impart nothing to a settled ball.
    constexpr float kFlipperWallEndY = 825.f;      // where the adjacent wall ends
    constexpr float kFlipperPivotDrop = 10.f;       // pivot placed this far under the wall
    constexpr sf::Vector2f kLeftPivot(200.f, kFlipperWallEndY + kFlipperPivotDrop);
    constexpr sf::Vector2f kRightPivot(440.f, kFlipperWallEndY + kFlipperPivotDrop);
    constexpr float kFlipperLength = 110.f;
    // Thickness of the flipper collision box (matches the flipper sprite).
    constexpr float kFlipperThickness = 26.f;

    // Anti-stick guard reach, measured from the flipper's pivot->tip centre-line.
    // The collision box is centred on that line, so the ball (radius
    // mBall.radius) actually rests against the box SURFACE when its centre is
    // mBall.radius + kFlipperThickness/2 beyond the line. The guard engages at
    // that surface distance (+ a little slack). The old `dist < mBall.radius`
    // test measured to the centre-line but compared against the ball radius,
    // i.e. it only matched inside the flipper body, so it never fired on contact
    // and the ball could settle forever in the pivot/wall valley.
    constexpr float kPivotPocketSlack = 8.0f;
    constexpr float kLeftRest = 25.0f * kDegToRad;
    constexpr float kLeftActive = -78.0f * kDegToRad;
    constexpr float kRightRest = 155.0f * kDegToRad;
    constexpr float kRightActive = 258.0f * kDegToRad;

    // Plunger (right-channel launcher).
    constexpr float kPlungerRestY = 835.f;
    constexpr float kPlungerMaxY = 865.f;
    constexpr float kDeppressSpeed = 320.f;   // px/s while held
    constexpr float kPlungerUpSpeed = 1000.f;  // px/s after release
    constexpr float kLaunchBase = 600.f;       // px/s
    constexpr float kLaunchExtra = 1400.f;     // px/s at full charge
    constexpr float kPlungerHalfW = 22.f;      // half width of the launch pad
    constexpr float kPlungerHalfH = 10.f;      // half thickness of the launch pad

    constexpr float kGravity = 1250.f;         // px/s^2
    constexpr float kFloorY = 885.f;           // ball drains below this
    // Minimum launch speed (px/s) imparted to the ball while it rests on an
    // active flipper, so it can never settle into the valley formed by the
    // flipper and the adjacent wall (the pivot corner, where the flipper's linear velocity is ~0, so
    // swinging it would impart nothing).
    constexpr float kMinLaunchSpeed = 300.f;
    constexpr float kBallSpawnX = 565.f;
    // The valve closes the channel mouth, so a new ball cannot drop in from
    // above; it is seated on the plunger pad (rest pad top 835, ball radius 9).
    constexpr float kBallSpawnY = 825.f;

    constexpr float kWallTexW = 72.f;
    constexpr float kWallTexH = 16.f;
    constexpr float kWallThickness = 8.f;      // visual rail thickness (px)

    // --- One-way flap valve across the mouth of the launch channel ---------
    // The launch channel is the vertical lane x in [kChannelLeft, kChannelRight]
    // on the right edge. Its left wall (x = kChannelLeft) ends at y = 480, so
    // the lane's mouth toward the table is that gap above the wall's top end:
    // a launched ball rises out through it, and any ball that later falls back
    // through it funnels straight onto the plunger. The valve is a plate hinged
    // at its lower end on top of the wall below -- the wall's top endpoint
    // (kChannelLeft, 480) IS the valve pivot -- that lies diagonally across the
    // mouth up to the right rail, closing the mouth entirely. The joint only
    // lets the plate swing further up (out of the lane): the rising ball meets
    // its underside at a glancing angle, slides along it and pushes it open as
    // it launches; gravity then seats it shut again, and anything pressing the
    // plate DOWN (a return toward the launcher) meets the closed limit instead
    // -- a one-way gate that keeps in-play balls out of the channel. Pixels.
    constexpr float kFlapHingeX = kChannelLeft;   // top end of the wall below
    constexpr float kFlapHingeY = 480.f;          // the valve pivot
    constexpr float kFlapLength = 150.f;          // reaches from the pivot to the rail
    constexpr float kFlapThickness = 12.f;        // plate thickness (px)
    constexpr float kFlapDensity = 0.15f;         // light so the ball swings it open
    constexpr float kFlapFriction = 0.05f;        // low: the ball slides over it

    // Plate angles (world radians; plate local +x runs along the plate from the
    // hinge end). At rest the plate leans ~70 deg up toward the rail end, so a
    // rising launch ball strikes its underside at a shallow angle and slides up
    // over it while the plate swings open. Gravity presses the free end down
    // onto the closed seat (the upper limit); the plate can only swing open
    // further upward from there, never down into the lane.
    constexpr float kFlapRestAngle = -1.2217f;           // ~-70 deg (70 deg up)
    constexpr float kFlapMaxAngle = kFlapRestAngle;              // closed seat
    constexpr float kFlapMinAngle = kFlapRestAngle - 0.85f;      // swings open up
    constexpr float kFlapSpringHertz = 5.0f;
    constexpr float kFlapDampingRatio = 0.9f;

    constexpr float kValveTexW = 80.f;
    constexpr float kValveTexH = 16.f;
    constexpr float kValveHingeTexX = 2.f;       // texel x of the plate's hinge end

    // Distinct marker for the flap's Box2D body so contact handling can tell it
    // apart from the ball (kBallTag) and bumpers (Bumper*).
    static const int kFlapTag = 1;

    // --- Coin pickup ---------------------------------------------------------
    // A coin randomly appears on the open playfield, lasts a few seconds, and
    // on contact awards points, redirects the ball to a random direction at the
    // flipper-tip speed, then disappears. Pixels.
    constexpr float kCoinRadius = 20.f;         // coin collision radius
    constexpr int kCoinScore = 2000;              // points awarded on hit
    constexpr float kCoinMinLife = 5.0f;        // shortest coin lifetime
    constexpr float kCoinMaxLife = 6.0f;        // longest coin lifetime
    constexpr float kCoinMinSpawn = 8.0f;       // shortest gap between coins
    constexpr float kCoinMaxSpawn = 10.0f;      // longest gap between coins
    constexpr float kCoinPad = 18.f;            // clearance kept from walls/objects

    // Distinct marker for the coin's Box2D body so contact handling can tell it
    // apart from the ball (kBallTag), the flap (kFlapTag) and bumpers (Bumper*).
    static const int kCoinTag = 2;

    // --- pixel <-> meter helpers ---
    inline b2Vec2 toM(sf::Vector2f p) { return b2Vec2{ p.x / kPpm, p.y / kPpm }; }
    inline b2Vec2 toM(float x, float y) { return b2Vec2{ x / kPpm, y / kPpm }; }
    inline sf::Vector2f toPx(b2Vec2 p) { return sf::Vector2f(p.x * kPpm, p.y * kPpm); }

    // Box2D body/shape ids are opaque handles with no operator==; bodies created
    // in one world carry a unique, stable `index`, so compare that.
    inline bool sameBody(b2BodyId a, b2BodyId b)
    {
        return a.index1 == b.index1;
    }

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
    // The physics world: downward gravity (same +y axis as the screen),
    // continuous collision so the fast ball never tunnels, and a speed cap.
    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = b2Vec2{ 0.f, kGravity / kPpm };
    worldDef.enableContinuous = true;
    worldDef.maximumLinearSpeed = kBallMaxSpeed / kPpm;
    mWorld = b2CreateWorld(&worldDef);

    buildTable();

    // The coin imparts the flipper tip's peak linear speed when the ball hits
    // it ("the same speed it would gain if it was hit by the tip of the
    // flipper"). Use the fastest flipper so the kick is a fair reference.
    for (const auto& f : mFlippers)
    {
        mFlipperTipSpeed = std::max(mFlipperTipSpeed, f->peakTipSpeed());
    }
    if (mFlipperTipSpeed <= 0.0f)
    {
        mFlipperTipSpeed = kFlipperLength * 12.0f;  // safety fallback (rad/s * px)
    }

    // First coin appears after a random 8..10 s (the "appears every 8-10s").
    std::uniform_real_distribution<float> spawnDist(kCoinMinSpawn, kCoinMaxSpawn);
    mCoinSpawnTimer = spawnDist(mRng);

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
    const bool wasHeld = mPlungerHeld;
    mPlungerHeld = held;

    // Edge-triggered so the plunger plays once per pull / release, not every
    // frame while the key is held down.
    if (mSound)
    {
        if (held && !wasHeld)
        {
            mSound->play("plunger_down");
        }
        else if (!held && wasHeld)
        {
            mSound->play("plunger_up");
        }
    }
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

    // Advance the flippers (this updates each flipper's angle / angular velocity).
    for (auto& f : mFlippers)
    {
        f->update(dt);
    }

    updatePlunger(dt);

    if (mPlungerCooldown > 0.0f)
    {
        mPlungerCooldown -= dt;
    }

    // Drive the kinematic flippers so Box2D's solver transfers their swing
    // momentum to the ball (replaces the old manual surface-velocity impulse).
    for (auto& f : mFlippers)
    {
        b2Transform target;
        target.p = toM(f->pivot());
        target.q = b2MakeRot(f->angle());
        b2Body_SetTargetTransform(f->bodyId, target, dt);
    }

    // Track the launch pad with the visual so the ball rests on the real surface.
    {
        b2Transform target;
        target.p = toM(kChannelCenterX, mPlungerY + kPlungerHalfH);
        target.q = b2MakeRot(0.f);
        b2Body_SetTransform(mPlungerBody, target.p, target.q);
    }

    // Advance the simulation: Box2D integrates the ball and resolves every
    // collision (walls, flippers, bumpers, plunger) for this sub-step batch.
    b2World_Step(mWorld, dt, mSubSteps);

    // Read the ball's state back from Box2D (meters -> pixels).
    mBall.position = toPx(b2Body_GetPosition(mBallBody));
    mBall.velocity = toPx(b2Body_GetLinearVelocity(mBallBody));

    // Turn contact events into bumper kicks/scoring, then flipper effects.
    processContacts();
    applyFlipperEffects();

    // Push any velocity changes (bumper kicks / anti-stick / coin redirect) back
    // into the body.
    b2Body_SetLinearVelocity(mBallBody, toM(mBall.velocity));

    updateCoin(dt);

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

    // One-way flap valve at the channel mouth.
    renderFlap(window);

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

    // The pickup coin (if one is currently alive).
    if (mCoin.active())
    {
        mCoin.render(window, mTextures);
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

void World::renderFlap(sf::RenderWindow& window) const
{
    if (!mFlapBody.index1)
    {
        return;
    }

    // The plate's hinge-end lower corner (its pivot, on the wall's top end) is
    // fixed by the revolute joint; recover its world point from the live pose
    // so the sprite always pivots where the physics plate does.
    const b2Transform tf = b2Body_GetTransform(mFlapBody);
    const float c = tf.q.c;
    const float s = tf.q.s;
    const float halfT = kFlapThickness * 0.5f / kPpm;
    const sf::Vector2f hinge = toPx(b2Body_GetPosition(mFlapBody)) +
                               sf::Vector2f(-s * halfT, c * halfT) * kPpm;
    const float ang = b2Rot_GetAngle(tf.q);

    if (mTextures.loaded())
    {
        sf::Sprite plate = mTextures.get("valve");
        // Origin at the plate's hinge-end lower corner (the pivot): rotation r
        // maps the sprite's +x axis to (cos r, sin r) in world pixels, like the
        // physics plate whose local +x runs along it from the hinge corner.
        plate.setOrigin(sf::Vector2f(kValveHingeTexX, kValveTexH));
        plate.setPosition(hinge);
        plate.setRotation(sf::radians(ang));
        plate.setScale(sf::Vector2f(kFlapLength / kValveTexW, kFlapThickness / kValveTexH));
        window.draw(plate);
    }

    // Hinge pin drawn over the pivot on the wall's top end: a dark disc + bore.
    sf::CircleShape outer(7.f);
    outer.setOrigin(sf::Vector2f(7.f, 7.f));
    outer.setPosition(hinge);
    outer.setFillColor(sf::Color(58, 67, 90));
    window.draw(outer);

    sf::CircleShape bore(3.f);
    bore.setOrigin(sf::Vector2f(3.f, 3.f));
    bore.setPosition(hinge);
    bore.setFillColor(sf::Color(29, 35, 48));
    window.draw(bore);
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------
void World::buildTable()
{
    // Shared static body carrying every wall as a two-sided segment. One body
    // keeps the broad-phase small; each segment keeps its own restitution.
    b2BodyDef wallDef = b2DefaultBodyDef();
    wallDef.type = b2_staticBody;
    mWallBody = b2CreateBody(mWorld, &wallDef);

    // Rounded top via an arc.
    spawnArc(320.f, 362.5f, 292.5f, 202.6f, 337.4f, 20, 0.45f);

    // Containing walls and guides.
    addWall(kLeft, kTop, kLeft, 760.f);
    addWall(kLeft, 760.f, 155.f, 800.f);
    addWall(kChannelLeft, 480.f, kChannelLeft, 700.f);
    addWall(kChannelLeft, 700.f, 470.f, 800.f);
    // Vertical left wall of the plunger launch lane. The right side has the
    // kRight rail, but below y=700 the left side was only the diagonal above,
    // so a ball returning down the lane could slip left and drain. This keeps
    // the lane sealed straight onto the pad so the ball rests on the plunger.
    addWall(kChannelLeft, 700.f, kChannelLeft, 865.f);
    addWall(kRight, kTop, kRight, 860.f);
    // These walls end at kFlipperWallEndY (unchanged). The pivot now sits below
    // this endpoint, so the wall overhangs and the ball falls onto the flipper.
    addWall(155.f, 800.f, kLeftPivot.x, kFlipperWallEndY);
    addWall(470.f, 800.f, kRightPivot.x, kFlipperWallEndY);

    // Flippers: kinematic bodies whose swing the solver transfers to the ball.
    {
        b2BodyDef fdef = b2DefaultBodyDef();
        fdef.type = b2_kinematicBody;
        fdef.fixedRotation = false;

        b2ShapeDef sdef = b2DefaultShapeDef();
        sdef.material.friction = 0.35f;
        const float hw = kFlipperLength * 0.5f / kPpm;
        const float hh = kFlipperThickness * 0.5f / kPpm;

        auto makeFlipper = [&](Flipper::Side side, sf::Vector2f pivot,
                               float restAngle, float activeAngle)
        {
            auto f = std::make_unique<Flipper>(side, pivot, kFlipperLength, restAngle, activeAngle);
            fdef.position = toM(pivot);
            fdef.rotation = b2MakeRot(f->angle());
            b2BodyId body = b2CreateBody(mWorld, &fdef);

            // Thin box from the pivot to the tip (local origin at the pivot).
            b2Polygon box = b2MakeOffsetBox(hw, hh, b2Vec2{ hw, 0.f }, b2MakeRot(0.f));
            b2ShapeId shape = b2CreatePolygonShape(body, &sdef, &box);
            b2Shape_SetRestitution(shape, 0.2f);

            f->bodyId = body;
            mFlippers.push_back(std::move(f));
        };

        makeFlipper(Flipper::Side::Left, kLeftPivot, kLeftRest, kLeftActive);
        makeFlipper(Flipper::Side::Right, kRightPivot, kRightRest, kRightActive);
    }

    // Bumpers: static discs. The ball contacts them and World::processContacts
    // applies the radial kick + scoring (Box2D resolves the geometry).
    addBumper(sf::Vector2f(320.f, 300.f), 34.f, 100, 540.f);
    addBumper(sf::Vector2f(220.f, 430.f), 30.f, 100, 540.f);
    addBumper(sf::Vector2f(420.f, 430.f), 30.f, 100, 540.f);
    addBumper(sf::Vector2f(320.f, 500.f), 26.f, 150, 560.f);

    // Plunger: static launch pad in the right channel, repositioned each frame.
    {
        b2BodyDef pdef = b2DefaultBodyDef();
        pdef.type = b2_staticBody;
        mPlungerBody = b2CreateBody(mWorld, &pdef);

        b2ShapeDef pshapeDef = b2DefaultShapeDef();
        pshapeDef.material.friction = 0.1f;
        const float hw = kPlungerHalfW / kPpm;
        const float hh = kPlungerHalfH / kPpm;
        b2Polygon pad = b2MakeBox(hw, hh);
        b2ShapeId shape = b2CreatePolygonShape(mPlungerBody, &pshapeDef, &pad);
        b2Shape_SetRestitution(shape, 0.0f);
    }

    // One-way flap valve across the mouth of the launch channel. The plate is
    // hinged at its hinge-end lower corner, which sits exactly on the wall's
    // top endpoint (kFlapHingeX, kFlapHingeY) = the valve pivot, and leans up
    // across the mouth to the right rail. The joint only lets it swing further
    // upward (out of the channel): the launched ball meets its underside at a
    // glancing angle and slides up over it as it opens, gravity seats it shut,
    // and any ball pressing the plate downward (a return toward the launcher)
    // meets the closed limit instead.
    {
        const float cosA = std::cos(kFlapRestAngle);
        const float sinA = std::sin(kFlapRestAngle);

        b2BodyDef fdef = b2DefaultBodyDef();
        fdef.type = b2_dynamicBody;
        fdef.enableSleep = false;
        // The plate is hinged at its hinge-end LOWER corner, which sits exactly
        // on the pivot (the top endpoint of the wall below). Anchoring at that
        // corner keeps every part of the plate at or above the pivot, so the
        // wall below can never touch or obstruct the plate and it can swing
        // fully open over the channel mouth.
        const float halfT = kFlapThickness * 0.5f / kPpm;
        const b2Vec2 hingeCorner = b2Vec2{ 0.f, halfT };   // lower corner, local
        const b2Vec2 worldCorner{
            cosA * hingeCorner.x - sinA * hingeCorner.y,
            sinA * hingeCorner.x + cosA * hingeCorner.y
        };
        fdef.position = toM(kFlapHingeX, kFlapHingeY) - worldCorner;
        fdef.rotation = b2MakeRot(kFlapRestAngle);   // closed pose across the mouth
        mFlapBody = b2CreateBody(mWorld, &fdef);
        b2Body_SetUserData(mFlapBody, (void*)&kFlapTag);

        b2ShapeDef sdef = b2DefaultShapeDef();
        sdef.material.friction = kFlapFriction;
        sdef.material.restitution = 0.0f;
        sdef.density = kFlapDensity;
        const float halfL = kFlapLength * 0.5f / kPpm;
        // Box centred at (halfL, 0) so it spans local x in [0, 2*halfL] = the
        // plate length, with the hinge end at the body's local x = 0.
        b2Polygon box = b2MakeOffsetBox(halfL, halfT, b2Vec2{ halfL, 0.f }, b2MakeRot(0.f));
        b2ShapeId shape = b2CreatePolygonShape(mFlapBody, &sdef, &box);
        b2Shape_SetRestitution(shape, 0.0f);

        // Hinge to the static wall body at the top of the wall below. Gravity
        // presses the free end down onto the upper (closed) limit; the lower
        // limit only allows the plate to swing open upward. The spring helps it
        // seat crisply at the closed pose and damps the return swing.
        b2RevoluteJointDef jdef = b2DefaultRevoluteJointDef();
        jdef.bodyIdA = mWallBody;
        jdef.bodyIdB = mFlapBody;
        jdef.localAnchorA = toM(kFlapHingeX, kFlapHingeY);
        jdef.localAnchorB = hingeCorner;
        jdef.referenceAngle = 0.f;
        jdef.enableLimit = true;
        jdef.lowerAngle = kFlapMinAngle;
        jdef.upperAngle = kFlapMaxAngle;
        jdef.enableSpring = true;
        jdef.hertz = kFlapSpringHertz;
        jdef.dampingRatio = kFlapDampingRatio;
        jdef.targetAngle = kFlapRestAngle;
        jdef.collideConnected = false;
        mFlapJoint = b2CreateRevoluteJoint(mWorld, &jdef);
    }

    // The ball: a dynamic, bullet circle so it cannot tunnel through the
    // swinging flippers. Restitution 0 means each surface's restitution
    // (via the max-mix rule) governs the bounce, matching the old per-wall code.
    {
        b2BodyDef bdef = b2DefaultBodyDef();
        bdef.type = b2_dynamicBody;
        bdef.isBullet = true;
        bdef.enableSleep = false;   // the ball must always stay responsive
        bdef.position = toM(kBallSpawnX, kBallSpawnY);
        bdef.linearVelocity = b2Vec2_zero;
        mBallBody = b2CreateBody(mWorld, &bdef);
        b2Body_SetUserData(mBallBody, (void*)&kBallTag);

        b2ShapeDef bshapeDef = b2DefaultShapeDef();
        bshapeDef.density = 1.0f;
        bshapeDef.material.friction = 0.1f;
        bshapeDef.material.restitution = 0.0f;
        bshapeDef.enableContactEvents = true;   // emit ball<->anything contact events
        b2Circle circle;
        circle.center = b2Vec2_zero;
        circle.radius = mBall.radius / kPpm;
        b2CreateCircleShape(mBallBody, &bshapeDef, &circle);
    }
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

    // Recreate the wall's Box2D segment with the same restitution.
    b2Segment seg;
    seg.point1 = toM(ax, ay);
    seg.point2 = toM(bx, by);
    b2ShapeDef sdef = b2DefaultShapeDef();
    sdef.material.friction = 0.1f;
    b2ShapeId shape = b2CreateSegmentShape(mWallBody, &sdef, &seg);
    b2Shape_SetRestitution(shape, restitution);
}

void World::addBumper(sf::Vector2f position, float radius, int score, float kickSpeed)
{
    auto bumper = std::make_unique<Bumper>(position, radius, score, kickSpeed);

    b2BodyDef bdef = b2DefaultBodyDef();
    bdef.type = b2_staticBody;
    bdef.position = toM(position);
    bumper->bodyId = b2CreateBody(mWorld, &bdef);
    b2Body_SetUserData(bumper->bodyId, (void*)bumper.get());

    b2ShapeDef sdef = b2DefaultShapeDef();
    sdef.material.friction = 0.05f;
    sdef.material.restitution = 0.0f;   // the kick (not restitution) launches the ball
    b2Circle circle;
    circle.center = b2Vec2_zero;
    circle.radius = radius / kPpm;
    b2ShapeId shape = b2CreateCircleShape(bumper->bodyId, &sdef, &circle);
    (void)shape;

    mBumpers.push_back(std::move(bumper));
}

// ---------------------------------------------------------------------------
// Physics
// ---------------------------------------------------------------------------
void World::processContacts()
{
    const b2ContactEvents events = b2World_GetContactEvents(mWorld);
    for (int i = 0; i < events.beginCount; ++i)
    {
        const b2ContactBeginTouchEvent& e = events.beginEvents[i];
        const b2BodyId bodyA = b2Shape_GetBody(e.shapeIdA);
        const b2BodyId bodyB = b2Shape_GetBody(e.shapeIdB);
        void* const ua = b2Body_GetUserData(bodyA);
        void* const ub = b2Body_GetUserData(bodyB);

        // Ball <-> flap valve: resolved by Box2D only (no kick, no scoring).
        // Route it out here like any other surface contact, so the bumper
        // reinterpretation below never mistakes the flap's integer tag for a
        // Bumper* pointer.
        if ((ua == (void*)&kBallTag && ub == (void*)&kFlapTag) ||
            (ub == (void*)&kBallTag && ua == (void*)&kFlapTag))
        {
            if (mSound)
            {
                mSound->play("ball_hit_wall", mBall.velocity.length());
            }
            continue;
        }

        // Ball <-> coin: award points, redirect the ball to a random direction
        // at the flipper-tip speed, and flash the coin. Gated by the coin's
        // flash cooldown so a single contact cannot score / redirect twice.
        const bool hitCoin = (ua == (void*)&kBallTag && ub == (void*)&kCoinTag) ||
                             (ub == (void*)&kBallTag && ua == (void*)&kCoinTag);
        if (hitCoin)
        {
            if (mCoin.active() && !mCoin.isFlashing())
            {
                const sf::Vector2f diff = mBall.position - mCoin.position();
                const float dist = diff.length();
                // A genuine contact: the ball's centre is about radius+ballRadius
                // from the coin centre. Guard against a stray far match.
                if (dist <= mCoin.radius() + mBall.radius + 2.0f && dist >= 1e-6f)
                {
                    // Same speed as a flipper-tip hit, but forced to a random
                    // direction at any angle.
                    const float speed = mFlipperTipSpeed;
                    std::uniform_real_distribution<float> angDist(-kPi, kPi);
                    const float ang = angDist(mRng);
                    mBall.velocity = sf::Vector2f(std::cos(ang), std::sin(ang)) * speed;

                    mCoin.hit();
                    mScore += kCoinScore;
                    if (mSound)
                    {
                        mSound->play("ball_hit_coin", mBall.velocity.length());
                    }
                    // Burst of gold sparks at the contact point.
                    const sf::Vector2f contact = mBall.position - diff / dist * mBall.radius;
                    mParticles.emitBurst(contact, 12, sf::Color(255, 215, 120),
                                         sf::Color(255, 160, 40), 60.f, 220.f, 0.4f, 1.4f, 4.0f);
                }
            }
            continue;
        }

        // Only ball <-> bumper contacts carry a non-null, non-ball user data.
        Bumper* bumper = nullptr;
        if (ua == (void*)&kBallTag && ub != nullptr && ub != (void*)&kBallTag)
        {
            bumper = static_cast<Bumper*>(ub);
        }
        else if (ub == (void*)&kBallTag && ua != nullptr && ua != (void*)&kBallTag)
        {
            bumper = static_cast<Bumper*>(ua);
        }
        else if (ua == (void*)&kBallTag || ub == (void*)&kBallTag)
        {
            // Ball hit a non-bumper surface (wall, flipper or launch pad).
            const bool hitWall = (sameBody(bodyA, mWallBody) || sameBody(bodyB, mWallBody));
            bool hitFlipper = false;
            for (const auto& f : mFlippers)
            {
                if (sameBody(f->bodyId, bodyA) || sameBody(f->bodyId, bodyB))
                {
                    hitFlipper = true;
                    break;
                }
            }
            if (mSound)
            {
                if (hitWall)
                {
                    mSound->play("ball_hit_wall", mBall.velocity.length());
                }
                else if (hitFlipper)
                {
                    mSound->play("ball_hit_flipper", mBall.velocity.length());
                }
            }
            // A non-bumper contact (wall / flipper / launch pad) is already
            // resolved by Box2D, and there is no bumper kick or scoring to apply.
            // Skip the bumper code below, which assumes a non-null `bumper`.
            continue;
        }
        else
        {
            continue;
        }

        // Both shapes are circles, so the contact normal is the line between centers.
        const sf::Vector2f diff = mBall.position - bumper->position();
        const float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        if (dist < 1e-6f)
        {
            continue;
        }
        const sf::Vector2f n = diff / dist;

        // Radial kick once per impact, gated by the bumper's flash cooldown
        // (this also prevents double-scoring). Box2D has already resolved the
        // geometry, so the ball sits on the surface and the kick launches it off.
        if (!bumper->isFlashing())
        {
            const float vn = mBall.velocity.x * n.x + mBall.velocity.y * n.y;
            const sf::Vector2f vnVec = n * vn;
            const sf::Vector2f vt = mBall.velocity - vnVec;
            mBall.velocity = n * bumper->kickSpeed() + vt * 0.85f;

            bumper->hit();
            if (mSound)
            {
                mSound->play("ball_hit_bumper", mBall.velocity.length());
            }
            mScore += bumper->score();

            // Burst of sparks off the point where the ball meets the bumper.
            const sf::Vector2f contact = mBall.position - n * mBall.radius;
            mParticles.emitBurst(contact, 15, sf::Color(205, 255, 255),
                                 sf::Color(80, 205, 255), 220.f, 520.f, 0.5f, 2.f, 4.6f);
        }
    }
}

void World::applyFlipperEffects()
{
    for (auto& f : mFlippers)
    {
        const sf::Vector2f closest = ClosestPointOnSegment(mBall.position, f->bodyA(), f->bodyB());

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
        //
        // "Genuine contact" is dist < radius + half-thickness (+slack): the
        // collision box is centred on the pivot->tip line, so the ball's surface
        // sits a half flipper-thickness beyond it. Engaging at that surface
        // distance (not the bare ball radius, which lies inside the body) is
        // what makes the guard actually fire instead of never matching.
        const sf::Vector2f diff = mBall.position - closest;
        const float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        if (f->isActive() &&
            dist < mBall.radius + kFlipperThickness * 0.5f + kPivotPocketSlack &&
            dist >= 1e-6f)
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
        // Only launch when the ball is actually resting in the launch lane on
        // the pad. Without this, releasing the plunger would fling the ball
        // even when it is nowhere near the pad -- the pad would "push" the ball
        // from a distance. The ball must sit inside the channel and essentially
        // on (or just above) the pad surface to be launched.
        if (mPrevPlungerHeld && mCharge > 0.03f)
        {
            const float gap = mPlungerY - (mBall.position.y + mBall.radius);  // >0: just above the pad
            const bool onPad = mBall.position.x > kChannelLeft
                               && mBall.position.x < kChannelRight
                               && gap < mBall.radius + 2.f
                               && gap > -(kPlungerHalfH + 2.f);
            if (onPad)
            {
                const float speed = kLaunchBase + mCharge * kLaunchExtra;
                mBall.velocity = sf::Vector2f(-150.f, -speed);
                mPlungerCooldown = 0.1f;
                // Kick up sparks from the channel on launch.
                mParticles.emitBurst(mBall.position, 10, sf::Color(255, 165, 85),
                                     sf::Color(255, 95, 45), 80.f, 260.f, 0.45f, 2.f, 4.f);
                // Apply the launch immediately so it is integrated this step.
                b2Body_SetLinearVelocity(mBallBody, toM(mBall.velocity));
            }
        }
        mCharge = 0.0f;
        if (mPlungerY > kPlungerRestY)
        {
            mPlungerY = std::max(kPlungerRestY, mPlungerY - kPlungerUpSpeed * dt);
        }
    }
    mPrevPlungerHeld = mPlungerHeld;
}

// ---------------------------------------------------------------------------
// Coin pickup
// ---------------------------------------------------------------------------
bool World::validSpawn(sf::Vector2f p) const
{
    // The coin must sit inside the playfield and clear of every interactive
    // element so it never touches a wall, flipper or bumper. The distance from
    // `p` to the nearest point of each segment / the centre of each shape must
    // exceed the coin radius plus a clearance pad.
    const float minDist = kCoinRadius + kCoinPad;

    // Walls (this also clears the coin off every internal wall and guide).
    for (const auto& w : mWalls)
    {
        if ((ClosestPointOnSegment(p, w.a, w.b) - p).length() < minDist)
        {
            return false;
        }
    }

    // Flippers: their collision box is centred on the pivot->tip centre-line, so
    // offset by half the flipper thickness to reach the box surface.
    const float flipMin = kFlipperThickness * 0.5f + minDist;
    for (const auto& f : mFlippers)
    {
        if ((ClosestPointOnSegment(p, f->bodyA(), f->bodyB()) - p).length() < flipMin)
        {
            return false;
        }
    }

    // Bumpers.
    for (const auto& b : mBumpers)
    {
        if ((p - b->position()).length() < b->radius() + minDist)
        {
            return false;
        }
    }

    // Keep the coin off the one-way flap plate's sweep near the channel mouth.
    const sf::Vector2f flapDelta = p - sf::Vector2f(kFlapHingeX, kFlapHingeY);
    if (flapDelta.length() < kFlapLength * 0.5f + minDist)
    {
        return false;
    }

    return true;
}

void World::spawnCoin()
{
    // Rejection-sample a point inside the open playfield. The rightmost lane
    // (x >= kChannelLeft) is the plunger launch channel -- the ball only reaches
    // it on a launch, so the coin never spawns there or behind the flap. The
    // rounded arc bounds the top; the wall-distance check clears it.
    const float r = kCoinRadius;
    const float pad = r + kCoinPad;
    const float x0 = kLeft + pad;
    const float x1 = kChannelLeft - pad;
    const float y0 = kTop + pad + 60.f;          // clear of the arc's top corners
    const float y1 = kFloorY - pad - 40.f;       // clear of the drain gap

    sf::Vector2f chosen{kChannelCenterX, 400.f};
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        std::uniform_real_distribution<float> xDist(x0, x1);
        std::uniform_real_distribution<float> yDist(y0, y1);
        const sf::Vector2f p(xDist(mRng), yDist(mRng));
        if (validSpawn(p))
        {
            chosen = p;
            break;
        }
    }

    mCoin = Coin(chosen, r);
    std::uniform_real_distribution<float> lifeDist(kCoinMinLife, kCoinMaxLife);
    mCoin.setLife(lifeDist(mRng));
    mCoin.create(mWorld);
    mCoin.setUserData((void*)&kCoinTag);
}

void World::updateCoin(float dt)
{
    if (mCoin.active())
    {
        mCoin.update(dt);
        if (mCoin.isExpired() || mCoin.isCollected())
        {
            mCoin.destroy();
        }
        return;
    }

    mCoinSpawnTimer -= dt;
    if (mCoinSpawnTimer <= 0.0f)
    {
        spawnCoin();
        std::uniform_real_distribution<float> spawnDist(kCoinMinSpawn, kCoinMaxSpawn);
        mCoinSpawnTimer = spawnDist(mRng);
    }
}

void World::checkDrain()
{
    if (mBall.position.y <= kFloorY)
    {
        return;
    }

    if (mSound)
    {
        mSound->play("ball_drain");
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
    // Seat a fresh ball directly on the plunger pad in the launch channel. The
    // one-way valve closes the channel mouth, so a ball can no longer drop into
    // the channel from above: new balls are placed on the pad instead.
    mBall.position = sf::Vector2f(kBallSpawnX, kBallSpawnY);
    mBall.velocity = sf::Vector2f(0.f, 0.f);

    // Move the Box2D body to the spawn point and stop it.
    b2Body_SetTransform(mBallBody, toM(mBall.position), b2MakeRot(0.f));
    b2Body_SetLinearVelocity(mBallBody, b2Vec2_zero);
    b2Body_SetAwake(mBallBody, true);
}

} // namespace pinballgame
