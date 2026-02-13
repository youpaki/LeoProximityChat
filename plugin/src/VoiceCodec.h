#pragma once
#include "Protocol.h"
#include <vector>
#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

/**
 * Wraps Opus encoder and decoder for voice communication.
 * Mono input, 48kHz, 20ms frames (960 samples).
 */
class VoiceCodec {
public:
    VoiceCodec();
    ~VoiceCodec();

    VoiceCodec(const VoiceCodec&) = delete;
    VoiceCodec& operator=(const VoiceCodec&) = delete;

    /** Initialize encoder and decoder. Returns false on failure. */
    bool initialize(int sampleRate = Protocol::SAMPLE_RATE,
                    int channels   = Protocol::CHANNELS_MONO,
                    int bitrate    = Protocol::OPUS_BITRATE);

    /** Shutdown and free resources. */
    void shutdown();

    /** Encode a frame of float PCM samples. Returns encoded bytes (empty on error). */
    std::vector<uint8_t> encode(const float* pcm, int frameSize);

    /** Decode an Opus packet to float PCM. Returns decoded sample count (0 on error). */
    int decode(const uint8_t* opusData, int opusLen, float* pcmOut, int maxFrameSize);

    /** Decode with packet loss concealment (no data available). */
    int decodePLC(float* pcmOut, int maxFrameSize);

    /** Set encoder bitrate. */
    void setBitrate(int bitrate);

    /** Set encoder complexity (0-10). */
    void setComplexity(int complexity);

    /** Is codec ready? */
    bool isInitialized() const { return initialized_; }

    /** Get last error message. */
    const std::string& lastError() const { return lastError_; }

private:
    OpusEncoder* encoder_ = nullptr;
    OpusDecoder* decoder_ = nullptr;
    bool initialized_ = false;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::string lastError_;
    std::vector<uint8_t> encodeBuffer_;
};
