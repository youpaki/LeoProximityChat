#include "pch.h"
#include "AudioEngine.h"
#include <cmath>
#include <algorithm>

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

AudioEngine::AudioEngine() {
    captureAccumBuffer_.resize(Protocol::FRAME_SIZE, 0.0f);
    mixBuffer_.resize(Protocol::FRAME_SIZE * 2, 0.0f); // Stereo mix buffer
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    if (initialized_) return true;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        setError(std::string("PortAudio init failed: ") + Pa_GetErrorText(err));
        return false;
    }

    // Initialize local encoder/decoder
    if (!localCodec_.initialize()) {
        setError("Failed to initialize Opus codec: " + localCodec_.lastError());
        Pa_Terminate();
        return false;
    }

    // Use default devices initially
    inputDeviceId_  = Pa_GetDefaultInputDevice();
    outputDeviceId_ = Pa_GetDefaultOutputDevice();

    initialized_ = true;
    setError("");
    return true;
}

void AudioEngine::shutdown() {
    stopStreams();

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.clear();
    }

    localCodec_.shutdown();

    if (initialized_) {
        Pa_Terminate();
        initialized_ = false;
    }
}

bool AudioEngine::startStreams() {
    if (!initialized_ || streaming_) return streaming_;

    PaError err;

    // ── Capture stream (mono input) ─────────────────────────────────────
    if (inputDeviceId_ >= 0) {
        PaStreamParameters inputParams{};
        inputParams.device = inputDeviceId_;
        inputParams.channelCount = Protocol::CHANNELS_MONO;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = Pa_GetDeviceInfo(inputDeviceId_)->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = nullptr;

        err = Pa_OpenStream(
            &captureStream_,
            &inputParams,
            nullptr,                    // No output for capture stream
            Protocol::SAMPLE_RATE,
            Protocol::FRAME_SIZE,       // Frames per buffer = one Opus frame
            paClipOff,
            captureCallback,
            this
        );

        if (err != paNoError) {
            setError(std::string("Failed to open capture stream: ") + Pa_GetErrorText(err));
            // Continue without mic — we can still receive audio
            captureStream_ = nullptr;
        }
    }

    // ── Playback stream (stereo output) ─────────────────────────────────
    if (outputDeviceId_ >= 0) {
        PaStreamParameters outputParams{};
        outputParams.device = outputDeviceId_;
        outputParams.channelCount = Protocol::CHANNELS_STEREO;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = Pa_GetDeviceInfo(outputDeviceId_)->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = nullptr;

        err = Pa_OpenStream(
            &playbackStream_,
            nullptr,                    // No input for playback stream
            &outputParams,
            Protocol::SAMPLE_RATE,
            Protocol::FRAME_SIZE,
            paClipOff,
            playbackCallback,
            this
        );

        if (err != paNoError) {
            setError(std::string("Failed to open playback stream: ") + Pa_GetErrorText(err));
            playbackStream_ = nullptr;
        }
    }

    // Start streams
    if (captureStream_) {
        err = Pa_StartStream(captureStream_);
        if (err != paNoError) {
            setError(std::string("Failed to start capture: ") + Pa_GetErrorText(err));
            Pa_CloseStream(captureStream_);
            captureStream_ = nullptr;
        }
    }

    if (playbackStream_) {
        err = Pa_StartStream(playbackStream_);
        if (err != paNoError) {
            setError(std::string("Failed to start playback: ") + Pa_GetErrorText(err));
            Pa_CloseStream(playbackStream_);
            playbackStream_ = nullptr;
        }
    }

    streaming_ = (captureStream_ != nullptr || playbackStream_ != nullptr);
    return streaming_;
}

void AudioEngine::stopStreams() {
    if (captureStream_) {
        Pa_StopStream(captureStream_);
        Pa_CloseStream(captureStream_);
        captureStream_ = nullptr;
    }

    if (playbackStream_) {
        Pa_StopStream(playbackStream_);
        Pa_CloseStream(playbackStream_);
        playbackStream_ = nullptr;
    }

    streaming_ = false;
    captureAccumPos_ = 0;
    holdFramesRemaining_ = 0;
    isSpeaking_ = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Device Management
// ═════════════════════════════════════════════════════════════════════════════

std::vector<AudioEngine::DeviceInfo> AudioEngine::getInputDevices() const {
    std::vector<DeviceInfo> devices;
    if (!initialized_) return devices;

    int defaultIn = Pa_GetDefaultInputDevice();
    int count = Pa_GetDeviceCount();

    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0) {
            devices.push_back({
                i,
                info->name ? info->name : "Unknown",
                info->maxInputChannels,
                info->maxOutputChannels,
                info->defaultSampleRate,
                (i == defaultIn)
            });
        }
    }
    return devices;
}

std::vector<AudioEngine::DeviceInfo> AudioEngine::getOutputDevices() const {
    std::vector<DeviceInfo> devices;
    if (!initialized_) return devices;

    int defaultOut = Pa_GetDefaultOutputDevice();
    int count = Pa_GetDeviceCount();

    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels > 0) {
            devices.push_back({
                i,
                info->name ? info->name : "Unknown",
                info->maxInputChannels,
                info->maxOutputChannels,
                info->defaultSampleRate,
                (i == defaultOut)
            });
        }
    }
    return devices;
}

bool AudioEngine::setInputDevice(int deviceId) {
    if (!initialized_) return false;

    const PaDeviceInfo* info = Pa_GetDeviceInfo(deviceId);
    if (!info || info->maxInputChannels <= 0) return false;

    bool wasStreaming = streaming_;
    if (wasStreaming) stopStreams();

    inputDeviceId_ = deviceId;

    if (wasStreaming) startStreams();
    return true;
}

bool AudioEngine::setOutputDevice(int deviceId) {
    if (!initialized_) return false;

    const PaDeviceInfo* info = Pa_GetDeviceInfo(deviceId);
    if (!info || info->maxOutputChannels <= 0) return false;

    bool wasStreaming = streaming_;
    if (wasStreaming) stopStreams();

    outputDeviceId_ = deviceId;

    if (wasStreaming) startStreams();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// PortAudio Callbacks
// ═════════════════════════════════════════════════════════════════════════════

int AudioEngine::captureCallback(
    const void* input, void* /*output*/,
    unsigned long frameCount,
    const PaStreamCallbackTimeInfo* /*timeInfo*/,
    PaStreamCallbackFlags /*statusFlags*/,
    void* userData)
{
    auto* engine = static_cast<AudioEngine*>(userData);
    if (input) {
        engine->processCapturedAudio(static_cast<const float*>(input), frameCount);
    }
    return paContinue;
}

int AudioEngine::playbackCallback(
    const void* /*input*/, void* output,
    unsigned long frameCount,
    const PaStreamCallbackTimeInfo* /*timeInfo*/,
    PaStreamCallbackFlags /*statusFlags*/,
    void* userData)
{
    auto* engine = static_cast<AudioEngine*>(userData);
    engine->processPlaybackAudio(static_cast<float*>(output), frameCount);
    return paContinue;
}

// ═════════════════════════════════════════════════════════════════════════════
// Audio Processing
// ═════════════════════════════════════════════════════════════════════════════

void AudioEngine::processCapturedAudio(const float* input, unsigned long frameCount) {
    if (micMuted_) {
        isSpeaking_ = false;
        currentInputLevel_ = 0.0f;
        return;
    }

    // Apply mic volume
    float micVol = micVolume_.load();

    // Accumulate samples until we have a full Opus frame
    for (unsigned long i = 0; i < frameCount; i++) {
        captureAccumBuffer_[captureAccumPos_] = input[i] * micVol;
        captureAccumPos_++;

        if (captureAccumPos_ >= Protocol::FRAME_SIZE) {
            // We have a full frame — process it
            captureAccumPos_ = 0;

            // Calculate input level
            float rms = 0.0f;
            for (int j = 0; j < Protocol::FRAME_SIZE; j++) {
                rms += captureAccumBuffer_[j] * captureAccumBuffer_[j];
            }
            rms = std::sqrt(rms / Protocol::FRAME_SIZE);
            currentInputLevel_ = rms;

            // Check if we should transmit
            bool shouldTransmit = false;
            if (pushToTalk_) {
                shouldTransmit = pttActive_;
            } else {
                shouldTransmit = detectVoiceActivity(captureAccumBuffer_.data(), Protocol::FRAME_SIZE);
            }

            isSpeaking_ = shouldTransmit;

            if (shouldTransmit && packetReadyCb_) {
                // Encode with Opus
                auto encoded = localCodec_.encode(captureAccumBuffer_.data(), Protocol::FRAME_SIZE);
                if (!encoded.empty()) {
                    // Build network packet with local position
                    Protocol::Vec3 pos = localPosition_;
                    auto packet = Protocol::buildOutgoingAudioPacket(pos, encoded.data(), encoded.size());
                    packetReadyCb_(packet);
                }
            }
        }
    }
}

void AudioEngine::processPlaybackAudio(float* output, unsigned long frameCount) {
    // Clear output buffer (stereo)
    std::memset(output, 0, frameCount * Protocol::CHANNELS_STEREO * sizeof(float));

    // Process all pending incoming packets
    while (auto pktOpt = incomingPackets_.tryPop()) {
        const auto& pkt = *pktOpt;
        auto& peer = getOrCreatePeerState(pkt.senderSteamId);

        // Decode
        if (!pkt.opusData.empty()) {
            int decoded = peer.codec.decode(
                pkt.opusData.data(), static_cast<int>(pkt.opusData.size()),
                peer.decodeBuffer.data(), Protocol::FRAME_SIZE
            );

            if (decoded > 0) {
                peer.lastPosition = pkt.senderPosition;
                peer.lastPacketTime = std::chrono::steady_clock::now();
                peer.plcFrames = 0;
                peer.active = true;

                // Spatialize into stereo
                Protocol::Vec3 lPos = listenerPos_;
                int lYaw = listenerYaw_.load();

                peer.spatial.setDistanceParams(
                    spatialAudio_.isEnabled() ? Protocol::DEFAULT_FULL_VOL_DISTANCE : 0.0f,
                    spatialAudio_.isEnabled() ? Protocol::DEFAULT_MAX_DISTANCE : 100000.0f,
                    Protocol::DEFAULT_ROLLOFF_FACTOR
                );
                peer.spatial.setEnabled(spatialAudio_.isEnabled());
                peer.spatial.setMasterVolume(outputVolume_.load());

                peer.spatial.process(
                    peer.decodeBuffer.data(), decoded, peer.spatialBuffer.data(),
                    lPos, lYaw, peer.lastPosition
                );

                // Insert into jitter buffer
                size_t stereoSamples = static_cast<size_t>(decoded) * 2;
                peer.jitterBuffer.insert(
                    peer.jitterBuffer.end(),
                    peer.spatialBuffer.begin(),
                    peer.spatialBuffer.begin() + stereoSamples
                );
            }
        }
    }

    // Mix all peers' jitter buffers into output
    size_t stereoFrameCount = frameCount * 2;
    std::lock_guard<std::mutex> lock(peersMutex_);

    for (auto& [steamId, peer] : peers_) {
        if (!peer->active) continue;

        size_t available = peer->jitterBuffer.size();
        size_t toRead = std::min(available, stereoFrameCount);

        if (toRead > 0) {
            // Mix into output
            for (size_t i = 0; i < toRead; i++) {
                output[i] += peer->jitterBuffer[i];
            }
            // Remove consumed samples
            peer->jitterBuffer.erase(
                peer->jitterBuffer.begin(),
                peer->jitterBuffer.begin() + toRead
            );
        } else {
            // No data available — apply packet loss concealment
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - peer->lastPacketTime
            ).count();

            if (elapsed < 500 && peer->plcFrames < 10) {
                // Try PLC
                int plcSamples = peer->codec.decodePLC(
                    peer->decodeBuffer.data(), Protocol::FRAME_SIZE
                );
                if (plcSamples > 0) {
                    peer->plcFrames++;
                    Protocol::Vec3 lPos = listenerPos_;
                    int lYaw = listenerYaw_.load();

                    peer->spatial.process(
                        peer->decodeBuffer.data(), plcSamples, peer->spatialBuffer.data(),
                        lPos, lYaw, peer->lastPosition
                    );

                    size_t plcStereo = static_cast<size_t>(plcSamples) * 2;
                    size_t plcToMix = std::min(plcStereo, stereoFrameCount);
                    for (size_t i = 0; i < plcToMix; i++) {
                        output[i] += peer->spatialBuffer[i];
                    }
                }
            } else if (elapsed > 2000) {
                // Peer hasn't sent audio in 2 seconds — mark inactive
                peer->active = false;
            }
        }
    }

    // Soft clamp output to prevent clipping
    for (size_t i = 0; i < stereoFrameCount; i++) {
        output[i] = std::tanh(output[i]); // Soft saturation
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Voice Activity Detection
// ═════════════════════════════════════════════════════════════════════════════

bool AudioEngine::detectVoiceActivity(const float* samples, int count) {
    float threshold = voiceThreshold_.load();

    // Calculate RMS energy
    float rms = 0.0f;
    for (int i = 0; i < count; i++) {
        rms += samples[i] * samples[i];
    }
    rms = std::sqrt(rms / count);

    bool voiceDetected = (rms > threshold);

    // Hysteresis: hold transmission for a while after voice stops
    if (voiceDetected) {
        float holdMs = holdTimeMs_.load();
        // Convert hold time to number of frames
        holdFramesRemaining_ = static_cast<int>(
            holdMs / Protocol::FRAME_DURATION_MS
        );
        return true;
    } else if (holdFramesRemaining_ > 0) {
        holdFramesRemaining_--;
        return true; // Still in hold period
    }

    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Remote Peer Management
// ═════════════════════════════════════════════════════════════════════════════

void AudioEngine::feedIncomingPacket(const Protocol::AudioPacket& packet) {
    incomingPackets_.push(packet);
}

AudioEngine::PeerAudioState& AudioEngine::getOrCreatePeerState(const std::string& steamId) {
    std::lock_guard<std::mutex> lock(peersMutex_);

    auto it = peers_.find(steamId);
    if (it != peers_.end()) {
        return *it->second;
    }

    auto state = std::make_unique<PeerAudioState>();
    state->codec.initialize();
    state->lastPacketTime = std::chrono::steady_clock::now();

    auto& ref = *state;
    peers_[steamId] = std::move(state);
    return ref;
}

void AudioEngine::setListenerState(const Protocol::Vec3& pos, int yaw) {
    listenerPos_ = pos;
    listenerYaw_ = yaw;
}
