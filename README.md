# PimBalGame

A pinball game written in C++17 using the [SFML](https://www.sfml-dev.org/)
library. It ships with a complete, self-contained implementation: a custom
2D physics loop, two flippers, four bumpers, a chargeable plunger launcher,
scoring, three balls per game and a game-over screen.

The game builds SFML in-tree from a git submodule, so no system-wide SFML
installation or `find_package` step is required.

## How this game was made

PimBalGame was designed and implemented end-to-end by an AI coding agent,
[Ornith 1.5 35B-A3B](https://huggingface.co/ornith-ai/Ornith-1.5-35B-A3B) — a
sparse MoE LLM (35B total parameters, ~3B active) — running inside the
[DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness) agent loop.
Model inference was served locally with
[FreeToken](https://github.com/FlashML-org/FreeToken), a high-throughput
transformer inference engine for sparse models.

| Component        | Tool / Model                                                        |
| ---------------- | ------------------------------------------------------------------- |
| Coding agent     | [Ornith 1.5 35B-A3B](https://huggingface.co/ornith-ai/Ornith-1.5-35B-A3B) |
| Agent harness    | [DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness)     |
| Inference engine | [FreeToken](https://github.com/FlashML-org/FreeToken)                   |
| Runtime hardware | NVIDIA RTX 5070 Ti (16 GB VRAM), 96 GB DDR4 system RAM              |
| Game library     | [SFML](https://www.sfml-dev.org/) (Graphics, Window, System)        |

## Features

- **Fixed-timestep physics loop.** The simulation runs at a constant
  `1/120 s` sub-step, decoupled from the render rate (capped at 60 FPS), so the
  behaviour is deterministic and stable regardless of frame timing. Large frames
  (e.g. after losing window focus) are clamped to avoid the "spiral of death".
- **Segment-based collision.** The playfield is made of straight wall segments;
  the ball (a circle) resolves against each segment using closest-point projection
  plus impulse-based reflection with per-wall restitution.
- **Flippers.** Two rotating flippers pivot around fixed points and swing between
  a resting and an active angle. Their angular velocity is transferred to the ball
  on contact, and an anti-stick guard prevents the ball from settling in the
  valley formed by an active flipper and the adjacent wall.
- **Bumpers.** Four circular bumpers apply a fixed radial kick and award points on
  contact, flashing briefly to give visual feedback.
- **Plunger launcher.** Hold to charge a spring in the right channel; release to
  launch the ball with a power proportional to the charge.
- **HUD & game loop.** On-screen score, remaining balls and a "Game Over / restart"
  prompt. Three balls per game.
- **Portable font loading.** A bundled font is resolved relative to the executable
  (or source tree) at runtime, falling back gracefully if it is missing.

## Controls

| Action            | Keys                          |
| ----------------- | ----------------------------- |
| Left flipper      | `A`, `Z` or `←` (Left arrow)  |
| Right flipper     | `D` or `→` (Right arrow)      |
| Plunger (hold)    | `Space`                       |
| Restart (game over) | `R`                         |
| Quit              | `Esc` or the window close button |

## Project layout

```
pimbalgame/
├── CMakeLists.txt          # Top-level build configuration
├── dependencies/
│   └── sfml/               # SFML library (git submodule, built in-tree)
├── src/
│   ├── CMakeLists.txt      # Builds the game executable
│   ├── main.cpp            # Program entry point
│   └── pimbalgame/
│       ├── Game.hpp/.cpp   # Window, main loop, input, HUD rendering
│       ├── World.hpp/.cpp  # Playfield geometry, physics, scoring, plunger
│       ├── Physics.hpp     # Shared geometry helper (closest point on a segment)
│       ├── Ball.hpp/.cpp   # The ball: state, gravity integration, speed clamp
│       ├── Flipper.hpp/.cpp# Rotating flipper with angular momentum transfer
│       └── Bumper.hpp/.cpp # Circular scoring bumper with flash effect
├── assets/
│   └── fonts/
│       └── DejaVuSans.ttf  # Bundled HUD font (copied next to the executable)
├── .gitignore
└── README.md
```

## Prerequisites

- A C++17 capable compiler (GCC 7+, Clang 5+, or MSVC 2017+).
- CMake >= 3.16.
- SFML system dependencies for the platform (only the **Graphics**, **Window**
  and **System** components are built; the Network and Audio components are
  disabled, so Libssh2 / Vorbis / gsm are not required):
  - **Linux:** `build-essential cmake git`, plus the development packages for the
    SFML system dependencies (see the
    [SFML Linux build guide](https://www.sfml-dev.org/tutorials/2.6/start_linux.php)).
  - **Windows:** MSVC 2017+ plus the SFML system dependencies (OpenGL, Win32).
  - **macOS:** Homebrew + `brew install sfml`.

## Getting started

Clone the repository **with submodules** so that SFML is available:

```bash
git clone --recurse-submodules <repo-url> pimbalgame
cd pimbalgame
```

If the submodule is already present, initialize/update it:

```bash
git submodule update --init --recursive
```

### Configure & build (Linux / macOS)

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

The executable will be produced at `build/bin/pimbalgame`.

### Configure & build (Windows)

```bat
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

## Running

```bash
./build/bin/pimbalgame   # Linux / macOS
```

On Windows the executable is at `build\Release\pimbalgame.exe`. The bundled font
is copied next to the executable by the build so it can be located at runtime.

## License

TBD
