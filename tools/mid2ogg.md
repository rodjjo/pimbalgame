# mid2ogg

A developer tool that turns a **Standard MIDI File** (`.mid`) into an
**Ogg Vorbis** sound file (`.ogg`).

A `.mid` file is not audio — it only contains notes (pitch, velocity, timing and
program changes). To hear it we have to *play it back* against an instrument
bank (a SoundFont) and record what comes out. `mid2ogg` does exactly that:

1. Loads a SoundFont (`.sf2`) with [TinySoundFont](dependencies/tiny-sound-font)
   (`tsf.h`) — the same library the game uses.
2. Loads the MIDI with TinyMIDI (`tml.h`) and replays its messages on a virtual
   clock, asking TinySoundFont to synthesise the audio. This is the same
   playback path the game uses in [`src/pimbalgame/Music.cpp`](src/pimbalgame/Music.cpp).
3. Captures the interleaved 16-bit / 44.1 kHz PCM and hands it to **SFML**,
   which encodes and writes it directly to an Ogg Vorbis (`.ogg`) file.

It links `SFML::Audio` (for the OGG writer) and TinySoundFont (headers only).

## Usage

```
mid2ogg --save-path <out.ogg> --mid-path <in.mid> [--soundfont-path <in.sf2>]
```

| Option             | Meaning                                                            |
| ------------------ | ------------------------------------------------------------------ |
| `--save-path <p>`  | Output `.ogg` file path **(required)**.                            |
| `--mid-path <p>`   | Input `.mid` file path **(required)**.                             |
| `--soundfont-path <p>` | SoundFont (`.sf2`) to render against.                          |
| `-?`, `--help`     | Show the built-in help.                                            |

The SoundFont defaults to the project's bundled FluidR3_GM instrument bank;
pass `--soundfont-path` to render against a different `.sf2`. With the default
the two required arguments are enough.

```bash
# Render a MIDI into a playable sound file:
mid2ogg --save-path my-sound.ogg --mid-path my-sound.mid

# Render against a specific SoundFont:
mid2ogg --save-path sfx.ogg --mid-path 02_independent.mid \
        --soundfont-path path/to/your-soundfont.sf2
```

Pass absolute paths, or run from wherever you like. Relative input paths are
resolved from the project root automatically, so the built binary works from
`build/bin` as well.

## Examples

Render one of the generated MIDI files (see [`tools/text2mid`](tools/text2mid.md)
for how they are created) into a playable sound:

```bash
mid2ogg --save-path independent.ogg --mid-path 02_independent.mid
file independent.ogg      # -> "Ogg Vorbis audio ..."
```

## Notes

- Trailing and leading silence are removed automatically. A MIDI render ends
  with the instruments' decaying/reverb tail that fades into dead air, so the
  tool analyses the captured PCM from both ends (RMS energy over short windows)
  and trims everything below an audible threshold. The result starts and ends
  on real audio, so it can be looped with no silence between repeats.
- SFML chooses the writer from the file extension, so the `--save-path` must end
  in `.ogg` for Ogg encoding. (A `.wav` path would use the WAV writer instead.)
- The default SoundFont (FluidR3_GM) is large (~140 MB); loading it takes a
  moment on the first run.
