#pragma once

#include <SFML/Graphics.hpp>
#include <memory>
#include <optional>
#include <vector>

#include "pimbalgame/Ball.hpp"
#include "pimbalgame/Flipper.hpp"
#include "pimbalgame/Bumper.hpp"
#include "pimbalgame/Particles.hpp"
#include "pimbalgame/Textures.hpp"

namespace pimbalgame
{
// A single straight wall segment of the playfield.
struct Wall
{
    sf::Vector2f a{};
    sf::Vector2f b{};
    float restitution = 0.7f;
};

// The playfield: owns the ball, flippers, bumpers, walls and plunger, and
// integrates the physics for each sub-step. Also renders everything.
class World
{
public:
    World(int windowWidth, int windowHeight);

    // Starts a new game (score, balls and ball position).
    void reset();
    void resetBall();

    void setLeftFlipper(bool active);
    void setRightFlipper(bool active);
    void setPlungerHeld(bool held);

    void update(float dt);
    void render(sf::RenderWindow& window) const;

    int score() const { return mScore; }
    int balls() const { return mBalls; }
    bool gameOver() const { return mGameOver; }
    const Ball& ball() const { return mBall; }

private:
    void buildTable();
    void spawnArc(float cx, float cy, float r, float startDeg, float endDeg, int segments, float restitution = 0.5f);
    void addWall(float ax, float ay, float bx, float by, float restitution = 0.5f);
    void addBumper(sf::Vector2f position, float radius, int score, float kickSpeed);
    void collideWalls();
    void collideBumpers();
    void collideFlippers();
    void collidePlunger();
    void checkDrain();
    void renderBackground(sf::RenderWindow& window) const;

    void updatePlunger(float dt);
    void resolveSegment(const sf::Vector2f& a, const sf::Vector2f& b,
                        float restitution,
                        const std::optional<sf::Vector2f>& flipperVelocity = std::nullopt);

    Ball mBall;
    std::vector<Wall> mWalls;
    std::vector<std::unique_ptr<Flipper>> mFlippers;
    std::vector<std::unique_ptr<Bumper>> mBumpers;

    // Generated art: the embeddable texture atlas (owns the atlas sf::Texture)
    // and the ball's particle effects (glow + sparks).
    Textures mTextures;
    Particles mParticles;
    std::optional<sf::Sprite> mSkull;  // decorative background watermark

    // Input state.
    bool mLeftFlipper = false;
    bool mRightFlipper = false;
    bool mPlungerHeld = false;
    bool mPrevPlungerHeld = false;

    // Plunger (vertical launcher in the right channel).
    float mPlungerY = 0.f;      // current top-surface y of the pad
    float mCharge = 0.f;        // 0..1 charge built while held
    float mPlungerCooldown = 0.f;

    int mScore = 0;
    int mBalls = 3;
    bool mGameOver = false;
};

} // namespace pimbalgame
