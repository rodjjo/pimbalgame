// ---------------------------------------------------------------------------
// text2mid - synthesise a Standard MIDI File (.mid) from a melody written in
// the "mel" note language.
//
// The tool is completely self-contained: it needs only the C++ standard
// library and links against nothing external. It parses the instruction
// string into note events, repeats the phrase until the file is a comfortable
// background-music length (20 s by default) and writes a format-0 Standard
// MIDI File that SFML's TinySoundFont playback path renders against a sound
// font.
// ---------------------------------------------------------------------------

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
    // The "mel" melody language
    //
    // The instruction is a whitespace- and comma-separated stream of tokens.
    // The recognised tokens are:
    //
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

    void printUsage()
    {
        std::cout <<
            "text2mid - synthesise a Standard MIDI File from a melody written in the\n"
            "'mel' note language.\n"
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
            "  Note    [A-G] [#..|b..] [0-9]   C, C#, Fb, B4, e5      (C4 == MIDI 60)\n"
            "  Rest    r  or  -\n"
            "  Chord   C+E+G                    sounds together\n"
            "  Duration /denominator           /1 whole, /2 half, /4 quarter (default),\n"
            "                                  /8 eighth, /16 sixteenth\n"
            "  Tempo   tempo <bpm> | bpm <bpm>\n"
            "  Octave  o <0-9>   | octave <0-9>\n"
            "  Bar     |   (ignored)   End   end | ;\n"
            "\n"
            "Example:\n"
            "  text2mid --save-path theme.mid --instruction\n"
            "           \"tempo 110 C4/4 E4/4 G4/4 B4/4 | C5/2 B4/2\"\n";
    }

    // The default theme, used when --instruction is not supplied. A short,
    // looping, major-key phrase that sits comfortably under a sound font.
    const char* kDefaultInstruction =
        "tempo 104 "
        "E5/8 G5/8 A5/8 C6/8 B5/8 G5/8 E5/8 D5/8 "
        "C5/8 D5/8 E5/8 F5/8 G5/4 r/4 | "
        "G4/4 B4/4 D5/4 F4/4 "
        "A4/8 C5/8 E5/8 A5/8 G5/8 E5/8 C5/8 A4/8 "
        "G4/8 B4/8 D5/8 G5/8 F5/8 D5/8 B4/8 G4/8 "
        "C5/2 G4/4 r/4";

    // -----------------------------------------------------------------------
    // Standard MIDI File (format 0, single track) writer.
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

    // Build the full MIDI byte stream: header + one track. The phrase is
    // repeated to fill `targetTicks`, then a final note-off burst and an
    // end-of-track message close the file off at the target length.
    std::vector<uint8_t> buildMidi(const std::vector<NoteEvent>& baseNotes,
                                   uint32_t baseLenTicks, uint32_t targetTicks,
                                   uint8_t program, uint32_t bpm)
    {
        const uint32_t cap = targetTicks > 0 ? targetTicks : baseLenTicks;

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
        meta(0x03, "text2mid");   // description
        meta(0x0F, "PimBalGame theme");  // track name

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

        // program change: channel 0, chosen instrument (a bright lead).
        {
            RawMessage m;
            m.tick = 0;
            m.bytes.push_back(0xC0 | (program & 0x0F));
            m.bytes.push_back(program & 0x7F);
            raw.push_back(std::move(m));
        }

        // Notes, repeated to fill the target length. A note that starts at or
        // after the cut is dropped; a note crossing the cut is clipped at cap.
        bool capReached = false;
        for (uint32_t global = 0; !capReached; global += baseLenTicks)
        {
            for (const NoteEvent& n : baseNotes)
            {
                const uint32_t start = global + n.tick;
                if (start >= cap)
                {
                    capReached = true;
                    break;
                }
                const uint32_t end = std::min<uint32_t>(cap, start + n.length);
                RawMessage on;
                on.tick = start;
                on.bytes = {0x90, n.key, n.velocity};  // note-on, channel 0
                raw.push_back(on);
                RawMessage off;
                off.tick = end;
                off.bytes = {0x80, n.key, 0x40};      // note-off, channel 0
                raw.push_back(off);
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
        // insertion order (meta/program first, notes in sequence, eot last).
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

        std::vector<NoteEvent> notes;
        int octave = kDefaultOctave;
        uint32_t cursor = 0;  // ticks into the phrase
        const int velocityBase = 90;
        int voice = 0;

        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            const std::string& tok = tokens[i];
            if (tok == "tempo" || tok == "bpm")
            {
                if (i + 1 >= tokens.size())
                {
                    std::cerr << "warning: " << tok << " expects a bpm value\n";
                    continue;
                }
                int v = 0;
                if (parseInt(tokens[++i], v) && v > 0)
                {
                    bpm = v;
                }
                continue;
            }
            if (tok == "o" || tok == "octave")
            {
                if (i + 1 >= tokens.size())
                {
                    continue;
                }
                int v = 0;
                if (parseInt(tokens[++i], v) && v >= 0 && v <= 9)
                {
                    octave = v;
                }
                continue;
            }
            if (tok == "end" || tok == ";")
            {
                break;
            }

            std::vector<uint8_t> keys;
            uint32_t length = 0;
            if (!parseToken(tok, octave, keys, length))
            {
                std::cerr << "warning: skipping unrecognized token \"" << tok << "\"\n";
                continue;
            }

            // Velocity gently oscillates so the line breathes.
            const uint8_t velocity =
                static_cast<uint8_t>((velocityBase + ((voice % 6) * 3)) & 0x7F);

            for (uint8_t key : keys)
            {
                NoteEvent ne;
                ne.tick = cursor;
                ne.key = key;
                ne.velocity = velocity;
                ne.length = length;
                notes.push_back(ne);
            }
            cursor += length;
            ++voice;
        }

        if (notes.empty())
        {
            std::cerr << "error: no notes could be parsed from the instruction\n";
            return false;
        }

        const uint32_t baseLenTicks = cursor > 0 ? cursor : kTicksPerBeat;
        const std::vector<uint8_t> midi = buildMidi(notes, baseLenTicks, targetTicks, 74, bpm);

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
                  << " bytes, " << notes.size() << " notes, " << bpm << " bpm, "
                  << static_cast<int>(durationMs / 1000.0) << " s)\n";
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
