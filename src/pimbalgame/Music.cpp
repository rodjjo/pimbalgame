#include "pimbalgame/Music.hpp"

#include "tsf.h"
#include "tml.h"

#include <cstring>
#include <fstream>
#include <vector>

// Pull in the TinySoundFont (tsf.h) and TinyMIDI (tml.h) implementations in this
// single translation unit. Everything else only needs the headers (included via
// the include directory in CMake).
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_IMPLEMENTATION
#include "tml.h"

namespace pimbalgame
{
namespace
{
    constexpr unsigned int kSampleRate = 44100;
    constexpr unsigned int kChannels = 2;

    // TSF renders in blocks of 64 samples. We render a few blocks per chunk
    // (256 sample frames) so the MIDI is advanced frequently enough to stay in
    // sync while keeping per-chunk work modest.
    constexpr int kBlocksPerChunk = 4;
    constexpr int kChunkSamples = 64 * kBlocksPerChunk;

    // Extra tail rendered past the end of the MIDI so a decaying note lands the
    // loop point in silence instead of clicking.
    constexpr int kDecayTailMs = 5000;

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
            file.read(reinterpret_cast<char*>(bytes.data()), size);
        }
        return bytes;
    }

    // Advance the MIDI playback to `currentTimeMs` and dispatch every message
    // scheduled up to (and including) that time onto the sound font.
    void advanceMidi(tml_message*& next, tsf* soundFont, double currentTimeMs)
    {
        while (next != nullptr && next->time <= static_cast<unsigned int>(currentTimeMs))
        {
            switch (next->type)
            {
                case TML_PROGRAM_CHANGE:
                    // Channel 9 (zero-based) is the MIDI drums channel.
                    tsf_channel_set_presetnumber(soundFont, next->channel, next->program,
                                                 next->channel == 9);
                    break;
                case TML_NOTE_ON:
                    tsf_channel_note_on(soundFont, next->channel, next->key, next->velocity / 127.0f);
                    break;
                case TML_NOTE_OFF:
                    tsf_channel_note_off(soundFont, next->channel, next->key);
                    break;
                case TML_PITCH_BEND:
                    tsf_channel_set_pitchwheel(soundFont, next->channel, next->pitch_bend);
                    break;
                case TML_CONTROL_CHANGE:
                    tsf_channel_midi_control(soundFont, next->channel, next->control,
                                             next->control_value);
                    break;
                default:
                    break;
            }
            next = next->next;
        }
    }
}

Music::Music() = default;
Music::~Music() = default;

bool Music::load(const std::filesystem::path& soundFontPath, const std::filesystem::path& midiPath)
{
    const std::vector<std::uint8_t> sf2Bytes = readFile(soundFontPath);
    if (sf2Bytes.empty())
    {
        return false;
    }

    const std::vector<std::uint8_t> midBytes = readFile(midiPath);
    if (midBytes.empty())
    {
        return false;
    }

    tsf* soundFont = tsf_load_memory(sf2Bytes.data(), static_cast<int>(sf2Bytes.size()));
    if (!soundFont)
    {
        return false;
    }

    tml_message* messages = tml_load_memory(midBytes.data(), static_cast<int>(midBytes.size()));
    if (!messages)
    {
        tsf_close(soundFont);
        return false;
    }

    // Total track length in milliseconds, used to size the cache and stop
    // rendering once the MIDI is exhausted.
    unsigned int noteLengthMs = 0;
    tml_get_info(messages, nullptr, nullptr, nullptr, nullptr, &noteLengthMs);

    // Stereo interleaved 16-bit output at 44.1 kHz.
    tsf_set_output(soundFont, TSF_STEREO_INTERLEAVED, kSampleRate, 0);

    // Render the whole track into the PCM cache.
    mPcm.clear();
    mPcm.reserve(static_cast<std::size_t>(
        (static_cast<double>(noteLengthMs) + kDecayTailMs) * kSampleRate * kChannels / 1000.0));

    std::vector<std::int16_t> chunk(static_cast<std::size_t>(kChunkSamples) * kChannels);
    double currentTimeMs = 0.0;
    const double durationMs = static_cast<double>(noteLengthMs) + kDecayTailMs;
    tml_message* next = messages;

    while (next != nullptr || tsf_active_voice_count(soundFont) > 0)
    {
        // Advance the virtual playback clock for this chunk, then play any MIDI
        // messages due up to this point before rendering.
        currentTimeMs += kChunkSamples * 1000.0 / kSampleRate;
        advanceMidi(next, soundFont, currentTimeMs);

        std::memset(chunk.data(), 0, sizeof(chunk));
        tsf_render_short(soundFont, reinterpret_cast<short*>(chunk.data()), kChunkSamples, 0);

        mPcm.insert(mPcm.end(), chunk.begin(), chunk.end());

        // Stop once the MIDI is exhausted and every voice has finished decaying,
        // so the (looped) playback starts from silence. Cap the render time as a
        // safety net in case a voice never fully decays.
        if (next == nullptr && tsf_active_voice_count(soundFont) == 0)
        {
            break;
        }
        if (currentTimeMs >= durationMs)
        {
            break;
        }
    }

    tml_free(messages);
    tsf_close(soundFont);

    if (mPcm.empty())
    {
        return false;
    }

    // Hand the cached PCM to SFML. The buffer takes its own copy of the samples.
    if (!mBuffer.loadFromSamples(
            mPcm.data(),
            static_cast<std::uint64_t>(mPcm.size()),
            kChannels,
            kSampleRate,
            {sf::SoundChannel::FrontLeft, sf::SoundChannel::FrontRight}))
    {
        return false;
    }

    mSound = std::make_unique<sf::Sound>(mBuffer);
    mSound->setLooping(true);
    mValid = true;
    return true;
}

void Music::play()
{
    if (mValid && mSound)
    {
        mSound->play();
    }
}

void Music::pause()
{
    if (mSound)
    {
        mSound->pause();
    }
}

void Music::stop()
{
    if (mSound)
    {
        mSound->stop();
    }
}

bool Music::isPlaying() const
{
    return mValid && mSound && mSound->getStatus() == sf::SoundSource::Status::Playing;
}
} // namespace pimbalgame
