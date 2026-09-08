#pragma once

#include <SFML/Audio.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace pinballgame
{
// Procedural sound effects.
//
// Two kinds of effect live side by side:
//
//   * Tonal effects (plunger pull/release, ball drain) are short melodic
//     snippets described by a tiny note language (see render() below) and
//     rendered *once* into PCM at construction time. Playing merely restarts a
//     cached buffer on a free voice -- unchanged from the classic model.
//
//   * Metallic impact effects (the ball hitting a bumper, wall, flipper or coin)
//     are rendered *on the fly* at play time from a MetallicConfig plus the
//     measured impact speed. A strike is modelled as a short broadband noise
//     "crack" summed with several inharmonic, exponentially-decaying partials
//     (the way a struck steel plate or bell sounds). The impact speed -- the
//     ball's speed at the instant of contact -- scales the pitch, brightness,
//     number of partials, ring length and overall level, so a light tap sounds
//     dull and short while a hard hit rings bright and long. This replaces the
//     previous musical-note synthesis for the contact sounds, which is why they
//     now read as metal rather than as chimes.
//
// Playback is decoupled from the game loop. play() never touches the audio
// device directly: it enqueues the *name* (and, for impacts, the impact speed)
// of the effect to play and returns immediately. Two worker threads pull events
// off a small bounded queue (see below) and replay them on their own voices.
// Both workers share the single SFML audio device (the same one the background
// Music uses), so several effects -- and the music -- genuinely play in parallel.
//
// Grammar (tokens separated by whitespace) for the tonal note language:
//     @<bpm>          set tempo (beats per minute for a quarter note). default 120
//     ~<waveform>     set waveform: sine | square | saw|sawtooth | triangle. default triangle
//     <note><dur>     a pitch: A-G, optional '#' or 'b', an octave digit, then an
//                     optional duration suffix. default quarter.
//     R<dur>          a rest of the given duration.
//
// Duration suffixes (in beats): w = whole (4), h = half (2), q = quarter (1),
// e = eighth (1/2), s = sixteenth (1/4), t = triplet quarter (1/3).
//
// Concurrency model
// -----------------
// Each worker owns a *VoiceBank*: its own copies of every effect plus that
// sound's ring of voices. Because no buffer or voice is shared between workers,
// playback never races on sf::SoundBuffer's internal attached-sound list, and no
// lock guards the hot path -- the two workers really do play in parallel. The
// only shared state is the work queue (mutex + condition variable) and the
// read-only effect definitions (tonal PCM and metallic configs).
//
//   * A bounded queue (kQueueCapacity items) protected by a mutex and a
//     condition variable.
//   * enqueue() is non-blocking: if the queue is full the event is discarded
//     (a busy plunger can never stall the game loop).
//   * dequeue() blocks (on the condition variable) until an item is available
//     or the object is being shut down.
//   * Two worker threads loop on dequeue(); worker i replays events on
//     mBanks[i].
//
// For metallic effects each play renders fresh PCM on the worker thread and
// reloads the next voice's buffer with it, so the ring is deliberately larger
// than the decay time: a stolen voice is restarting with brand-new samples,
// which simply cuts the (short) previous strike -- exactly the polyphony we
// want, with no click because the new buffer is rendered with its own envelope.
class SoundEffect
{
public:
    // A metallic impact timbre: the inharmonic partial bank and noise transient
    // that give one named surface (bumper / wall / flipper / coin) its character.
    // Impact speed is supplied at play time and modulates these values. Tunable
    // starting points; the magnitudes are not physically calibrated, just tuned
    // to read as distinct real-metal strikes.
    struct MetallicConfig
    {
        float baseFreq   = 1100.f;  // Hz, fundamental of the partial bank
        float freqSweep  = 0.30f;   // fractional pitch rise with impact speed
        float brightness = 1.00f;   // brightens the partial bank with speed
        float decayMin   = 0.08f;   // s, ring length at a soft impact
        float decayMax   = 0.30f;   // s, ring length at a hard impact
        float minLevel   = 0.35f;   // peak amplitude at a soft impact (0..1)
        float maxLevel   = 0.90f;   // peak amplitude at a hard impact (0..1)
        float noiseGain  = 0.22f;   // level of the noise "crack"
        float ringParts  = 9.0f;    // number of inharmonic partials
    };

    // Constructs the effect database up front so every sound is ready to fire as
    // soon as the game starts. Tonal effects (plunger/drain) are rendered from
    // the note language; the metallic impact effects are registered with a
    // MetallicConfig and rendered on demand. Unknown/empty effect strings are
    // simply skipped.
    SoundEffect();

    // Stops the workers and joins them. Sound playback is not copyable/movable
    // because it owns threads.
    ~SoundEffect();
    SoundEffect(const SoundEffect&) = delete;
    SoundEffect& operator=(const SoundEffect&) = delete;

    // Render (and cache) a named tonal effect from its note-language string into
    // PCM. Returns false if the string was empty/invalid. Tonal effects only.
    bool addSound(const std::string& name, const std::string& language);

    // Register a named metallic impact effect with its timbre configuration.
    // The PCM is not rendered yet -- it is generated at play time from the
    // impact speed. Impact effects only.
    void addMetallic(const std::string& name, const MetallicConfig& config);

    // Queue a tonal effect to play. Cheap and non-blocking; the name is pushed
    // (or discarded if the queue is full) and the call returns immediately;
    // playback happens on a worker thread. No-op if unknown or muted.
    void play(const std::string& name);

    // Queue a metallic impact effect, with the ball's impact speed (pixels per
    // second) at the moment of contact. Cheap and non-blocking; the rest matches
    // play(name). The speed scales pitch/brightness/decay/level. No-op if
    // unknown or muted.
    void play(const std::string& name, float impactSpeed);

    // Global mute (e.g. if the audio device cannot be initialised). When muted,
    // play() does nothing.
    void setMuted(bool muted) { mMuted = muted; }
    [[nodiscard]] bool isMuted() const { return mMuted; }

    // Master volume (0..100+, SFML convention) applied to every effect voice on
    // both workers. Safe to call at any time; used by the menu's SFX/General
    // sliders. Cheap enough to call every frame.
    void setSfxVolume(float volume);

    // Number of simultaneous voices for a single effect on a single worker.
    [[nodiscard]] static constexpr int voiceCount() { return kVoicesPerEffect; }

private:
    // One event pushed onto the queue: an effect name plus, for impacts, the
    // measured impact speed. Tonal events carry speed == 0, which is ignored.
    struct QueuedEvent
    {
        std::string name;
        float speed = 0.0f;
    };

    // A single play-ready voice: its own buffer (loaded either once for a tonal
    // effect, or freshly on every hit for a metallic one) permanently bound to
    // its sound. `scratch` holds the PCM to load and is reused play to play so a
    // hit performs no heap growth past the first sizing.
    struct Voice
    {
        sf::SoundBuffer buffer;
        std::unique_ptr<sf::Sound> sound;  // bound to `buffer`
        std::vector<std::int16_t> scratch;  // reuse-safe render target
    };

    // One effect as owned by a single worker: its ring of voices plus, for
    // metallic effects, the timbre config (rendered on demand from impact speed).
    // `metallic` selects which dispatch path the worker uses at play time.
    struct Effect
    {
        bool metallic = false;
        MetallicConfig config;
        std::vector<Voice> voices;
        std::size_t next = 0;
    };

    // A single worker's private replay state: its own copy of every effect
    // (buffers + voice rings). Never shared between threads.
    struct VoiceBank
    {
        std::map<std::string, Effect> effects;

        // Build this worker's own buffers and voice rings from the shared,
        // read-only tonal PCM and metallic configs. Called on the main thread
        // before workers start.
        void load(const std::map<std::string, std::vector<std::int16_t>>& tonal,
                  const std::map<std::string, MetallicConfig>& metallic);

        // Restart `name` on the next voice in its ring. Tonal effects replay a
        // cached buffer; metallic effects render fresh PCM from `speed` first.
        // No-op if unknown. Because this worker owns all of `effects`, this
        // touches no shared state.
        void play(const std::string& name, float speed);

        // Apply `volume` (0..100+) to every voice of every cached effect. Since
        // this worker owns all of `effects`, it touches no shared state.
        void applyVolume(float volume);
    };

    static constexpr unsigned int kSampleRate = 44100;
    static constexpr std::size_t kMaxFrames = kSampleRate * 2;  // 2 s cap per effect

    // Ball speed at contact that reads as a "full power" hit. The game clamps the
    // ball's speed to Ball::maxSpeed, so this is a natural normalisation.
    static constexpr float kMaxImpactSpeed = 1500.0f;

    // Voices per effect (a larger ring than the decay time, so a stolen metallic
    // voice has already finished and can be reloaded cleanly), how many worker
    // threads run, and the maximum pending events. Total simultaneous effects
    // == kWorkers * kVoicesPerEffect.
    static constexpr int kVoicesPerEffect = 8;
    static constexpr int kWorkers = 2;
    static constexpr std::size_t kQueueCapacity = 4;

    // Parse + render one tonal note-language string into outPcm. Returns false on
    // empty/invalid input.
    bool render(const std::string& language, std::vector<std::int16_t>& outPcm);

    // Non-blocking push. If the queue is at capacity the event is discarded.
    void enqueue(const QueuedEvent& event);

    // Blocking pop: waits on the condition variable for an item or for shutdown,
    // writing the popped event to out. Returns false when shut down with nothing
    // left to do.
    bool dequeue(QueuedEvent& out);

    // Worker i's body: dequeue events until shutdown and replay each on mBanks[i].
    void workerLoop(std::size_t index);

    // The canonical rendered tonal PCM and the metallic timbre configs, built
    // once in the constructor and treated as read-only afterwards (each worker
    // copies the definitions into its own VoiceBank).
    std::map<std::string, std::vector<std::int16_t>> mTonal;
    std::map<std::string, MetallicConfig> mMetallic;

    // One private VoiceBank per worker, fully built on the main thread before the
    // workers start. Worker i only ever touches mBanks[i].
    std::vector<VoiceBank> mBanks;

    std::queue<QueuedEvent> mQueue;                                                 // pending events
    std::mutex mQueueMutex;                                                         // guards mQueue
    std::condition_variable mQueueCond;                                             // wakes a worker
    std::vector<std::thread> mWorkers;                                              // playback workers
    std::atomic<bool> mRunning{false};                                              // worker run flag

    bool mMuted = false;
};
} // namespace pinballgame
