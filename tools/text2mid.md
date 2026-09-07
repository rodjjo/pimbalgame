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

### Voices

A melody may contain **up to four independent voices**. Each voice is started
with a `voice` / `v` token followed by a number 1–4, and every note that
follows belongs to it — until the next voice token or `end`. Notes written
before any voice token belong to **voice 1**.

```
tempo 120
v1 guitar  E4  G4  B4  E5
v2 drums   R   R   R   R
v3 piano   C5  A4  G4  E4
v4 bass    E2  G2  B2  E3
end
```

Each voice is **completely independent**: it runs on its **own timeline** (its
own cursor and its own loop length) and plays **in parallel** with the others —
exactly like the separate channels of a real multi-track MIDI file. Voices are
*not* sequential and *not* round-robin: a fast voice and a slow voice loop at
their own rates at the same time.

**Instruments.** A voice's instrument is chosen with `program <name|num>`
(aliases `prog`, `inst`), or as a shorthand right after the voice tag (`v1
piano ...`). Instruments may be given by name or by a raw MIDI program number
(0–127). The default instrument is a synth lead (program 74). Supported names:

```
piano, electricPiano, organ, guitar, guitarNylon, guitarSteel, guitarJazz,
bass, bassPick, synthBass, violin, cello, harp, strings, staccato, choir,
trumpet, sax, flute, lead, pad, matrix, drums
```

**Drums.** `drums` (also `drum`, `percussion`, `perc`, `kit`) is routed to the
MIDI drum channel (channel 10 / zero-based channel 9, the game's drum-kit
channel) and rendered as a drum kit. Melodic voices get successive channels
0, 1, 2, 3.

### Example

A four-voice arrangement:

```
tempo 120
v1 guitar  E4/8 G4/8 B4/8 E5/8  D5/8 B4/8 G4/8 E4/8
v2 drums   R/4 R/4 R/4 R/4      (drum hits on the kit channel)
v3 piano   C5/2 A4/2
v4 bass    E2/4 G2/8 B2/8 E3/4
end
```

This writes a format-0, single-track Standard MIDI File on channels 0–3 (drums
on channel 9), each voice looping independently.

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

## How the game uses it

The generated MIDI is copied next to the executable at build time (see
`src/CMakeLists.txt`) and played continuously. `Music.cpp` loads the SoundFont
(`assets/sounds/sound_file.sf2`) and the MIDI, replays the MIDI against the
SoundFont with TinySoundFont (dispatching program / note-on / note-off per
channel, so the independent voices play together), caches the rendered 16-bit
PCM, and hands it to SFML to loop. The menu's **Music** / **General** sliders
drive the playback volume. See the project [README](../README.md#release-notes)
release notes.
