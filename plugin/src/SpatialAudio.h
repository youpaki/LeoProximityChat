#pragma once
#include "Protocol.h"
#include <vector>

/**
 * 3D spatial audio processor.
 * Takes mono input from a remote peer and produces stereo output
 * with distance attenuation and stereo panning based on relative position.
 *
 * Features:
 *   - Equal-power stereo panning based on angle to source
 *   - Distance-based volume attenuation with inner/outer radius
 *   - Smooth rolloff curve (smoothstep)
 *   - Optional distance-based low-pass filter (muffling)
 *   - Per-source processing (each peer gets independent spatialization)
 */
class SpatialAudio {
public:
    SpatialAudio();
    ~SpatialAudio() = default;

    /** Configure distance parameters. */
    void setDistanceParams(float innerRadius, float outerRadius, float rolloff = 1.0f);

    /** Enable/disable 3D processing (bypasses to center-panned mono if disabled). */
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    /** Set master volume multiplier. */
    void setMasterVolume(float vol) { masterVolume_ = std::clamp(vol, 0.0f, 2.0f); }

    /** Enable distance-based low-pass filter. */
    void setLowPassEnabled(bool enabled) { lowPassEnabled_ = enabled; }

    /**
     * Process mono input into stereo output with 3D spatialization.
     *
     * @param monoIn       Mono input samples from remote peer
     * @param frameSize    Number of mono samples
     * @param stereoOut    Output buffer (must hold frameSize * 2 floats)
     * @param listenerPos  Local player position
     * @param listenerYaw  Local player yaw (Unreal rotation units, 0-65535)
     * @param sourcePos    Remote player position
     * @return Volume multiplier applied (0.0 = silent due to distance)
     */
    float process(const float* monoIn, int frameSize, float* stereoOut,
                  const Protocol::Vec3& listenerPos, int listenerYaw,
                  const Protocol::Vec3& sourcePos);

    /**
     * Mix a spatialized stereo buffer into an accumulation buffer.
     * Adds (does not replace) into mixBuffer.
     */
    static void mixInto(float* mixBuffer, const float* source, int stereoSamples);

private:
    /** Smoothstep interpolation (cubic Hermite). */
    static float smoothstep(float edge0, float edge1, float x);

    /** Convert Unreal rotation yaw to radians. */
    static float yawToRadians(int yaw);

    /** Simple one-pole low-pass filter state per channel. */
    struct LowPassState {
        float prevL = 0.0f;
        float prevR = 0.0f;
    };

    /** Apply distance-based low-pass filter. */
    void applyLowPass(float* stereoOut, int frameSize, float distanceFactor, LowPassState& state);

    bool  enabled_        = true;
    float innerRadius_    = Protocol::DEFAULT_FULL_VOL_DISTANCE;
    float outerRadius_    = Protocol::DEFAULT_MAX_DISTANCE;
    float rolloff_        = 1.0f;
    float masterVolume_   = 1.0f;
    bool  lowPassEnabled_ = true;

    // Per-source filter state (cleared when source changes, but we keep one for simplicity)
    LowPassState lpState_;
};
