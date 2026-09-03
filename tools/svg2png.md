# svg2png

A developer tool for turning the project's vector art into ready-to-use
textures, and for packing those textures into a single embeddable C++ header.

It is written in C++17 and relies only on two git submodules under
`dependencies/` (added the same way SFML is):

- [CImg](https://github.com/GreycLab/CImg) — header-only image buffer: the
  anti-aliased, supersampled render target and the texture-atlas packer.
- [lodepng](https://github.com/lvandeve/lodepng) — PNG decoding and encoding.

The tool is built only when the project is configured with
`-DBUILD_TOOLS=ON` (the default), wired through the top-level `CMakeLists.txt`.
Its source lives in [`tools/svg2png/main.cpp`](tools/svg2png/main.cpp).

## Modes

Two modes are selected as the first argument:

```
svg2png png    --svg-path <f.svg>   --save-path <out.png>
svg2png png    --svg-string "<svg>...</svg>" --save-path <out.png>
svg2png texture --output-path <out.cxxpng> --png-path <t1.png> [--png-path ...]
```

Run `svg2png help` (or `svg2png --help`) for the full option list.

## `png` mode — SVG → PNG

An SVG is a *vector* (XML) format, so neither CImg nor lodepng can rasterize it
directly. This mode therefore parses the SVG markup and its presentation
attributes itself, rasterizes the shapes into an anti-aliased, supersampled
buffer with CImg, and encodes the result with lodepng.

Input comes from `--svg-path` (a file) or `--svg-string` (inline markup); the
output path is set with `--save-path`.

```bash
# From a file, on a transparent background:
svg2png png --svg-path assets/flipper.svg --save-path out/flipper.png --transparent

# From an inline string, on a white background:
svg2png png --svg-string '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48"><circle cx="24" cy="24" r="22" fill="#cfd6ea"/></svg>' \
            --save-path out/ball.png
```

### Options

| Option               | Meaning                                                                        |
| -------------------- | ------------------------------------------------------------------------------ |
| `--svg-path <p>`     | Path to the input `.svg` file.                                                 |
| `--svg-string <s>`   | Raw SVG markup as a string (mutually exclusive with `--svg-path`).             |
| `--save-path <p>`    | Output `.png` path (required).                                                 |
| `-w`, `--width <n>`  | Force output width in px (default: from the SVG).                              |
| `-h`, `--height <n>` | Force output height in px (default: from the SVG).                             |
| `-S`, `--supersample`| Force a supersampling factor (default: auto 1..8, scaled to the output size).  |
| `--background <c>`   | Background color, e.g. `"#ffffff"` (default: white).                           |
| `--transparent`      | Make the background transparent instead of white.                              |

### Supported SVG

`svg`, `g`, `defs`, `symbol`, `use`, `rect`, `circle`, `ellipse`, `line`,
`polyline`, `polygon`, `path` — with `fill` / `stroke` (named colors, `#rgb`,
`#rrggbb`, `#rrgbbaa`, `rgb()`), `fill-opacity`, `stroke-width`,
`stroke-linecap`, `opacity`, and `transform` (`matrix`, `translate`, `rotate`,
`scale`, `skewX`, `skewY`).

Text, embedded `<image>`, clip paths, gradients, masks and filters are
intentionally out of scope.

## `texture` mode — PNGs → embeddable C++ atlas

Packs several already-encoded `.png` textures into a single in-memory atlas and
writes a C++ header (`.cxxpng`) that embeds the atlas PNG bytes together with a
`std::map<std::string, coordinate_t>` giving each texture's rectangle inside the
atlas. The header can be `#include`d into C++ code to embed the textures
directly in an application — no image files need ship.

```bash
# Pack three PNGs into an embeddable header:
svg2png texture --output-path out/textures.cxxpng \
    --png-path out/flipper.png --png-path out/bump.png --png-path out/ball.png \
    --shelf-width 0 --padding 3
```

### Options

| Option             | Meaning                                                                       |
| ------------------ | ----------------------------------------------------------------------------- |
| `--output-path <p>`| Output `.cxxpng` C++ header (required).                                        |
| `--png-path <p>`   | A source PNG texture to pack (repeatable, required).                          |
| `--atlas-path <p>` | Also write the raw packed atlas PNG here (optional, for inspection).          |
| `--shelf-width <n>`| Max atlas width per shelf in px (default: 2048, `0` = single row).            |
| `--padding <n>`    | Gap (px) between packed textures (default: 1).                                |

The header defines a `texture_atlas` namespace exposing:

- `texture_atlas::atlas_png` — `std::array<std::uint8_t, N>` with the raw PNG
  bytes of the packed atlas.
- `texture_atlas::textures()` — `const std::map<std::string, coordinate_t>&`
  mapping each texture name to its rectangle (`x`, `y`, `width`, `height`).

Pull a texture straight out of the atlas and use it in-game without shipping any
image files, e.g.:

```cpp
#include "textures.cxxpng"

unsigned w, h;
std::vector<unsigned char> px;
lodepng::decode(px, w, h, texture_atlas::atlas_png.data(),
                texture_atlas::atlas_png.size(), LCT_RGBA, 8);

const auto& rect = texture_atlas::textures().at("ball");
/* blit px at (rect.x, rect.y, rect.width, rect.height) */
```

## How the game uses it

At build time the `src/` `CMakeLists.txt` rasterizes every `assets/*.svg` to a
PNG (with `png --transparent`) and packs them all into a single embeddable header
(`textures.cxxpng`) with `texture`, then `#include`s that header into the game.
`src/pimbalgame/Textures.cpp` decodes the in-memory atlas bytes and hands out
`sf::Sprite`s by name, so the game ships no image files at all — the only art
that is version-controlled is the SVG source. See the project
[README](../README.md#release-notes) release notes.
