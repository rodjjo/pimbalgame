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

namespace pimbalgame
{
// Procedural sound effects.
//
// Each effect is described by a short string in a tiny note language, e.g.
//
//     "@180 ~square C5q E5q G5q C6h"
//
// meaning: tempo 180 BPM, square waveform, then the notes C5/E5/G5/C6 of
// quarter/half duration. The string is rendered *once* at construction time
// into PCM and cached; playing merely restarts a cached buffer on a free voice.
//
// Playback is decoupled from the game loop. play() never touches the audio
// device directly: it enqueues the *name* of the effect to play and returns
// immediately. Two worker threads pull effect names off a small bounded queue
// (see below) and replay the buffers on their own voices. Both workers share
// the single SFML audio device (the same one the background Music uses), so
// several effects -- and the music -- genuinely play in parallel.
//
// The effect database (rendered PCM) is fully built in the constructor, so
// every sound is ready to fire as soon as the game starts.
//
// Grammar (tokens separated by whitespace):
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
// Each worker owns a *VoiceBank*: its own copies of every effect's sf::SoundBuffer
// plus that sound's ring of voices (each voice permanently bound to that buffer).
// Because no buffer or voice is shared between workers, playback never races on
// sf::SoundBuffer's internal attached-sound list, and no lock guards the hot path
// -- the two workers really do play in parallel. The only shared state is the
// work queue (mutex + condition variable) and the read-only PCM database.
//
//   * A bounded queue (kQueueCapacity items) protected by a mutex and a
//     condition variable.
//   * enqueue() is non-blocking: if the queue is full the event is discarded
//     (a busy plunger can never stall the game loop).
//   * dequeue() blocks (on the condition variable) until an item is available
//     or the object is being shut down.
//   * Two worker threads loop on dequeue(); worker i replays names on mBanks[i].
class SoundEffect
{
public:
    // Constructs the effect database: renders the set of short sounds for the
    // common game events (plunger pull/release, ball collisions, ball drain)
    // into the cache, builds each worker's voice bank, then starts the worker
    // threads. Unknown/empty effect strings are simply skipped.
    SoundEffect();

    // Stops the workers and joins them. Sound playback is not copyable/movable
    // because it owns threads.
    ~SoundEffect();
    SoundEffect(const SoundEffect&) = delete;
    SoundEffect& operator=(const SoundEffect&) = delete;

    // Render (and cache) a named effect from its language string into PCM.
    // Returns false if the string was empty/invalid.
    bool addSound(const std::string& name, const std::string& language);

    // Queue an effect to play. Cheap and non-blocking: the name is pushed onto
    // the worker queue (or discarded if the queue is full) and the call returns
    // immediately; the actual playback happens on a worker thread. No-op if the
    // name is unknown or the effect is muted.
    void play(const std::string& name);

    // Global mute (e.g. if the audio device cannot be initialised). When muted,
    // play() does nothing.
    void setMuted(bool muted) { mMuted = muted; }
    [[nodiscard]] bool isMuted() const { return mMuted; }

    // Number of simultaneous voices for a single effect on a single worker.
    [[nodiscard]] static constexpr int voiceCount() { return kVoicesPerEffect; }

private:
    // One effect as owned by a single worker: that worker's own buffer plus the
    // ring of voices permanently bound to it. `next` round-robins the ring so
    // that when every voice is busy the oldest (earliest in the ring) is cut off.
    struct Effect
    {
        sf::SoundBuffer buffer;
        std::vector<std::unique_ptr<sf::Sound>> voices;  // each bound to `buffer`
        std::size_t next = 0;
    };

    // A single worker's private replay state: its own copy of every effect
    // (buffer + voice ring). Never shared between threads.
    struct VoiceBank
    {
        std::map<std::string, Effect> effects;

        // Build this worker's own buffers and voice rings from the shared,
        // read-only rendered PCM. Called on the main thread before workers start.
        void loadFromPcm(const std::map<std::string, std::vector<std::int16_t>>& pcm);

        // Restart `name` on the next voice in its ring. No-op if unknown. Because
        // this worker owns all of `effects`, this touches no shared state.
        void play(const std::string& name);
    };

    static constexpr unsigned int kSampleRate = 44100;
    static constexpr std::size_t kMaxFrames = kSampleRate * 2;  // 2 s cap per effect

    // Voices per effect (a small ring, so one effect can overlap itself), and how
    // many worker threads run. Total simultaneous effects == kWorkers * kVoicesPerEffect.
    static constexpr int kVoicesPerEffect = 4;
    static constexpr int kWorkers = 2;
    // Maximum number of pending effect names held by the queue.
    static constexpr std::size_t kQueueCapacity = 4;

    // Parse + render one effect language string into outPcm. Returns false on
    // empty/invalid input.
    bool render(const std::string& language, std::vector<std::int16_t>& outPcm);

    // Non-blocking push. If the queue is at capacity the name is discarded.
    void enqueue(const std::string& name);

    // Blocking pop: waits on the condition variable for an item or for shutdown,
    // writing the popped name to out. Returns false when shut down with nothing
    // left to do.
    bool dequeue(std::string& out);

    // Worker i's body: dequeue names until shutdown and replay each on mBanks[i].
    void workerLoop(std::size_t index);

    // The canonical rendered PCM, built once in the constructor and treated as
    // read-only afterwards (each worker copies it into its own VoiceBank).
    std::map<std::string, std::vector<std::int16_t>> mSounds;

    // One private VoiceBank per worker, fully built on the main thread before the
    // workers start. Worker i only ever touches mBanks[i].
    std::vector<VoiceBank> mBanks;

    std::queue<std::string> mQueue;                                                 // pending effect names
    std::mutex mQueueMutex;                                                         // guards mQueue
    std::condition_variable mQueueCond;                                             // wakes a worker
    std::vector<std::thread> mWorkers;                                              // playback workers
    std::atomic<bool> mRunning{false};                                              // worker run flag

    bool mMuted = false;
};
} // namespace pimbalgame
