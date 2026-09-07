// ---------------------------------------------------------------------------
// mid2ogg - render a Standard MIDI File (.mid) into an Ogg Vorbis audio file
//           (.ogg) using a SoundFont for instrument samples.
//
// A .mid file only contains notes (pitch, velocity, timing, program changes);
// it carries no actual audio. To turn it into a playable sound we have to
// *playback* the MIDI against an instrument bank (a SoundFont) and record the
// rendered samples. This tool does exactly that:
//
//   1. Loads the SoundFont (.sf2) with TinySoundFont (tsf.h).
//   2. Loads the MIDI (tml.h) and replays its messages on a virtual clock,
//      asking TinySoundFont to synthesise the audio the same way the game does
//      (see src/pimbalgame/Music.cpp).
//   3. Collects the interleaved 16-bit / 44.1 kHz PCM and hands it to SFML,
//      which encodes and writes it straight to an Ogg Vorbis file.
//
// Build: -DBUILD_TOOLS=ON (the default). Links SFML::Audio for the OGG writer.
// ---------------------------------------------------------------------------

#include <SFML/Audio.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tsf.h"
#include "tml.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_IMPLEMENTATION
#include "tml.h"

namespace
{
    constexpr unsigned int kSampleRate = 44100;
    constexpr unsigned int kChannels = 2;

    // TSF renders in blocks of 64 samples. We render a few blocks per chunk
    // (256 sample frames) so the MIDI is advanced frequently enough to stay in
    // sync while keeping per-chunk work modest.
    constexpr int kBlocksPerChunk = 4;
    constexpr int kChunkSamples = 64 * kBlocksPerChunk;

    // Upper bound on how long the rendered track can be, used only to size the
    // PCM reserve and as a safety cap. The actual end of the audio is decided
    // later by trimming trailing/leading silence (see lastLoudEnd/firstLoudStart).
    constexpr int kDecayTailMs = 5000;

    // Default SoundFont. Resolved relative to the current directory first, then
    // relative to the project root (two levels up from this binary), so the tool
    // works whether it is launched from the project root or from build/bin.
    constexpr const char* kDefaultSoundFont = "temp/soundfont/FluidR3_GM.sf2";

    // Silence trimming -------------------------------------------------------
    // A MIDI render ends with the instruments' decaying/reverb tail, which fades
    // into dead air. To export a file that starts and ends on real audio (so it
    // can be looped with no silence between repeats), we analyse the PCM from
    // both ends and drop the near-silent windows. An RMS energy window is used
    // (rather than a single sample's peak) so one quiet echo in an otherwise
    // silent region does not count as content.

    // Window length in sample frames (~46 ms). Expressed in frames so it is the
    // same regardless of channel count.
    constexpr std::size_t kSilenceWindowFrames = 2048;

    // A window is "audio" when its RMS level reaches this fraction of full scale
    // (~-34 dBFS). Below it we treat the region as silence.
    constexpr double kSilenceRmsFraction = 0.02;

    constexpr std::size_t kSilenceWindowSamples = kSilenceWindowFrames * kChannels;

    // Index just past the end of the last window whose RMS is above the
    // silence threshold. Returns pcm.size() when no such window exists (the
    // whole render is quiet, so nothing is trimmed from the end).
    std::size_t lastLoudEnd(const std::vector<std::int16_t>& pcm)
    {
        const std::size_t window = kSilenceWindowSamples;
        std::size_t end = pcm.size();
        while (end >= window)
        {
            const std::size_t start = end - window;
            double rms = 0.0;
            for (std::size_t i = start; i < end; ++i)
            {
                const double s = static_cast<double>(pcm[i]);
                rms += s * s;
            }
            rms = std::sqrt(rms / static_cast<double>(window));
            if (rms >= kSilenceRmsFraction * 32767.0)
            {
                return end;
            }
            end = start;
        }
        return pcm.size();
    }

    // Index of the first sample of the first window (from the start) whose RMS
    // is above the silence threshold. Returns 0 when no such window exists.
    std::size_t firstLoudStart(const std::vector<std::int16_t>& pcm)
    {
        const std::size_t window = kSilenceWindowSamples;
        for (std::size_t start = 0; start + window <= pcm.size(); start += window)
        {
            const std::size_t end = start + window;
            double rms = 0.0;
            for (std::size_t i = start; i < end; ++i)
            {
                const double s = static_cast<double>(pcm[i]);
                rms += s * s;
            }
            rms = std::sqrt(rms / static_cast<double>(window));
            if (rms >= kSilenceRmsFraction * 32767.0)
            {
                return start;
            }
        }
        return 0;
    }

    void printUsage(const char* argv0)
    {
        std::printf(
            "mid2ogg - render a MIDI file into an Ogg Vorbis (.ogg) sound file.\n"
            "\n"
            "usage:\n"
            "  %s --save-path <out.ogg> --mid-path <in.mid> [options]\n"
            "\n"
            "options:\n"
            "  --save-path <p>        Output .ogg file path (required).\n"
            "  --mid-path <p>         Input .mid file path (required).\n"
            "  --soundfont-path <p>   SoundFont (.sf2) to render against.\n"
            "                         (default: %s)\n"
            "  -?, --help             Show this help and exit.\n"
            "\n"
            "Run from the project root, or pass absolute paths.\n",
            argv0, kDefaultSoundFont);
    }

    std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        std::vector<std::uint8_t> bytes;
        if (size > 0)
        {
            bytes.resize(static_cast<std::size_t>(size));
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        }
        return bytes;
    }

    std::string formatError(const sf::SoundBuffer&)
    {
        // SFML's SoundBuffer::saveToFile returns false with no message; the most
        // common cause is the audio module being built without an Ogg/Vorbis
        // writer, so surface a helpful hint.
        return "unknown error (SFML could not write the .ogg; "
               "rebuild with SFML_BUILD_AUDIO=ON and system Vorbis available)";
    }

    // Play every MIDI message due up to (and including) `currentTimeMs`. Mirrors
    // the in-game playback in src/pimbalgame/Music.cpp.
    void advanceMidi(tml_message*& next, tsf* soundFont, double currentTimeMs)
    {
        while (next != nullptr && next->time <= static_cast<unsigned int>(currentTimeMs))
        {
            switch (next->type)
            {
                case TML_PROGRAM_CHANGE:
                    tsf_channel_set_presetnumber(
                        soundFont, next->channel, next->program, next->channel == 9);
                    break;
                case TML_NOTE_ON:
                    tsf_channel_note_on(
                        soundFont, next->channel, next->key, next->velocity / 127.0f);
                    break;
                case TML_NOTE_OFF:
                    tsf_channel_note_off(soundFont, next->channel, next->key);
                    break;
                case TML_PITCH_BEND:
                    tsf_channel_set_pitchwheel(soundFont, next->channel, next->pitch_bend);
                    break;
                case TML_CONTROL_CHANGE:
                    tsf_channel_midi_control(
                        soundFont, next->channel, next->control, next->control_value / 127.0f);
                    break;
                default:
                    break;
            }
            next = next->next;
        }
    }
}

int main(int argc, char** argv)
{
    std::string savePath;
    std::string midPath;
    std::string soundFontPath = kDefaultSoundFont;

    // ---- Parse the command line -------------------------------------------
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto nextValue = [&](const char* name) -> std::string
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "error: missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--save-path" || arg == "--save-path=")
        {
            savePath = (arg == "--save-path=") ? arg.substr(std::string("--save-path=").size())
                                               : nextValue("--save-path");
        }
        else if (arg == "--mid-path" || arg == "--mid-path=")
        {
            midPath = (arg == "--mid-path=") ? arg.substr(std::string("--mid-path=").size())
                                             : nextValue("--mid-path");
        }
        else if (arg == "--soundfont-path" || arg == "--soundfont-path=")
        {
            soundFontPath =
                (arg == "--soundfont-path=") ? arg.substr(std::string("--soundfont-path=").size())
                                             : nextValue("--soundfont-path");
        }
        else if (arg == "-?" || arg == "--help" || arg == "-h")
        {
            printUsage(argv[0]);
            return 0;
        }
        else
        {
            std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    if (savePath.empty() || midPath.empty())
    {
        std::fprintf(stderr, "error: --save-path and --mid-path are both required\n\n");
        printUsage(argv[0]);
        return 2;
    }

    // ---- Resolve input paths ----------------------------------------------
    // A relative path is used against the current directory first; if it is not
    // found there we retry walking up from the executable (build/bin -> build ->
    // project root) so the tool is usable when launched from build/bin as well.
    auto resolveExisting = [&](const std::string& path) -> std::filesystem::path
    {
        std::filesystem::path p(path);
        if (std::filesystem::exists(p))
        {
            return p;
        }
        if (!p.is_absolute())
        {
            // Normalise first: a relative argv[0] such as "./mid2ogg" survives as
            // ".../build/bin/./mid2ogg", whose parent keeps the stray "./" and would
            // shift every upward step by one level. lexically_normal() collapses it.
            const std::filesystem::path exeDir =
                std::filesystem::absolute(argv[0]).lexically_normal().parent_path();
            for (int up = 0; up <= 3; ++up)
            {
                std::filesystem::path candidate = exeDir;
                for (int u = 0; u < up; ++u)
                {
                    candidate = candidate.parent_path();
                }
                candidate /= path;
                if (std::filesystem::exists(candidate))
                {
                    return candidate;
                }
            }
        }
        return p; // return the original (missing) path for a clear error message
    };

    const std::filesystem::path saveP = savePath;
    const std::filesystem::path midP = resolveExisting(midPath);
    const std::filesystem::path sf2P = resolveExisting(soundFontPath);

    // ---- Load the assets --------------------------------------------------
    const std::vector<std::uint8_t> sf2 = readFile(sf2P);
    if (sf2.empty())
    {
        std::fprintf(stderr, "FAIL: could not read SoundFont '%s' (exists=%s)\n",
                     sf2P.c_str(), std::filesystem::exists(sf2P) ? "yes" : "no");
        return 1;
    }
    const std::vector<std::uint8_t> mid = readFile(midP);
    if (mid.empty())
    {
        std::fprintf(stderr, "FAIL: could not read MIDI '%s' (exists=%s)\n",
                     midP.c_str(), std::filesystem::exists(midP) ? "yes" : "no");
        return 1;
    }

    tsf* soundFont = tsf_load_memory(sf2.data(), static_cast<int>(sf2.size()));
    if (!soundFont)
    {
        std::fprintf(stderr, "FAIL: TinySoundFont could not load the SoundFont\n");
        return 1;
    }
    tml_message* messages = tml_load_memory(mid.data(), static_cast<int>(mid.size()));
    if (!messages)
    {
        std::fprintf(stderr, "FAIL: TinyMIDI (tml) could not load the MIDI\n");
        tsf_close(soundFont);
        return 1;
    }

    // ---- Render the whole track -------------------------------------------
    unsigned int noteLengthMs = 0;
    tml_get_info(messages, nullptr, nullptr, nullptr, nullptr, &noteLengthMs);

    // Stereo interleaved 16-bit output at 44.1 kHz.
    tsf_set_output(soundFont, TSF_STEREO_INTERLEAVED, kSampleRate, 0);

    std::vector<std::int16_t> pcm;
    pcm.reserve(
        static_cast<std::size_t>((static_cast<double>(noteLengthMs) + kDecayTailMs) *
                                 kSampleRate * kChannels / 1000.0));

    std::vector<std::int16_t> chunk(static_cast<std::size_t>(kChunkSamples) * kChannels);
    double currentTimeMs = 0.0;
    const double durationMs = static_cast<double>(noteLengthMs) + kDecayTailMs;
    tml_message* next = messages;

    // Safety cap so a stuck/never-decaying voice can never hang the tool.
    const double maxRenderMs = durationMs + 10000.0;

    while (next != nullptr || tsf_active_voice_count(soundFont) > 0)
    {
        // Advance the virtual playback clock for this chunk, then play any MIDI
        // messages due up to this point before rendering.
        currentTimeMs += kChunkSamples * 1000.0 / kSampleRate;
        advanceMidi(next, soundFont, currentTimeMs);

        std::memset(chunk.data(), 0, sizeof(chunk));
        tsf_render_short(soundFont, reinterpret_cast<short*>(chunk.data()), kChunkSamples, 0);

        pcm.insert(pcm.end(), chunk.begin(), chunk.end());

        // Stop once the MIDI is exhausted and every voice has finished decaying,
        // so a looping file would start from silence. Cap the render time too.
        if (next == nullptr && tsf_active_voice_count(soundFont) == 0)
        {
            break;
        }
        if (currentTimeMs >= durationMs || currentTimeMs >= maxRenderMs)
        {
            break;
        }
    }

    tml_free(messages);
    tsf_close(soundFont);

    if (pcm.empty())
    {
        std::fprintf(stderr, "FAIL: the MIDI produced no audio\n");
        return 1;
    }

    // ---- Trim trailing/leading silence ------------------------------------
    // Drop the near-silent windows at both ends so the file starts and ends on
    // real audio. This removes the decaying/reverb tail (which would otherwise
    // show up as dead air) and lets the result loop with no silence between
    // repeats. We cut exactly at the detected audio boundary (no fade) so no
    // silence is left behind; fading to zero would re-add the very silence we
    // are removing.
    const std::size_t lead = firstLoudStart(pcm);
    const std::size_t tail = lastLoudEnd(pcm);
    const bool hasAudio = (tail > lead);
    const bool hasSilenceToTrim = (lead > 0 || tail < pcm.size());

    if (hasAudio && hasSilenceToTrim)
    {
        pcm.erase(pcm.begin(),
                  pcm.begin() + static_cast<std::vector<std::int16_t>::difference_type>(lead));
        pcm.resize(tail);
    }

    const double seconds = static_cast<double>(pcm.size()) / kSampleRate / kChannels;
    std::printf("RENDER ok: midi=%s  soundfont=%s\n"
                "  notes=%u ms  audio=%.3f s  samples=%zu (stereo %u Hz)\n",
                midP.c_str(), sf2P.c_str(), noteLengthMs, seconds, pcm.size(), kSampleRate);

    // ---- Encode and save as Ogg Vorbis via SFML ---------------------------
    sf::SoundBuffer buffer;
    if (!buffer.loadFromSamples(pcm.data(), static_cast<std::uint64_t>(pcm.size()), kChannels,
                                kSampleRate,
                                {sf::SoundChannel::FrontLeft, sf::SoundChannel::FrontRight}))
    {
        std::fprintf(stderr, "FAIL: could not build the SFML audio buffer\n");
        return 1;
    }
    if (!buffer.saveToFile(saveP))
    {
        std::fprintf(stderr, "FAIL: could not save '%s' -- %s\n", saveP.c_str(),
                     formatError(buffer).c_str());
        return 1;
    }

    std::printf("WROTE %s (%s)\n", saveP.string().c_str(),
                std::filesystem::exists(saveP) ? "done" : "check path");
    return 0;
}
