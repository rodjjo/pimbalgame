# text2mid

A small, self-contained C++17 tool for synthesising a **Standard MIDI File**
(`.mid`) from a melody written in the project's own **"mel"** note language.

It is used to author the game's background music without any third-party audio
libraries: the tool only parses a melody and writes bytes, and the game renders
the resulting MIDI against a sound font at load time (see
`src/pimbalgame/Music.cpp`). The tool links against the C++ standard library
alone and is built whenever `-DBUILD_TOOLS=ON` (the default). Its source lives
in [`tools/text2mid/main.cpp`](tools/text2mid/main.cpp).

## Usage

```
text2mid --save-path <out.mid> [--instruction "<melody>"] [--tempo <bpm>] [--duration <seconds>]
```

| Option            | Meaning                                                            |
| ----------------- | ------------------------------------------------------------------ |
| `--save-path <p>` | Output MIDI file path (required).                                  |
| `--instruction <s>` | The melody in the `mel` language (see below).                   |
| `--tempo <bpm>`   | Beats per minute (default 120).                                    |
| `--duration <s>`  | Target file length in seconds (default 20).                        |
| `-?`, `--help`    | Show the built-in help.                                            |

The melody is repeated to fill the requested duration, so a short phrase
produces a comfortable, loopable background track.

```bash
# From a project script: generate a 20 s theme next to the sound assets.
text2mid --save-path assets/sounds/<name>.mid \
         --instruction "tempo 120 C4 E4 G4 C5"
```

Run `text2mid --help` for the full option list.

## The `mel` note language

The instruction is a whitespace- / comma- / pipe-separated stream of tokens.

| Token    | Syntax                                                              | Example            |
| -------- | ------------------------------------------------------------------ | ------------------ |
| Note     | `[A-G]` + optional `#`/`b` run + optional single-digit octave (0-9) | `C`, `C#`, `Fb`, `B4`, `e5` |
| Rest     | `r` or `-` (silence; only its duration matters)                    | `r/4`              |
| Chord    | two or more notes joined by `+` (sound together, share duration)   | `C+E+G`, `C4+E4+A4` |
| Duration | `/<denominator>` — beats = `4 / denominator`                       | `C4/2` (half), `D/8` (eighth) |
| Tempo    | `tempo <bpm>` or `bpm <bpm>` (default 120)                         | `tempo 110`        |
| Octave   | `o <0-9>` or `octave <0-9>` — default octave for notes that omit one (default 4) | `o 5` |
| Bar      | `|` (cosmetic separator, ignored)                                  | `C E | G B`        |
| End      | `end` or `;` — stops parsing; trailing tokens are dropped          | `... G4 end`       |

### Note naming

A note's MIDI key is

```
12 * (octave + 1) + letterOffset + sharps - flats
```

where the letter offsets are `C=0, D=2, E=4, F=5, G=7, A=9, B=11`, so that
`C4 == 60` and `A4 == 69` (concert pitch). The octave defaults to 4 (`o 4`)
unless changed by an `o` / `octave` token, so `C`, `C#`, and `C4` all refer to
the same key while `c5` is one octave higher.

### Duration / beats

A `/` suffix sets the note length in *beats* (`beats = 4 / denominator`):

| Token   | Name         | Beats |
| ------- | ------------ | ----- |
| `/1`    | whole        | 4     |
| `/2`    | half         | 2     |
| `/4`    | quarter      | 1     |
| `/8`    | eighth       | 1/2   |
| `/16`   | sixteenth    | 1/4   |
| `/32`   | 32nd         | 1/8   |

The default (no suffix) is a quarter note.

### Example

```
tempo 110
C4/4 E4/4 G4/4 B4/4 | C5/2 B4/2 A4/4
G4/8 B4/8 D5/8 G5/8 F5/8 D5/8 B4/8 G4/8
E4 r/4 end
```

This writes a format-0, single-track Standard MIDI File on channel 0 using a
bright lead program — the same path the game renders against its sound font.

## How the game uses it

The generated MIDI is copied next to the executable at build time (see
`src/CMakeLists.txt`) and played continuously. `Music.cpp` loads the SoundFont
(`assets/sounds/sound_file.sf2`) and the MIDI, replays the MIDI against the
SoundFont with TinySoundFont, caches the rendered 16-bit PCM, and hands it to
SFML to loop. The menu's **Music** / **General** sliders drive the playback
volume. See the project [README](../README.md#release-notes) release notes.
