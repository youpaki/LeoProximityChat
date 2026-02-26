#include "pch.h"
#include "VoiceCodec.h"
#include <opus/opus.h>

VoiceCodec::VoiceCodec() {
    encodeBuffer_.resize(Protocol::MAX_OPUS_FRAME_BYTES);
}

VoiceCodec::~VoiceCodec() {
    shutdown();
}

bool VoiceCodec::initialize(int sampleRate, int channels, int bitrate) {
    shutdown();

    sampleRate_ = sampleRate;
    channels_   = channels;
    int err = 0;

    // ── Encoder ──────────────────────────────────────────────────────────
    encoder_ = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !encoder_) {
        lastError_ = std::string("Opus encoder create failed: ") + opus_strerror(err);
        return false;
    }

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(Protocol::OPUS_COMPLEXITY));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));          // Forward error correction
    opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(5));    // Expect ~5% packet loss
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));                 // Discontinuous transmission

    // ── Decoder ──────────────────────────────────────────────────────────
    decoder_ = opus_decoder_create(sampleRate, channels, &err);
    if (err != OPUS_OK || !decoder_) {
        lastError_ = std::string("Opus decoder create failed: ") + opus_strerror(err);
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }

    initialized_ = true;
    lastError_.clear();
    return true;
}

void VoiceCodec::shutdown() {
    if (encoder_) { opus_encoder_destroy(encoder_); encoder_ = nullptr; }
    if (decoder_) { opus_decoder_destroy(decoder_); decoder_ = nullptr; }
    initialized_ = false;
}

std::vector<uint8_t> VoiceCodec::encode(const float* pcm, int frameSize) {
    if (!initialized_ || !encoder_) return {};

    int encoded = opus_encode_float(
        encoder_, pcm, frameSize,
        encodeBuffer_.data(), static_cast<opus_int32>(encodeBuffer_.size())
    );

    if (encoded < 0) {
        lastError_ = std::string("Opus encode error: ") + opus_strerror(encoded);
        return {};
    }

    // DTX: Opus may return 1 or 2 bytes for silence, skip sending
    if (encoded <= 2) return {};

    return std::vector<uint8_t>(encodeBuffer_.begin(), encodeBuffer_.begin() + encoded);
}

int VoiceCodec::decode(const uint8_t* opusData, int opusLen, float* pcmOut, int maxFrameSize) {
    if (!initialized_ || !decoder_) return 0;

    int decoded = opus_decode_float(
        decoder_, opusData, opusLen,
        pcmOut, maxFrameSize, 0 /* no FEC for normal decode */
    );

    if (decoded < 0) {
        lastError_ = std::string("Opus decode error: ") + opus_strerror(decoded);
        return 0;
    }

    return decoded;
}

int VoiceCodec::decodePLC(float* pcmOut, int maxFrameSize) {
    if (!initialized_ || !decoder_) return 0;

    // Pass nullptr for packet loss concealment
    int decoded = opus_decode_float(
        decoder_, nullptr, 0,
        pcmOut, maxFrameSize, 0
    );

    if (decoded < 0) return 0;
    return decoded;
}

void VoiceCodec::setBitrate(int bitrate) {
    if (encoder_) {
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    }
}

void VoiceCodec::setComplexity(int complexity) {
    if (encoder_) {
        complexity = std::clamp(complexity, 0, 10);
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity));
    }
}
