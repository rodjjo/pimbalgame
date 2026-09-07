// ---------------------------------------------------------------------------
// text2mid - synthesise a Standard MIDI File (.mid) from a melody written in
// the "mel" note language.
//
// The tool is completely self-contained: it needs only the C++ standard
// library and links against nothing external. It parses the instruction
// string into one or more independent note voices, repeats every voice to fill
// the requested file length and writes a format-0 Standard MIDI File that
// SFML's TinySoundFont playback path renders against a sound font.
//
// A melody has up to four *independent* voices. Each voice runs on its OWN
// timeline (its own cursor and loop length) and plays in parallel with the
// others, exactly like the separate channels of a real multi-track MIDI file:
// they are neither sequential nor round-robin.
// ---------------------------------------------------------------------------

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

namespace
{
    // -----------------------------------------------------------------------
    // The "mel" note language.
    //
    // The instruction is a whitespace- / comma- / pipe-separated stream of
    // tokens. The recognised tokens are:
    //
    //   Voice    "voice <1-4>" or "v <1-4>" starts (or switches to) an
    //            independent voice. Notes that follow belong to it until the
    //            next voice token or "end". Defaults to voice 1.
    //   Program  "program <name|num>" (aka "prog"/"inst") sets the instrument
    //            of the *current* voice, either by a recognised name (see the
    //            list below) or by a raw MIDI program number (0-127). An
    //            instrument name may also be written right after the voice tag
    //            as a shorthand: "v1 piano C4 E4". The default instrument is a
    //            synth lead (program 74). "drums" is placed on the MIDI drums
    //            channel (ch. 10) and rendered as a drum kit.
    //   Note     one of the letters A-G, an optional run of '#' (sharps) or
    //            'b' (flats), an optional single-digit octave (0-9, default 4).
    //            Examples: C, C#, Fb, B4, e5.
    //   Rest     'r' or '-' (a silence; only its duration matters).
    //   Chord    two or more notes joined by '+'; they sound together and share
    //            the duration. Examples: C+E+G, C4+E4+A4.
    //   Duration an optional '/denominator' attached to a note/chord/rest,
    //            where beats = 4 / denominator. 1 = whole (4), 2 = half (2),
    //            4 = quarter (1, default), 8 = eighth (1/2), 16 = sixteenth
    //            (1/4), 32 = 32nd (1/8). Example: C4/2 a half note, D/8 an
    //            eighth note.
    //   Tempo    "tempo <bpm>" or "bpm <bpm>" (default 120).
    //   Octave   "o <0-9>" or "octave <0-9>" sets the default octave for notes
    //            that omit one (default 4).
    //   Bar      '|' is a cosmetic separator and is ignored.
    //   End      "end" or ";" stops parsing (any following tokens are dropped).
    //
    // A note's MIDI key is: 12 * (octave + 1) + letterOffset + sharps - flats,
    // where the letter offsets are C=0, D=2, E=4, F=5, G=7, A=9, B=11, so that
    // C4 == 60 and A4 == 69 (concert pitch).
    // -----------------------------------------------------------------------

    constexpr int kDefaultBpm = 120;
    constexpr int kDefaultOctave = 4;
    constexpr uint32_t kTicksPerBeat = 480;  // MIDI pulses per quarter note
    constexpr double kDefaultDurationMs = 20.0;
    constexpr int kMaxVoices = 4;
    constexpr int kDrumChannel = 9;  // MIDI ch. 10 (zero-based 9) is the drum kit
    constexpr uint8_t kDefaultProgram = 74;

    // A scheduled note: absolute position (ticks), the key, velocity and the
    // note length (ticks). A chord is emitted as several of these sharing a
    // start tick and length.
    struct NoteEvent
    {
        uint32_t tick = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint32_t length = 0;
    };

    // An independent musical voice: its own note stream, its own phrase length
    // (cursor) and its own instrument. The channel is assigned when the file is
    // written.
    struct Voice
    {
        std::vector<NoteEvent> notes;
        uint32_t cursor = 0;   // phrase length in ticks (sum of note lengths)
        uint32_t tokens = 0;   // note tokens seen (drives velocity breathing)
        int program = static_cast<int>(kDefaultProgram);
        bool isDrums = false;
        int channel = -1;      // assigned at emit time
    };

    // Note-letter semitone offsets within an octave (C=0 ... B=11).
    int letterOffset(char letter)
    {
        switch (letter)
        {
            case 'C': return 0;
            case 'D': return 2;
            case 'E': return 4;
            case 'F': return 5;
            case 'G': return 7;
            case 'A': return 9;
            case 'B': return 11;
            default: return 0;
        }
    }

    // Split a string on whitespace/comma/pipe separators, discarding empties.
    std::vector<std::string> tokenize(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        auto flush = [&]()
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        };
        for (char ch : s)
        {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                ch == ',' || ch == '|' || ch == ';')
            {
                flush();
            }
            else
            {
                cur.push_back(ch);
            }
        }
        flush();
        return out;
    }

    // Parse a base-10 integer from a token; returns false on failure.
    bool parseInt(const std::string& tok, int& out)
    {
        if (tok.empty())
        {
            return false;
        }
        try
        {
            size_t idx = 0;
            const long v = std::stol(tok, &idx, 10);
            if (idx != tok.size())  // reject trailing junk
            {
                return false;
            }
            out = static_cast<int>(v);
            return true;
        }
        catch (const std::exception&)
        {
            // Out-of-range or non-numeric input.
            return false;
        }
    }

    // Parse a note / chord / rest token. Returns false (and skips the token) if
    // the token is not a valid note. On success `keys` receives every MIDI key
    // (at least one) and `durationTicks` the note length.
    bool parseToken(const std::string& tok, int octave,
                    std::vector<uint8_t>& keys, uint32_t& durationTicks)
    {
        // A trailing '/denominator' sets the duration (beats = 4 / denominator).
        double beats = 1.0;  // quarter note by default
        std::string pitch = tok;
        const size_t slash = tok.find('/');
        if (slash != std::string::npos)
        {
            const std::string denomStr = tok.substr(slash + 1);
            pitch = tok.substr(0, slash);
            int denom = 0;
            if (!parseInt(denomStr, denom) || denom <= 0)
            {
                return false;
            }
            beats = 4.0 / denom;
        }
        durationTicks = static_cast<uint32_t>(std::lround(beats * double(kTicksPerBeat)));
        if (durationTicks == 0)
        {
            durationTicks = 1;
        }

        // A chord is several pitches joined by '+'.
        std::vector<std::string> parts;
        std::string acc;
        for (char ch : pitch)
        {
            if (ch == '+')
            {
                parts.push_back(acc);
                acc.clear();
            }
            else
            {
                acc.push_back(ch);
            }
        }
        parts.push_back(acc);

        keys.clear();
        bool sawRest = false;
        for (const std::string& part : parts)
        {
            // A rest ('r' or '-') produces no key but is still a valid token.
            if (part == "r" || part == "-")
            {
                sawRest = true;
                continue;
            }
            if (part.empty() || part.size() > 4)
            {
                return false;
            }

            const char letter = part[0];
            if (letter < 'A' || letter > 'G')
            {
                return false;
            }
            const int offset = letterOffset(letter);

            int sharps = 0;
            int flats = 0;
            size_t i = 1;
            for (; i < part.size() && part[i] == '#'; ++i)
            {
                ++sharps;
            }
            for (; i < part.size() && part[i] == 'b'; ++i)
            {
                ++flats;
            }

            int oct = octave;
            if (i < part.size() && part[i] >= '0' && part[i] <= '9')
            {
                oct = part[i] - '0';
                ++i;
            }
            if (i != part.size())  // trailing junk after the optional octave
            {
                return false;
            }

            int key = 12 * (oct + 1) + offset + sharps - flats;
            if (key < 0 || key > 127)
            {
                return false;
            }
            keys.push_back(static_cast<uint8_t>(key));
        }

        // A valid token is either at least one pitch or a (rest-only) silence.
        return !keys.empty() || sawRest;
    }

    // Return true if the token could be parsed as a note/chord/rest.
    bool isNoteLike(const std::string& tok)
    {
        std::vector<uint8_t> keys;
        uint32_t length = 0;
        return parseToken(tok, kDefaultOctave, keys, length);
    }

    // A named MIDI instrument. `drums` marks the kit (played on channel 9).
    struct Program
    {
        const char* name;
        int program;
        bool drums;
    };

    // A small, sensible subset of General-Music programs plus "drums". A raw
    // number (0-127) is also accepted by resolveProgram.
    const Program kPrograms[] = {
        {"piano", 0, false},         {"acousticpiano", 0, false},
        {"electricpiano", 4, false}, {"electricgrandpiano", 4, false}, {"rh", 4, false},
        {"organ", 8, false},         {"churchorgan", 8, false},
        {"guitar", 24, false},       {"guitaracoustic", 24, false}, {"acousticguitar", 24, false},
        {"guitarnylon", 25, false},  {"nylonguitar", 25, false},
        {"guitarjazz", 26, false},   {"jazzguitar", 26, false},
        {"guitarsteel", 29, false},  {"electrichuitar", 29, false}, {"electriguitar", 29, false},
        {"bass", 33, false},         {"bassfinger", 33, false}, {"bassacoustic", 33, false},
        {"basspick", 34, false},     {"synthbass", 38, false},
        {"violin", 40, false},
        {"cello", 43, false},
        {"harp", 46, false},
        {"strings", 48, false},
        {"staccato", 49, false},
        {"choir", 52, false},        {"voiceooh", 52, false},
        {"trumpet", 56, false},
        {"sax", 65, false},          {"sopranosax", 65, false},
        {"flute", 73, false},        {"tinflute", 73, false},
        {"lead", 80, false},         {"synthlead", 80, false},
        {"pad", 88, false},
        {"matrix", 89, false},
        // Drum kit: played on the MIDI drums channel (9).
        {"drums", 0, true},          {"drum", 0, true},
        {"percussion", 0, true},     {"perc", 0, true}, {"kit", 0, true},
    };

    std::string toLower(const std::string& s)
    {
        std::string r = s;
        for (char& c : r)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return r;
    }

    // Resolve an instrument name (or a raw program number) to a MIDI program.
    // `drumsOut` is set when a drum-kit instrument was requested. Returns -1 if
    // nothing matched.
    int resolveProgram(const std::string& rawName, bool& drumsOut)
    {
        drumsOut = false;
        const std::string n = toLower(rawName);
        if (n.empty())
        {
            return -1;
        }
        for (const Program& p : kPrograms)
        {
            if (std::string(p.name) == n)
            {
                drumsOut = p.drums;
                return p.program;
            }
        }
        int v = 0;
        if (parseInt(n, v) && v >= 0 && v <= 127)  // raw MIDI program number
        {
            return v;
        }
        return -1;
    }

    void setVoiceProgram(Voice& v, int program, bool drums)
    {
        v.program = program;
        v.isDrums = drums;
    }

    void printUsage()
    {
        std::cout <<
            "text2mid - synthesise a Standard MIDI File from a melody written in the\n"
            "'mel' note language (up to 4 independent voices).\n"
            "\n"
            "Usage:\n"
            "  text2mid --save-path <out.mid> [--instruction \"<melody>\"]\n"
            "           [--tempo <bpm>] [--duration <seconds>]\n"
            "\n"
            "Options:\n"
            "  --save-path <p>         Output MIDI file path (required).\n"
            "  --instruction <s>       The melody (see the language below).\n"
            "                          Defaults to a ready-made PimBalGame theme.\n"
            "  --tempo <bpm>           Beats per minute (default 120).\n"
            "  --duration <seconds>    Target file length in seconds (default 20).\n"
            "  -?, --help              Show this help.\n"
            "\n"
            "The 'mel' language (tokens separated by spaces / commas):\n"
            "  Voice    voice <1-4> | v <1-4>    start/switch an independent voice\n"
            "  Program  program <name|num> | prog | inst   set current voice's\n"
            "                               instrument (name or raw MIDI number).\n"
            "                               'drums' uses the drum kit (ch. 10).\n"
            "  Note     [A-G] [#..|b..] [0-9]   C, C#, Fb, B4, e5      (C4 == MIDI 60)\n"
            "  Rest     r  or  -\n"
            "  Chord    C+E+G                    sounds together\n"
            "  Duration /denominator           /1 whole, /2 half, /4 quarter (default),\n"
            "                                  /8 eighth, /16 sixteenth\n"
            "  Tempo    tempo <bpm> | bpm <bpm>\n"
            "  Octave   o <0-9>   | octave <0-9>\n"
            "  Bar     |   (ignored)   End   end | ;\n"
            "\n"
            "Each voice runs on its OWN timeline and plays in parallel with the others\n"
            "(independent loops, not sequential / not round-robin). Named instruments:\n"
            "  piano, electricPiano, organ, guitar, guitarNylon, guitarSteel, guitarJazz,\n"
            "  bass, bassPick, synthBass, violin, cello, harp, strings, staccato, choir,\n"
            "  trumpet, sax, flute, lead, pad, matrix, drums\n";
    }

    // The default theme, used when --instruction is not supplied. A short,
    // looping, major-key phrase that sits comfortably under a sound font. Kept
    // as a single-voice instruction for backward compatibility.
    const char* kDefaultInstruction =
        "tempo 104 "
        "E5/8 G5/8 A5/8 C6/8 B5/8 G5/8 E5/8 D5/8 "
        "C5/8 D5/8 E5/8 F5/8 G5/4 r/4 | "
        "G4/4 B4/4 D5/4 F4/4 "
        "A4/8 C5/8 E5/8 A5/8 G5/8 E5/8 C5/8 A4/8 "
        "G4/8 B4/8 D5/8 G5/8 F5/8 D5/8 B4/8 G4/8 "
        "C5/2 G4/4 r/4";

    // -----------------------------------------------------------------------
    // Standard MIDI File (format 0, single track, up to 4 channels) writer.
    // -----------------------------------------------------------------------

    // Encode a delta time as a MIDI variable-length quantity (7-bit groups,
    // most-significant group first, every group but the last carries 0x80).
    void encodeVLQ(uint32_t value, std::vector<uint8_t>& out)
    {
        std::vector<uint8_t> groups;
        do
        {
            groups.push_back(static_cast<uint8_t>(value & 0x7F));
            value >>= 7;
        } while (value != 0);
        for (int i = static_cast<int>(groups.size()) - 1; i >= 0; --i)
        {
            uint8_t b = groups[static_cast<size_t>(i)];
            if (i != 0)
            {
                b |= 0x80;  // continuation bit on every group but the last
            }
            out.push_back(b);
        }
    }

    struct RawMessage
    {
        uint32_t tick = 0;
        std::vector<uint8_t> bytes;  // status/data, no delta prefix
    };

    // Build the full MIDI byte stream: header + one track. Every voice repeats
    // its own phrase to fill `targetTicks`, then a final note-off burst and an
    // end-of-track message close the file off at the target length. Each voice
    // gets its own MIDI channel (0..3, or 9 for drums) and program, so the
    // voices are genuinely independent and play in parallel.
    std::vector<uint8_t> buildMidi(std::vector<Voice>& voices,
                                   uint32_t targetTicks, uint32_t bpm)
    {
        // Assign a channel per voice: drums on the kit channel, the rest on
        // successive melodic channels 0, 1, 2, ... (all distinct).
        int melodicRank = 0;
        for (Voice& v : voices)
        {
            if (v.isDrums)
            {
                v.channel = kDrumChannel;
            }
            else
            {
                v.channel = melodicRank++;
            }
        }

        const uint32_t cap = targetTicks > 0 ? targetTicks : kTicksPerBeat;

        std::vector<RawMessage> raw;

        // meta: text + track name (delta zero).
        auto meta = [&](uint8_t type, const std::string& text)
        {
            RawMessage m;
            m.tick = 0;
            m.bytes.push_back(0xFF);
            m.bytes.push_back(type);
            encodeVLQ(static_cast<uint32_t>(text.size()), m.bytes);
            m.bytes.insert(m.bytes.end(), text.begin(), text.end());
            raw.push_back(std::move(m));
        };
        meta(0x03, "text2mid");
        meta(0x0F, voices.size() == 1 ? "PimBalGame theme"
                                      : "PimBalGame multi-voice theme");

        // meta: set tempo. Microseconds per quarter note = 60,000,000 / bpm.
        const uint32_t microPerBeat =
            bpm <= 0 ? 500000u : std::max(1u, 60000000u / uint32_t(bpm));
        {
            RawMessage m;
            m.tick = 0;
            m.bytes.push_back(0xFF);
            m.bytes.push_back(0x51);  // tempo
            m.bytes.push_back(0x03);  // 3-byte length
            m.bytes.push_back(static_cast<uint8_t>((microPerBeat >> 16) & 0xFF));
            m.bytes.push_back(static_cast<uint8_t>((microPerBeat >> 8) & 0xFF));
            m.bytes.push_back(static_cast<uint8_t>(microPerBeat & 0xFF));
            raw.push_back(std::move(m));
        }

        // program change per voice, on its channel.
        for (const Voice& v : voices)
        {
            RawMessage m;
            m.tick = 0;
            m.bytes.push_back(0xC0 | (static_cast<uint8_t>(v.channel) & 0x0F));
            m.bytes.push_back(static_cast<uint8_t>(v.program & 0x7F));
            raw.push_back(std::move(m));
        }

        // Notes. Each voice repeats its own phrase on its own timeline, so
        // voices overlap freely (independent loops, not sequential).
        for (Voice& v : voices)
        {
            if (v.notes.empty())
            {
                continue;
            }
            const uint32_t baseLen = v.cursor > 0 ? v.cursor : kTicksPerBeat;
            const uint8_t chByte = static_cast<uint8_t>(v.channel) & 0x0F;
            for (uint32_t global = 0; global < cap; global += baseLen)
            {
                bool reached = false;
                bool any = false;
                for (const NoteEvent& n : v.notes)
                {
                    const uint32_t start = global + n.tick;
                    if (start >= cap)
                    {
                        reached = true;  // this voice has been filled past the cut
                        break;
                    }
                    const uint32_t end = std::min<uint32_t>(cap, start + n.length);
                    RawMessage on;
                    on.tick = start;
                    on.bytes = {static_cast<uint8_t>(0x90 | chByte), n.key, n.velocity};  // note-on
                    raw.push_back(on);
                    RawMessage off;
                    off.tick = end;
                    off.bytes = {static_cast<uint8_t>(0x80 | chByte), n.key, 0x40};       // note-off
                    raw.push_back(off);
                    any = true;
                }
                if (reached || !any)
                {
                    break;
                }
            }
        }

        // End-of-track at (at least) the target length.
        {
            RawMessage eot;
            eot.tick = cap;
            eot.bytes = {0xFF, 0x2F, 0x00};
            raw.push_back(eot);
        }

        // Stable-sort by absolute tick so messages at the same tick keep their
        // insertion order (meta/programs first, notes in sequence, eot last).
        std::stable_sort(raw.begin(), raw.end(),
                         [](const RawMessage& a, const RawMessage& b)
                         { return a.tick < b.tick; });

        // Delta-encode with running status (every message carries its own
        // status byte, so this is straightforward and unambiguous).
        std::vector<uint8_t> track;
        uint32_t prev = 0;
        for (const RawMessage& m : raw)
        {
            const uint32_t delta = m.tick - prev;
            prev = m.tick;
            encodeVLQ(delta, track);
            track.insert(track.end(), m.bytes.begin(), m.bytes.end());
        }

        // Wrap in the MThd + MTrk chunks.
        std::vector<uint8_t> out;
        const uint8_t header[14] = {
            'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
            static_cast<uint8_t>(kTicksPerBeat >> 8),
            static_cast<uint8_t>(kTicksPerBeat & 0xFF)};
        out.insert(out.end(), header, header + sizeof(header));

        const uint32_t len = static_cast<uint32_t>(track.size());
        out.push_back('M');
        out.push_back('T');
        out.push_back('r');
        out.push_back('k');
        out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.insert(out.end(), track.begin(), track.end());

        return out;
    }

    bool run(const std::string& savePath, const std::string& instruction,
             int bpm, double durationMs)
    {
        if (savePath.empty())
        {
            std::cerr << "error: --save-path is required\n";
            return false;
        }
        if (bpm <= 0)
        {
            std::cerr << "error: --tempo must be a positive number of bpm\n";
            return false;
        }
        if (durationMs <= 0.0)
        {
            std::cerr << "error: --duration must be positive\n";
            return false;
        }

        // Create the parent directory if only a path was given.
        std::filesystem::path outPath(savePath);
        if (outPath.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(outPath.parent_path(), ec);
            if (ec)
            {
                std::cerr << "error: could not create directory for " << savePath << "\n";
                return false;
            }
        }

        // ms per tick derived from the tempo: micros-per-beat over pulses/beat.
        const double microPerBeat = 60000000.0 / double(bpm);
        const double msPerTick = microPerBeat / 1000.0 / kTicksPerBeat;
        const uint32_t targetTicks =
            static_cast<uint32_t>(std::lround(durationMs / msPerTick));

        std::vector<std::string> tokens = tokenize(instruction);

        Voice voices[kMaxVoices];
        for (int i = 0; i < kMaxVoices; ++i)
        {
            voices[i].program = static_cast<int>(kDefaultProgram);
        }
        Voice* cur = &voices[0];  // voice 1 is the default

        int octave = kDefaultOctave;
        std::vector<Voice> active;  // voices that actually got a note

        std::size_t i = 0;
        while (i < tokens.size())
        {
            const std::string& tok = tokens[i];

            // A voice start: "voice"/"v" (number on the next token), or a
            // compact form with the number attached, e.g. "v1" / "voice2".
            const std::string lt = toLower(tok);
            int vn = -1;
            bool numberAttached = false;
            if (lt == "voice" || lt == "v")
            {
                // the number comes from the following token
            }
            else if (lt.size() > 1 && lt[0] == 'v' &&
                     std::isdigit(static_cast<unsigned char>(lt[1])))
            {
                int tmp = 0;
                if (parseInt(lt.substr(1), tmp)) vn = tmp;
                numberAttached = true;
            }
            else if (lt.size() > 5 && lt.compare(0, 5, "voice") == 0 &&
                     std::isdigit(static_cast<unsigned char>(lt[5])))
            {
                int tmp = 0;
                if (parseInt(lt.substr(5), tmp)) vn = tmp;
                numberAttached = true;
            }

            if (lt == "voice" || lt == "v" || numberAttached)
            {
                // Resolve the voice number. It is either embedded in the token
                // ("v1" / "voice2") or carried on the following token
                // ("voice 1" / "v 1").
                if (!numberAttached)
                {
                    if (i + 1 >= tokens.size() ||
                        !parseInt(tokens[i + 1], vn) || vn < 1 || vn > 4)
                    {
                        std::cerr << "warning: voice expects a voice number 1-4\n";
                        ++i;  // move past the bare voice keyword
                        continue;
                    }
                }
                else if (vn < 1 || vn > 4)
                {
                    std::cerr << "warning: voice expects a voice number 1-4\n";
                    ++i;  // move past the "vN" / "voiceN" token
                    continue;
                }

                cur = &voices[vn - 1];
                // Consume the voice keyword (+ number), plus an optional
                // shorthand instrument written right after it, e.g.
                // "v1 piano C4 E4" or "voice 2 drums ...". The explicit
                // program/prog/inst keyword is left for its own branch.
                const std::size_t afterVoice = numberAttached ? i + 1 : i + 2;
                std::size_t next = afterVoice;
                if (next < tokens.size())
                {
                    const std::string& nx = tokens[next];
                    const std::string nxl = toLower(nx);
                    if (nxl != "program" && nxl != "prog" && nxl != "inst" && !isNoteLike(nx))
                    {
                        bool drums = false;
                        int prog = resolveProgram(nx, drums);
                        if (prog >= 0)
                        {
                            cur->program = prog;
                            cur->isDrums = drums;
                            next++;
                        }
                        else
                        {
                            std::cerr << "warning: unknown instrument \"" << nx << "\"\n";
                        }
                    }
                }
                i = next;
                continue;
            }
            if (tok == "program" || tok == "prog" || tok == "inst")
            {
                if (i + 1 >= tokens.size())
                {
                    std::cerr << "warning: " << tok << " expects an instrument name\n";
                    continue;
                }
                bool drums = false;
                int prog = resolveProgram(tokens[i + 1], drums);
                if (prog >= 0)
                {
                    setVoiceProgram(*cur, prog, drums);
                    ++i;
                }
                else
                {
                    std::cerr << "warning: unknown instrument \"" << tokens[i + 1] << "\"\n";
                }
                ++i;  // consume the "program" keyword too
                continue;
            }
            if (tok == "o" || tok == "octave")
            {
                if (i + 1 >= tokens.size())
                {
                    ++i;
                    continue;
                }
                int v = 0;
                if (parseInt(tokens[i + 1], v) && v >= 0 && v <= 9)
                {
                    octave = v;
                }
                i += 2;  // consume "o"/"octave" and the value
                continue;
            }
            if (tok == "end" || tok == ";")
            {
                break;
            }
            if (tok == "tempo" || tok == "bpm")
            {
                if (i + 1 >= tokens.size())
                {
                    std::cerr << "warning: " << tok << " expects a bpm value\n";
                    ++i;
                    continue;
                }
                int v = 0;
                if (parseInt(tokens[i + 1], v) && v > 0)
                {
                    bpm = v;
                }
                i += 2;  // consume "tempo"/"bpm" and the value
                continue;
            }

            std::vector<uint8_t> keys;
            uint32_t length = 0;
            if (!parseToken(tok, octave, keys, length))
            {
                std::cerr << "warning: skipping unrecognized token \"" << tok << "\"\n";
                ++i;
                continue;
            }

            // Velocity gently oscillates per note token so the line breathes.
            cur->tokens++;
            const uint8_t velocity =
                static_cast<uint8_t>((90 + ((cur->tokens - 1) % 6) * 3) & 0x7F);

            for (uint8_t key : keys)
            {
                NoteEvent ne;
                ne.tick = cur->cursor;
                ne.key = key;
                ne.velocity = velocity;
                ne.length = length;
                cur->notes.push_back(ne);
            }
            cur->cursor += length;
            ++i;
        }

        for (int i = 0; i < kMaxVoices; ++i)
        {
            if (!voices[i].notes.empty())
            {
                active.push_back(voices[i]);
            }
        }

        if (active.empty())
        {
            std::cerr << "error: no notes could be parsed from the instruction\n";
            return false;
        }

        const std::vector<uint8_t> midi = buildMidi(active, targetTicks, static_cast<uint32_t>(bpm));

        std::ofstream file(outPath, std::ios::binary);
        if (!file)
        {
            std::cerr << "error: cannot open " << savePath << " for writing\n";
            return false;
        }
        file.write(reinterpret_cast<const char*>(midi.data()),
                   static_cast<std::streamsize>(midi.size()));
        if (!file)
        {
            std::cerr << "error: failed while writing " << savePath << "\n";
            return false;
        }

        std::cout << "wrote " << outPath.string() << " (" << midi.size()
                  << " bytes, " << active.size() << " voices, " << bpm << " bpm, "
                  << static_cast<int>(durationMs / 1000.0) << " s)\n";
        for (const Voice& v : active)
        {
            std::cout << "  voice on channel " << (v.channel & 0x0F)
                      << (v.isDrums ? " (drums)" : "") << ", program " << v.program
                      << ", " << v.notes.size() << " notes\n";
        }
        return true;
    }
}  // namespace

int main(int argc, char** argv)
{
    std::string savePath;
    std::string instruction = kDefaultInstruction;
    int bpm = kDefaultBpm;
    double durationMs = kDefaultDurationMs * 1000.0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --" << what << " expects a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--save-path")
        {
            savePath = next("save-path");
        }
        else if (arg == "--instruction")
        {
            instruction = next("instruction");
        }
        else if (arg == "--tempo")
        {
            int v = 0;
            if (!parseInt(next("tempo"), v))
            {
                std::cerr << "error: --tempo expects a number\n";
                return 2;
            }
            bpm = v;
        }
        else if (arg == "--duration")
        {
            const std::string v = next("duration");
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || d <= 0.0)
            {
                std::cerr << "error: --duration expects a positive number\n";
                return 2;
            }
            durationMs = d * 1000.0;
        }
        else if (arg == "-?" || arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else
        {
            std::cerr << "error: unknown argument \"" << arg << "\"\n";
            printUsage();
            return 2;
        }
    }

    if (!run(savePath, instruction, bpm, durationMs))
    {
        return 1;
    }
    return 0;
}
