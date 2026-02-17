#pragma once
#include "Protocol.h"
#include "VoiceCodec.h"
#include "SpatialAudio.h"
#include "ThreadSafeQueue.h"
#include <portaudio.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>

/**
 * Audio I/O engine using PortAudio.
 *
 * Handles:
 *   - Device enumeration and selection (input/output)
 *   - Microphone capture with voice activity detection (VAD)
 *   - Opus encoding of captured audio
 *   - Decoding and spatial mixing of received audio
 *   - Push-to-talk and open-mic modes
 *
 * Threading model:
 *   - PortAudio runs its own threads for capture/playback callbacks
 *   - Encoded packets are pushed to an outgoing queue (for NetworkManager)
 *   - Incoming packets are pushed by NetworkManager into the incoming queue
 *   - Playback callback mixes all incoming audio with 3D spatialization
 */
class AudioEngine {
public:
    /** Audio device info for UI display. */
    struct DeviceInfo {
        int    id;
        std::string name;
        int    maxInputChannels;
        int    maxOutputChannels;
        double defaultSampleRate;
        bool   isDefault;
    };

    /** Callback for encoded audio packets ready to send. */
    using PacketReadyCallback = std::function<void(const std::vector<uint8_t>& packet)>;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────
    bool initialize();
    void shutdown();
    bool isInitialized() const { return initialized_; }

    bool startStreams();
    void stopStreams();
    bool isStreaming() const { return streaming_; }

    // ── Device management ────────────────────────────────────────────────
    std::vector<DeviceInfo> getInputDevices() const;
    std::vector<DeviceInfo> getOutputDevices() const;

    bool setInputDevice(int deviceId);
    bool setOutputDevice(int deviceId);
    int  getInputDeviceId() const { return inputDeviceId_; }
    int  getOutputDeviceId() const { return outputDeviceId_; }

    // ── Settings ─────────────────────────────────────────────────────────
    void setPushToTalk(bool enabled) { pushToTalk_ = enabled; }
    bool isPushToTalk() const { return pushToTalk_; }

    void setPTTActive(bool active) { pttActive_ = active; }
    bool isPTTActive() const { return pttActive_; }

    void setVoiceThreshold(float threshold) { voiceThreshold_ = std::clamp(threshold, 0.0f, 1.0f); }
    float getVoiceThreshold() const { return voiceThreshold_; }

    void setHoldTimeMs(float ms) { holdTimeMs_ = std::max(0.0f, ms); }
    float getHoldTimeMs() const { return holdTimeMs_; }

    void setMicVolume(float vol) { micVolume_ = std::clamp(vol, 0.0f, 3.0f); }
    float getMicVolume() const { return micVolume_; }

    void setOutputVolume(float vol) { outputVolume_ = std::clamp(vol, 0.0f, 2.0f); }
    float getOutputVolume() const { return outputVolume_; }

    void setMicMuted(bool muted) { micMuted_ = muted; }
    bool isMicMuted() const { return micMuted_; }

    // ── Callbacks ────────────────────────────────────────────────────────
    /** Set callback for when an encoded audio packet is ready to send. */
    void setPacketReadyCallback(PacketReadyCallback cb) { packetReadyCb_ = std::move(cb); }

    // ── Remote audio input ───────────────────────────────────────────────
    /** Feed an incoming audio packet from a remote peer. Thread-safe. */
    void feedIncomingPacket(const Protocol::AudioPacket& packet);

    // ── Spatial state (updated from game thread) ─────────────────────────
    /** Update the local player's position/rotation for 3D audio. */
    void setListenerState(const Protocol::Vec3& pos, int yaw);

    /** Access spatial audio processor for settings. */
    SpatialAudio& getSpatialAudio() { return spatialAudio_; }

    // ── Status ───────────────────────────────────────────────────────────
    bool   isSpeaking() const { return isSpeaking_; }
    float  getCurrentInputLevel() const { return currentInputLevel_; }
    std::string getLastError() const { std::lock_guard<std::mutex> l(errorMutex_); return lastError_; }

    /** Set the local player position for outgoing packets. */
    void setLocalPosition(const Protocol::Vec3& pos) { localPosition_ = pos; }

private:
    // ── PortAudio callbacks ──────────────────────────────────────────────
    static int captureCallback(const void* input, void* output,
                               unsigned long frameCount,
                               const PaStreamCallbackTimeInfo* timeInfo,
                               PaStreamCallbackFlags statusFlags,
                               void* userData);

    static int playbackCallback(const void* input, void* output,
                                unsigned long frameCount,
                                const PaStreamCallbackTimeInfo* timeInfo,
                                PaStreamCallbackFlags statusFlags,
                                void* userData);

    void processCapturedAudio(const float* input, unsigned long frameCount);
    void processPlaybackAudio(float* output, unsigned long frameCount);

    // ── Voice Activity Detection ─────────────────────────────────────────
    bool detectVoiceActivity(const float* samples, int count);

    // ── Per-peer decoder state ───────────────────────────────────────────

    /** Ring buffer for jitter-free audio playback. O(1) read/write. */
    struct RingBuffer {
        std::vector<float> data;
        size_t readPos  = 0;
        size_t writePos = 0;
        size_t count    = 0;  // Samples currently buffered
        size_t capacity = 0;

        void init(size_t cap) {
            capacity = cap;
            data.resize(cap, 0.0f);
            readPos = writePos = count = 0;
        }

        size_t available() const { return count; }
        size_t freeSpace() const { return capacity - count; }

        void write(const float* src, size_t n) {
            n = std::min(n, freeSpace());
            for (size_t i = 0; i < n; i++) {
                data[writePos] = src[i];
                writePos = (writePos + 1) % capacity;
            }
            count += n;
        }

        void read(float* dst, size_t n) {
            n = std::min(n, count);
            for (size_t i = 0; i < n; i++) {
                dst[i] = data[readPos];
                readPos = (readPos + 1) % capacity;
            }
            count -= n;
        }

        void readAdditive(float* dst, size_t n) {
            n = std::min(n, count);
            for (size_t i = 0; i < n; i++) {
                dst[i] += data[readPos];
                readPos = (readPos + 1) % capacity;
            }
            count -= n;
        }

        void clear() { readPos = writePos = count = 0; }
    };

    /** Pre-buffer threshold: accumulate this many stereo frames before
     *  starting playback to absorb network jitter (60ms = 3 frames). */
    static constexpr int PREBUFFER_FRAMES = 3;
    static constexpr size_t PREBUFFER_STEREO_SAMPLES =
        PREBUFFER_FRAMES * Protocol::FRAME_SIZE * Protocol::CHANNELS_STEREO;

    struct PeerAudioState {
        VoiceCodec codec;
        std::vector<float> decodeBuffer;       // Decoded PCM (mono)
        std::vector<float> spatialBuffer;      // Spatialized PCM (stereo)
        RingBuffer jitterBuffer;               // Ring buffer for smooth playback
        SpatialAudio spatial;                  // Per-peer spatial processor
        Protocol::Vec3 lastPosition;
        std::chrono::steady_clock::time_point lastPacketTime;
        int plcFrames = 0;                     // Consecutive PLC frames
        bool active = false;
        bool prebuffering = true;              // Waiting to accumulate pre-buffer

        PeerAudioState() {
            decodeBuffer.resize(Protocol::FRAME_SIZE * 2);
            spatialBuffer.resize(Protocol::FRAME_SIZE * 2 * 2); // Stereo
            // Ring buffer holds up to 500ms of stereo audio (generous)
            jitterBuffer.init(Protocol::SAMPLE_RATE * Protocol::CHANNELS_STEREO / 2);
        }
    };

    PeerAudioState& getOrCreatePeerState(const std::string& steamId);

    // ── State ────────────────────────────────────────────────────────────
    bool initialized_ = false;
    bool streaming_   = false;

    PaStream* captureStream_  = nullptr;
    PaStream* playbackStream_ = nullptr;
    int inputDeviceId_  = -1;   // -1 = default
    int outputDeviceId_ = -1;

    // Encoding
    VoiceCodec localCodec_;
    std::vector<float> captureAccumBuffer_;    // Accumulate samples to frame size
    int captureAccumPos_ = 0;

    // Voice settings
    std::atomic<bool>  pushToTalk_{false};
    std::atomic<bool>  pttActive_{false};
    std::atomic<float> voiceThreshold_{Protocol::DEFAULT_VOICE_THRESHOLD};
    std::atomic<float> holdTimeMs_{Protocol::DEFAULT_HOLD_TIME_MS};
    std::atomic<float> micVolume_{Protocol::DEFAULT_MIC_VOLUME};
    std::atomic<float> outputVolume_{Protocol::DEFAULT_MASTER_VOLUME};
    std::atomic<bool>  micMuted_{false};
    std::atomic<bool>  isSpeaking_{false};
    std::atomic<float> currentInputLevel_{0.0f};

    // VAD hold state
    int holdFramesRemaining_ = 0;

    // Spatial state
    Protocol::Vec3 listenerPos_;
    std::atomic<int> listenerYaw_{0};
    Protocol::Vec3 localPosition_;

    // Spatial audio
    SpatialAudio spatialAudio_;

    // Per-peer audio decoders and spatial processors
    std::mutex peersMutex_;
    std::unordered_map<std::string, std::unique_ptr<PeerAudioState>> peers_;

    // Incoming packet queue (fed by network thread)
    ThreadSafeQueue<Protocol::AudioPacket> incomingPackets_{128};

    // Outgoing packet callback
    PacketReadyCallback packetReadyCb_;

    // Mix buffer (stereo, reused)
    std::vector<float> mixBuffer_;

    // Error
    mutable std::mutex errorMutex_;
    std::string lastError_;

    void setError(const std::string& err) {
        std::lock_guard<std::mutex> l(errorMutex_);
        lastError_ = err;
    }
};
