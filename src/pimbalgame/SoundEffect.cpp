#include "pimbalgame/SoundEffect.hpp"

#include <cmath>
#include <sstream>

namespace pimbalgame
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // File-local mirror of the class constant so the free helper below can reach
    // it (it can't see the private static members).
    constexpr unsigned int kSampleRate = 44100;
    constexpr std::size_t kMaxFrames = 44100 * 2;  // 2 s cap per effect

    enum class Waveform
    {
        Sine,
        Square,
        Saw,
        Triangle,
    };

    // Clamp synthesis to an audible, artifact-free range.
    constexpr double kMinFreq = 60.0;
    constexpr double kMaxFreq = 8000.0;
    // Fast attack (~2 ms) then exponential decay, so notes start/stop without clicks.
    constexpr double kAttackSec = 0.002;

    double noteToFrequency(char letter, bool sharp, bool flat, int octave)
    {
        // Semitone index within an octave: C=0, C#/Db=1, D=2, ...
        int semitone;
        switch (letter)
        {
            case 'C': semitone = 0; break;
            case 'D': semitone = 2; break;
            case 'E': semitone = 4; break;
            case 'F': semitone = 5; break;
            case 'G': semitone = 7; break;
            case 'A': semitone = 9; break;
            case 'B': semitone = 11; break;
            default:  semitone = 0; break;
        }
        semitone += (sharp ? 1 : 0) - (flat ? 1 : 0);

        // MIDI number (A4 = 69). C4 = (4 + 1) * 12 + 0 = 60.
        const int midi = (octave + 1) * 12 + semitone;
        return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    }

    // Duration suffix -> beats (quarter note == 1 beat). Empty suffix == quarter.
    double beatsForSuffix(const std::string& suffix)
    {
        if (suffix.empty())
        {
            return 1.0;
        }
        switch (suffix[0])
        {
            case 'w': return 4.0;
            case 'h': return 2.0;
            case 'q':
            case ' ':
            case '\t': return 1.0;
            case 'e': return 0.5;
            case 's': return 0.25;
            case 't': return 1.0 / 3.0;
            default:  return 1.0;
        }
    }

    // Sample value for one waveform given a 0..1 phase `r` and frequency.
    double waveformSample(Waveform wave, double phase)
    {
        const double r = phase - std::floor(phase);  // 0..1
        switch (wave)
        {
            case Waveform::Sine:
                return std::sin(2.0 * kPi * r);
            case Waveform::Square:
                return r < 0.5 ? 1.0 : -1.0;
            case Waveform::Saw:
                return 2.0 * r - 1.0;
            case Waveform::Triangle:
                return r < 0.5 ? (4.0 * r - 1.0) : (3.0 - 4.0 * r);
        }
        return 0.0;
    }

    // Append one pitched note (or a rest) to the PCM buffer.
    void appendNote(std::vector<std::int16_t>& out, double frequency, double beats,
                    double bpm, Waveform wave)
    {
        const double framesD = beats * (bpm / 60.0) * kSampleRate;
        const int frames = static_cast<int>(framesD);
        if (frames <= 0 || out.size() >= kMaxFrames)
        {
            return;
        }
        const int count = std::min<int>(frames, static_cast<int>(kMaxFrames - out.size()));

        const double freq = std::min<double>(kMaxFreq, std::max<double>(kMinFreq, frequency));
        const double attackFrames = std::max(1, static_cast<int>(kAttackSec * kSampleRate));
        const double amplitude = 0.8;  // headroom so stacked effects don't clip

        for (int i = 0; i < count; ++i)
        {
            // Envelope: linear attack, then exponential decay.
            double env;
            if (i < attackFrames)
            {
                env = amplitude * (static_cast<double>(i) / attackFrames);
            }
            else
            {
                env = amplitude * std::exp(-6.0 * ((static_cast<double>(i) / kSampleRate) - kAttackSec));
            }

            const double phase = static_cast<double>(i) * freq / kSampleRate;
            const double s = waveformSample(wave, phase);

            const std::int16_t sample = static_cast<std::int16_t>(std::lround(env * s * 32767.0));
            out.push_back(sample);
        }
    }
}

// ---------------------------------------------------------------------------
// VoiceBank: a single worker's private replay state.
// ---------------------------------------------------------------------------

void SoundEffect::VoiceBank::loadFromPcm(const std::map<std::string, std::vector<std::int16_t>>& pcm)
{
    for (const auto& [name, samples] : pcm)
    {
        Effect& effect = effects[name];

        // This worker's own copy of the buffer ...
        if (!effect.buffer.loadFromSamples(
                samples.data(), samples.size(), 1, kSampleRate,
                {sf::SoundChannel::Mono}))
        {
            effects.erase(name);
            continue;
        }

        // ... plus a ring of voices permanently bound to it. No setBuffer() is
        // ever needed on the hot path, so the buffers' attached-sound lists are
        // never touched concurrently by more than one worker.
        effect.voices.reserve(kVoicesPerEffect);
        for (int i = 0; i < kVoicesPerEffect; ++i)
        {
            effect.voices.push_back(std::make_unique<sf::Sound>(effect.buffer));
        }
    }
}

void SoundEffect::VoiceBank::play(const std::string& name)
{
    const auto it = effects.find(name);
    if (it == effects.end())
    {
        return;
    }
    Effect& effect = it->second;
    if (effect.voices.empty())
    {
        return;
    }

    // Restart `name` on the next voice in the ring. When every voice is busy the
    // ring wraps to the earliest-started one, cutting it off (polyphony).
    sf::Sound* voice = effect.voices[effect.next].get();
    effect.next = (effect.next + 1) % effect.voices.size();
    voice->play();
}

// ---------------------------------------------------------------------------
// SoundEffect
// ---------------------------------------------------------------------------

SoundEffect::SoundEffect()
{
    // Build the effect database up front so every sound is ready to fire as soon
    // as the game starts. Each string is a tiny procedural composition.
    addSound("plunger_down",     "~saw C3e E3e G3e");        // pulling the plunger: rising charge
    addSound("plunger_up",       "~square E4e G4e C5q");     // releasing: launch arpeggio
    addSound("ball_hit_bumper",  "~sine E5s G5s C6h");       // bright chime
    addSound("ball_hit_wall",    "~triangle G3e");           // soft tap
    addSound("ball_hit_flipper","~square G4s B4s");          // punchy tock
    addSound("ball_drain",       "~sine E4s C3h");           // losing the ball: falling tone

    // Give each worker its own private voice bank, built from the (now read-only)
    // PCM database on the main thread, before any worker starts.
    mBanks.resize(kWorkers);
    for (auto& bank : mBanks)
    {
        bank.loadFromPcm(mSounds);
    }

    // Start the playback workers. Each pulls names off the shared queue and
    // replays them on its own mBanks[i].
    mRunning.store(true);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWorkers); ++i)
    {
        mWorkers.emplace_back([this, i]() { this->workerLoop(i); });
    }
}

SoundEffect::~SoundEffect()
{
    // Tell the workers to finish, wake them, and join so none is still replaying
    // when mBanks/voices are destroyed below.
    mRunning.store(false);
    mQueueCond.notify_all();
    for (auto& t : mWorkers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
}

bool SoundEffect::addSound(const std::string& name, const std::string& language)
{
    std::vector<std::int16_t> pcm;
    if (!render(language, pcm) || pcm.empty())
    {
        return false;
    }
    mSounds[name] = std::move(pcm);
    return true;
}

bool SoundEffect::render(const std::string& language, std::vector<std::int16_t>& outPcm)
{
    outPcm.clear();
    if (language.empty())
    {
        return false;
    }

    double bpm = 120.0;
    Waveform wave = Waveform::Triangle;

    std::istringstream stream(language);
    std::string token;
    while (stream >> token)
    {
        if (token.empty())
        {
            continue;
        }

        if (token[0] == '@')  // tempo
        {
            try
            {
                bpm = std::stod(token.substr(1));
            }
            catch (...)
            {
                // keep the current tempo on a malformed value
            }
            if (bpm <= 0.0)
            {
                bpm = 120.0;
            }
        }
        else if (token[0] == '~')  // waveform
        {
            const std::string w = token.substr(1);
            if (w == "sine")
            {
                wave = Waveform::Sine;
            }
            else if (w == "square")
            {
                wave = Waveform::Square;
            }
            else if (w == "saw" || w == "sawtooth")
            {
                wave = Waveform::Saw;
            }
            else
            {
                wave = Waveform::Triangle;  // default
            }
        }
        else if (token[0] == 'R' || token[0] == 'r')  // rest
        {
            appendNote(outPcm, 0.0, beatsForSuffix(token.substr(1)), bpm, wave);
        }
        else  // note: letter, optional accidental, octave, optional suffix
        {
            const char letter = static_cast<char>(std::toupper(token[0]));

            bool sharp = false, flat = false;
            size_t pos = 1;
            if (pos < token.size() && (token[pos] == '#'))
            {
                sharp = true;
                ++pos;
            }
            else if (pos < token.size() && (token[pos] == 'b'))
            {
                flat = true;
                ++pos;
            }

            // Octave: first remaining digit (default 4).
            int octave = 4;
            if (pos < token.size() && std::isdigit(static_cast<unsigned char>(token[pos])))
            {
                octave = token[pos] - '0';
                ++pos;
            }

            // Reject letters outside A-G.
            if (letter < 'A' || letter > 'G')
            {
                continue;
            }

            appendNote(outPcm, noteToFrequency(letter, sharp, flat, octave),
                       beatsForSuffix(token.substr(pos)), bpm, wave);
        }

        if (outPcm.size() >= kMaxFrames)
        {
            break;
        }
    }

    return !outPcm.empty();
}

void SoundEffect::play(const std::string& name)
{
    // Muted: nothing is queued.
    if (mMuted)
    {
        return;
    }
    // Unknown effect: nothing to play.
    if (mSounds.find(name) == mSounds.end())
    {
        return;
    }

    // Hand the name to the workers. This never blocks: if the queue is full the
    // event is simply dropped so a flood of game events can never stall the loop.
    enqueue(name);
}

void SoundEffect::enqueue(const std::string& name)
{
    if (name.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        if (mQueue.size() >= kQueueCapacity)
        {
            // Queue full: discard rather than block the game loop.
            return;
        }
        mQueue.push(name);
    }
    // Wake one waiting worker now that there is something to play.
    mQueueCond.notify_one();
}

bool SoundEffect::dequeue(std::string& out)
{
    std::unique_lock<std::mutex> lock(mQueueMutex);
    mQueueCond.wait(lock, [this]()
    {
        return !mRunning.load() || !mQueue.empty();
    });

    if (mQueue.empty())
    {
        // Shut down with nothing left to play.
        return false;
    }

    out = std::move(mQueue.front());
    mQueue.pop();
    return true;
}

void SoundEffect::workerLoop(std::size_t index)
{
    // Worker i replays exclusively on its own mBanks[i], so no locking is needed
    // for playback and the two workers run in parallel.
    std::string name;
    while (dequeue(name))
    {
        mBanks[index].play(name);
    }
}

} // namespace pimbalgame
