# Post-mortem 0001 — Resting flipper imparts spurious speed to the ball

- **Date:** 2026-09-04
- **Severity:** Medium (gameplay-correctness, not a crash)
- **Component:** `src/pimbalgame/Flipper.{hpp,cpp}`, Box2D kinematic-body driving in `src/pimbalgame/World.cpp`
- **Fixed in:** v2.7

## Symptom

The flipper behaves correctly only the very first time it is used. The reported
behaviour:

- A ball falling straight onto a **resting** flipper should bounce off it like a
  wall and **not** gain speed.
- A ball sliding down a guide wall onto the resting flipper should keep sliding
  along the flipper surface and **not** pick up extra vertical velocity.
- Only an actively **swinging** flipper should launch the ball.

The bug: once the flipper has been moved and hit the ball at least once, it
keeps "kicking" the ball even after the release is let go and the flipper sits
motionless for the rest of the game. The ball gains speed on every contact with
the idle flipper body, and a ball sliding along it is thrown upward instead of
continuing to slide.

The tell-tale sign was that the fault was **state-dependent**: nothing was wrong
with the resting geometry or restitution — the flipper had to be *moved once*
first before the resting flipper started misbehaving.

## Root cause

The flippers are Box2D **kinematic** bodies driven every frame by
`b2Body_SetTargetTransform(bodyId, target, dt)`, where `target` is the flipper's
current angle/pivot (see `World::update`). The solver then transfers the
flipper's swing momentum to the ball through contact resolution — this replaced
an older manual surface-velocity impulse.

`b2Body_SetTargetTransform` (Box2D `src/body.c`) works by computing the linear
and angular velocity required to travel from the body's **current** transform to
the requested target in one timestep, and writing that into the body state:

```c
float maxVelocity = b2Length( linearVelocity ) + b2AbsFloat( angularVelocity ) * sim->maxExtent;
if ( maxVelocity < body->sleepThreshold ) {
    return;   // <-- leaves the body velocity UNCHANGED
}
...
state->linearVelocity = linearVelocity;
state->angularVelocity = angularVelocity;
```

While the flipper is **swinging**, the requested angle differs from the body's
present angle, so `maxVelocity` is above the sleep threshold and the body is
driven at the swing speed (`kSwingSpeed = 12 rad/s`). Momentum reaches the ball.

The moment the flipper **settles** at its target angle (`mAngle == mTargetAngle`,
so `Flipper::mAngularVelocity == 0`), the requested velocity becomes zero and
`maxVelocity` drops below the sleep threshold. The function then **returns early**,
writing nothing. Crucially, it does *not* zero the body's existing velocity.

For a kinematic body the leftover velocity can never decay away: in
`b2IntegrateVelocitiesTask` a kinematic body has `invMass == 0` and zero gravity
scale, so no force is applied, and with the default damping of `0` the velocity
term is just carried forward (`state->linearVelocity` / `state->angularVelocity`
are used verbatim and integrated into the next position). The body therefore
retains a residual spin from the last moving frame forever.

That lingering angular velocity is exactly what smacks the ball. A ball resting,
falling on, or sliding into the idle flipper now contacts a surface that is
still (in the physics world) rotating, so the solver gives it a velocity kick —
the "spring"/vertical-speed gain described in the report. The very first flip
worked because the residual had not accumulated yet; afterwards it persisted for
the whole game.

## Fix

Reset the flipper Box2D body's velocity to zero whenever it has settled at its
target angle, so a stationary flipper is a genuine static wall. The reset lives
in `Flipper::update()` in `src/pimbalgame/Flipper.cpp`, gated on the same
condition that already indicates a settled flipper — `mAngularVelocity == 0.0f`
(is only `0.0f` when `prev == mAngle == mTargetAngle`) — and guarded against an
unconstructed body (`bodyId.index1 != 0`, following the id-null convention that
ids are zero when null):

```cpp
mAngularVelocity = dt > 0.0f ? (mAngle - prev) / dt : 0.0f;
recompute();

if (bodyId.index1 != 0 && mAngularVelocity == 0.0f)
{
    b2Body_SetLinearVelocity(bodyId, b2Vec2_zero);
    b2Body_SetAngularVelocity(bodyId, 0.0f);
}
```

The body velocity is only touched **after** the swing finishes. While flipping
`mAngularVelocity != 0`, so the momentum of the active stroke is never lost; the
subsequent `b2Body_SetTargetTransform` call in `World::update` then recomputes a
~zero velocity and bails out, leaving the body at rest. A settled flipper (in
either the rest or the active held position) therefore acts as a static surface:
a falling ball bounces off it with the normal flipper restitution, a sliding
ball keeps sliding, and only a moving flipper launches the ball.

## Verification

- The change is confined to `Flipper::update()` and rebuilds cleanly.
- Correctness checklist against the three expected behaviours:
  - **Falls onto a resting flipper** → bounces normally (restitution), no speed
    gain: the body is now stationary, so it is a plain wall collision.
  - **Slides from the wall onto the resting flipper** → keeps sliding: no
    residual spin means no spurious normal/vertical impulse.
  - **Swinging flipper hits the ball** → still gains speed: the reset is skipped
    while `mAngularVelocity != 0`, so swing momentum reaches the ball unchanged.
- A quick playtest with A/Z (left flipper) and D/Right (right flipper): flip
  once, release, and let the ball come to rest on the flippers — they hold the
  ball still with no further kicks, and the ball can slide along the idle
  flipper surfaces.
