#include "pinballgame/SoundEffect.hpp"

#include <cmath>
#include <random>
#include <sstream>

namespace pinballgame
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // File-local mirrors of the class constants so the free helpers below can reach
    // them (they can't see the private static members).
    constexpr unsigned int kSampleRate = 44100;
    constexpr std::size_t kMaxFrames = 44100 * 2;  // 2 s cap per effect
    constexpr float kMaxImpactSpeed = 1500.0f;     // ball speed that reads as full power

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

    // ------------------------------------------------------------------------
    // Metallic impact synthesis.
    //
    // A struck metal object (steel ball off a bumper, flipper, wall or coin) is
    // not a single pitched tone: it is a burst of inharmonic resonant modes plus
    // the broadband "crack" of the initial contact. Each mode is a sine at an
    // inharmonic frequency (like a plate/bell, whose modes scale with sqrt of
    // mode number) that decays exponentially; higher modes decay faster, which is
    // what makes a hit ring down from bright to dull. A short filtered-noise
    // transient added at the very start supplies the percussive attack.
    //
    // `impactSpeed` (0..kMaxImpactSpeed, clamped) drives everything: harder hits
    // are louder, brighter (more partials + higher pitch) and ring longer -- the
    // behaviour of a real pinball.
    // ------------------------------------------------------------------------
    void renderMetallic(const SoundEffect::MetallicConfig& config, float impactSpeed,
                        std::vector<std::int16_t>& out)
    {
        // Normalised impact energy: 0 = a whisper, 1 = full-speed impact.
        const float e = std::min(1.0f, std::max(0.0f, impactSpeed / kMaxImpactSpeed));

        // Parameters derived from the impact energy.
        const float baseFreq   = config.baseFreq * (1.0f + config.freqSweep * e);
        const float brightness = 1.0f + config.brightness * e;
        const float decay      = config.decayMin + (config.decayMax - config.decayMin) * e;  // ring time
        const float level      = config.minLevel + (config.maxLevel - config.minLevel) * e;  // peak gain
        const int   parts      = std::max(1, static_cast<int>(std::lround(
                                    config.ringParts * (0.6f + 0.6f * e))));          // more modes when harder
        const float noiseAmt   = config.noiseGain * (0.4f + 0.6f * e);

        // Length the render a little beyond the decay so the ring tails naturally.
        const int frames = static_cast<int>(
            std::min<double>(kMaxFrames, static_cast<double>(decay * 1.35f * kSampleRate)));
        if (frames <= 0)
        {
            out.clear();
            return;
        }
        out.resize(frames);

        // A single short noise burst shapes the initial "crack"; a one-pole low
        // pass keeps it from hissing like raw white noise, and an exponential
        // envelope makes it fade within a few milliseconds.
        const int noiseFrames = std::max(1, static_cast<int>(kSampleRate * 0.010));  // ~10 ms
        std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
        float noiseState = 0.0f;  // one-pole low-pass state for the noise
        const float noiseAlpha = 0.35f;

        for (int i = 0; i < frames; ++i)
        {
            const double t = i / static_cast<double>(kSampleRate);

            // Overall amplitude envelope: a sub-millisecond attack, then an
            // exponential decay whose time constant is the impact ring length.
            const double attack = 0.0008;
            const double env = (t < attack) ? (t / attack) : std::exp(-3.0 * (t - attack) / decay);

            // The inharmonic partial bank. Higher modes are quieter and decay
            // faster, which is the hallmark of a metallic timbre.
            double mix = 0.0;
            for (int k = 1; k <= parts; ++k)
            {
                const double ratio = std::sqrt(static_cast<double>(k));
                const double freq = baseFreq * ratio * brightness;
                if (freq > kMaxFreq)
                {
                    continue;  // skip modes pushed above the audible range
                }
                // Higher modes die out faster: their decay runs from ~decay down to
                // ~0.45*decay as k increases (guarded against a degenerate 1-mode bank).
                const double faster = (parts > 1) ? (k - 1) / (double)(parts - 1) : 0.0;
                const double tau = decay * (1.0 - 0.55 * faster);
                const double amp = (1.0 / std::sqrt(ratio)) * env * std::exp(-t / tau);
                mix += amp * std::sin(2.0 * kPi * freq * t);
            }

            // Percussive noise crack, present only at the very start. The one-pole
            // low pass smooths the white noise into a warmer "tock".
            double crack = 0.0;
            if (i < noiseFrames)
            {
                const float n = uni(rng);
                noiseState += noiseAlpha * (n - noiseState);  // low-pass the sample
                crack = noiseAmt * noiseState * std::exp(-12.0 * t);
            }

            // Combine, apply the impact-level gain and a safety headroom factor so
            // stacked partials can never clip, then saturate-safe quantise.
            const double sample = (mix + crack) * level * 0.85 * 32767.0;
            const double clamped = std::min(1.0, std::max(-1.0, sample / 32767.0));
            out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(clamped * 32767.0));
        }
    }
}

// ---------------------------------------------------------------------------
// VoiceBank: a single worker's private replay state.
// ---------------------------------------------------------------------------

void SoundEffect::VoiceBank::load(const std::map<std::string, std::vector<std::int16_t>>& tonal,
                                  const std::map<std::string, SoundEffect::MetallicConfig>& metallic)
{
    // Tonal effects: one immutable PCM, loaded verbatim into every voice so any
    // voice can restart the cached sound without ever reloading the buffer.
    for (const auto& [name, samples] : tonal)
    {
        Effect& effect = effects[name];
        effect.metallic = false;
        effect.voices.reserve(kVoicesPerEffect);
        for (int i = 0; i < kVoicesPerEffect; ++i)
        {
            Voice& v = effect.voices.emplace_back();
            v.scratch.assign(samples.begin(), samples.end());
            if (!v.buffer.loadFromSamples(v.scratch.data(),
                                          static_cast<unsigned int>(v.scratch.size()), 1, kSampleRate,
                                          {sf::SoundChannel::Mono}))
            {
                effects.erase(name);
                break;
            }
            v.sound = std::make_unique<sf::Sound>(v.buffer);
        }
    }

    // Metallic effects: store the timbre config and create the voice ring with a
    // (near-)empty placeholder buffer; the real PCM is rendered fresh on each hit.
    for (const auto& [name, config] : metallic)
    {
        Effect& effect = effects[name];
        effect.metallic = true;
        effect.config = config;
        effect.voices.reserve(kVoicesPerEffect);
        for (int i = 0; i < kVoicesPerEffect; ++i)
        {
            Voice& v = effect.voices.emplace_back();
            // Valid placeholder so the sound has a buffer to bind to; it is
            // reloaded with real samples on the first hit.
            const std::int16_t silence = 0;
            if (!v.buffer.loadFromSamples(&silence, 1, 1, kSampleRate, {sf::SoundChannel::Mono}))
            {
                break;  // placeholder failed: skip this effect's remaining voices
            }
            v.sound = std::make_unique<sf::Sound>(v.buffer);
            v.scratch.reserve(static_cast<std::size_t>(kSampleRate));  // ~1 s of headroom
        }
    }
}

void SoundEffect::VoiceBank::play(const std::string& name, float speed)
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
    Voice& voice = effect.voices[effect.next];
    effect.next = (effect.next + 1) % effect.voices.size();

    if (effect.metallic)
    {
        // Render this specific strike and load it into this voice's buffer. The
        // render is short (a few tens of ms) and pure math, so doing it on the
        // worker thread keeps the game loop unblocked.
        renderMetallic(effect.config, speed, voice.scratch);
        if (!voice.buffer.loadFromSamples(voice.scratch.data(),
                                          static_cast<unsigned int>(voice.scratch.size()), 1, kSampleRate,
                                          {sf::SoundChannel::Mono}))
        {
            return;
        }
    }
    // Tonal effects already carry their PCM in `scratch`, so the buffer is loaded
    // and a plain play() restarts the cached sound. Metallic voices just played
    // the buffer reloaded above.
    if (voice.sound)
    {
        voice.sound->play();
    }
}

void SoundEffect::VoiceBank::applyVolume(float volume)
{
    for (auto& entry : effects)
    {
        for (auto& voice : entry.second.voices)
        {
            if (voice.sound)
            {
                voice.sound->setVolume(volume);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SoundEffect
// ---------------------------------------------------------------------------

SoundEffect::SoundEffect()
{
    // Tonal effects: short melodic blips for the events that should stay musical
    // (plunger pull/release, ball drain).
    addSound("plunger_down",     "~saw C3e E3e G3e");        // pulling the plunger: rising charge
    addSound("plunger_up",       "~square E4e G4e C5q");     // releasing: launch arpeggio
    addSound("ball_drain",       "~sine E4s C3h");           // losing the ball: falling tone

    // Metallic impact effects: the ball hitting a surface. Each config tunes a
    // distinct metal character; impact speed (supplied at play time) modulates
    // pitch/brightness/decay/level for realism.
    addMetallic("ball_hit_bumper", {1100.f, 0.30f, 1.00f, 0.08f, 0.30f, 0.35f, 0.90f, 0.22f, 9.0f});  // ringing steel
    addMetallic("ball_hit_wall",   {520.f,  0.15f, 0.60f, 0.04f, 0.12f, 0.30f, 0.60f, 0.18f, 6.0f});  // duller wall tap
    addMetallic("ball_hit_flipper",{650.f,  0.20f, 0.80f, 0.05f, 0.18f, 0.35f, 0.75f, 0.20f, 7.0f});  // punchy flipper
    addMetallic("ball_hit_coin",   {2400.f, 0.40f, 1.40f, 0.03f, 0.10f, 0.30f, 0.70f, 0.28f, 8.0f});  // bright coin shimmer

    // Give each worker its own private voice bank, built from the (now read-only)
    // effect definitions on the main thread, before any worker starts.
    mBanks.resize(kWorkers);
    for (auto& bank : mBanks)
    {
        bank.load(mTonal, mMetallic);
    }

    // Start the playback workers. Each pulls events off the shared queue and
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
    mTonal[name] = std::move(pcm);
    return true;
}

void SoundEffect::addMetallic(const std::string& name, const MetallicConfig& config)
{
    // Metallic effects are not pre-rendered; register the timbre so the workers
    // can render it on demand. Duplicates just overwrite the config.
    mMetallic[name] = config;
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
    // Tonal effect only -- impact effects require the speed overload.
    if (mTonal.find(name) == mTonal.end())
    {
        return;
    }

    enqueue({name, 0.0f});
}

void SoundEffect::play(const std::string& name, float impactSpeed)
{
    // Muted: nothing is queued.
    if (mMuted)
    {
        return;
    }
    // Impact effect only.
    if (mMetallic.find(name) == mMetallic.end())
    {
        return;
    }

    enqueue({name, impactSpeed});
}

void SoundEffect::enqueue(const QueuedEvent& event)
{
    if (event.name.empty())
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
        mQueue.push(event);
    }
    // Wake one waiting worker now that there is something to play.
    mQueueCond.notify_one();
}

bool SoundEffect::dequeue(QueuedEvent& out)
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
    QueuedEvent event;
    while (dequeue(event))
    {
        mBanks[index].play(event.name, event.speed);
    }
}

void SoundEffect::setSfxVolume(float volume)
{
    // Mirror the requested volume onto every voice of every cached effect on
    // both worker banks. The banks are already fully built, so this is a plain
    // loop with no allocation or shared-state access.
    for (auto& bank : mBanks)
    {
        bank.applyVolume(volume);
    }
}

} // namespace pinballgame
