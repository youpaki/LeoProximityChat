#pragma once
#include "Protocol.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>

/**
 * Realistic 3D Spatial Audio Processor
 *
 * Full binaural 3D simulation with:
 *   - HRTF-based binaural rendering (head-related transfer function)
 *     · Frequency-dependent ILD (interaural level difference)
 *     · ITD simulation (interaural time delay up to ~0.7ms)
 *     · Pinna/head shadow frequency shaping per ear
 *   - Distance-based processing:
 *     · Inverse-square / smoothstep rolloff
 *     · Air absorption (high-frequency attenuation over distance)
 *     · Distance-dependent reverb (wet/dry mix)
 *   - Environment simulation:
 *     · Schroeder reverb (4 comb filters + 2 allpass filters)
 *     · Early reflections (6 taps simulating stadium walls/floor/ceiling)
 *     · Late diffuse reverb tail
 *   - Smooth parameter interpolation to avoid clicks/pops
 *   - Per-source independent processing state
 */
class SpatialAudio {
public:
    SpatialAudio();
    ~SpatialAudio() = default;

    void setDistanceParams(float innerRadius, float outerRadius, float rolloff = 1.0f);
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    void setMasterVolume(float vol) { masterVolume_ = std::clamp(vol, 0.0f, 2.0f); }
    void setReverbMix(float mix) { reverbMix_ = std::clamp(mix, 0.0f, 1.0f); }
    void setReverbEnabled(bool e) { reverbEnabled_ = e; }

    float getInnerRadius() const { return innerRadius_; }
    float getOuterRadius() const { return outerRadius_; }
    float getRolloff() const { return rolloff_; }
    float getReverbMix() const { return reverbMix_; }

    /**
     * Process mono input into stereo output with full 3D spatialization.
     * Returns volume multiplier applied (0 = silent / out of range).
     */
    float process(const float* monoIn, int frameSize, float* stereoOut,
                  const Protocol::Vec3& listenerPos, int listenerYaw,
                  const Protocol::Vec3& sourcePos);

    /** Additive mix source into mixBuffer. */
    static void mixInto(float* mixBuffer, const float* source, int stereoSamples);

    /** Reset all internal filter/delay state. */
    void reset();

private:
    // ── HRTF / Binaural ──────────────────────────────────────────────────

    /** Head model constants */
    static constexpr float HEAD_RADIUS_M   = 0.0875f;   // ~8.75cm
    static constexpr float SPEED_OF_SOUND  = 343.0f;    // m/s at ~20°C
    static constexpr float MAX_ITD_SECONDS = HEAD_RADIUS_M / SPEED_OF_SOUND; // ~0.255ms
    static constexpr int   MAX_ITD_SAMPLES = static_cast<int>(MAX_ITD_SECONDS * 48000.0f + 2); // ~13 samples

    /** Delay line for ITD simulation */
    struct DelayLine {
        std::array<float, 64> buffer{};  // Ring buffer (power of 2 for efficiency)
        int writePos = 0;

        void write(float sample) {
            buffer[writePos & 63] = sample;
            writePos++;
        }
        float read(float delaySamples) const {
            // Linear interpolation for fractional delay
            int pos = writePos - 1;
            int d0 = static_cast<int>(delaySamples);
            float frac = delaySamples - d0;
            float s0 = buffer[(pos - d0) & 63];
            float s1 = buffer[(pos - d0 - 1) & 63];
            return s0 + frac * (s1 - s0);
        }
    };

    /** Large delay line for Doppler pitch-shifting via variable-rate read */
    static constexpr int DOPPLER_BUF_SIZE = 4096;  // Must be power of 2
    static constexpr int DOPPLER_BUF_MASK = DOPPLER_BUF_SIZE - 1;
    struct DopplerLine {
        std::array<float, DOPPLER_BUF_SIZE> buffer{};
        int writePos = 0;
        double readPos = 0.0;  // fractional read position

        void write(float sample) {
            buffer[writePos & DOPPLER_BUF_MASK] = sample;
            writePos++;
        }
        float readAt(double pos) const {
            int i0 = static_cast<int>(pos) & DOPPLER_BUF_MASK;
            int i1 = (i0 + 1) & DOPPLER_BUF_MASK;
            double frac = pos - std::floor(pos);
            return static_cast<float>(buffer[i0] * (1.0 - frac) + buffer[i1] * frac);
        }
        void reset() {
            buffer.fill(0.0f);
            writePos = 0;
            readPos = 0.0;
        }
    };

    /** Two-pole head shadow filter (models high-freq attenuation around head) */
    struct HeadShadowFilter {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;

        void setCoeffs(float angle, float sampleRate);
        float process(float in);
        void reset() { z1 = z2 = 0.0f; }
    };

    /** Air absorption filter (one-pole LP that increases with distance) */
    struct AirAbsorptionFilter {
        float prev = 0.0f;
        float process(float in, float alpha) {
            prev = prev + alpha * (in - prev);
            return prev;
        }
        void reset() { prev = 0.0f; }
    };

    // ── Reverb Engine (Schroeder-style) ──────────────────────────────────

    /** Comb filter for reverb tail */
    struct CombFilter {
        std::vector<float> buffer;
        int writePos = 0;
        float feedback = 0.0f;
        float damp = 0.0f;
        float dampState = 0.0f;

        void init(int delaySamples, float fb, float dmp) {
            buffer.resize(delaySamples, 0.0f);
            writePos = 0;
            feedback = fb;
            damp = dmp;
            dampState = 0.0f;
        }
        float process(float in) {
            float out = buffer[writePos];
            // Damped feedback
            dampState = out * (1.0f - damp) + dampState * damp;
            buffer[writePos] = in + dampState * feedback;
            writePos = (writePos + 1) % static_cast<int>(buffer.size());
            return out;
        }
        void clear() {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            dampState = 0.0f;
        }
    };

    /** Allpass filter for reverb diffusion */
    struct AllpassFilter {
        std::vector<float> buffer;
        int writePos = 0;
        float feedback = 0.5f;

        void init(int delaySamples, float fb = 0.5f) {
            buffer.resize(delaySamples, 0.0f);
            writePos = 0;
            feedback = fb;
        }
        float process(float in) {
            float delayed = buffer[writePos];
            float out = -in + delayed;
            buffer[writePos] = in + delayed * feedback;
            writePos = (writePos + 1) % static_cast<int>(buffer.size());
            return out;
        }
        void clear() { std::fill(buffer.begin(), buffer.end(), 0.0f); }
    };

    /** Early reflection tap */
    struct EarlyReflection {
        int delaySamples;
        float gainL, gainR;   // per-ear gain (simulates reflection direction)
    };

    /** Full reverb unit */
    struct ReverbEngine {
        // 4 parallel comb filters (Schroeder design)
        std::array<CombFilter, 4> combsL;
        std::array<CombFilter, 4> combsR;
        // 2 series allpass filters
        std::array<AllpassFilter, 2> allpassL;
        std::array<AllpassFilter, 2> allpassR;
        // Early reflections delay line
        std::vector<float> earlyDelayLine;
        int earlyWritePos = 0;
        static constexpr int EARLY_DELAY_SIZE = 4096;

        // 6 early reflection taps (stadium-like environment)
        std::array<EarlyReflection, 6> earlyTaps;

        void init(float sampleRate);
        void process(float monoIn, float& outL, float& outR);
        void clear();
    };

    // ── Smooth parameter interpolation ───────────────────────────────────
    struct SmoothParam {
        float current = 0.0f;
        float target  = 0.0f;
        float smooth(float coeff = 0.005f) {
            current += coeff * (target - current);
            return current;
        }
        void set(float val) { target = val; }
        void snap(float val) { current = target = val; }
    };

    // ── Utility ──────────────────────────────────────────────────────────
    static float smoothstep(float edge0, float edge1, float x);
    static float yawToRadians(int yaw);
    static float unitsToMeters(float uu) { return uu / 100.0f; }  // UE4: 1uu ≈ 1cm

    // ── State ────────────────────────────────────────────────────────────
    bool  enabled_        = true;
    float innerRadius_    = Protocol::DEFAULT_FULL_VOL_DISTANCE;
    float outerRadius_    = Protocol::DEFAULT_MAX_DISTANCE;
    float rolloff_        = 1.0f;
    float masterVolume_   = 1.0f;
    bool  reverbEnabled_  = true;
    float reverbMix_      = 0.90f;   // 0 = dry only, 1 = full wet (heavy reverb)

    // Per-source processing state
    DelayLine delayL_, delayR_;
    HeadShadowFilter headFilterL_, headFilterR_;
    AirAbsorptionFilter airAbsL_, airAbsR_;
    AirAbsorptionFilter airAbsMono_;   // Pre-reverb absorption
    ReverbEngine reverb_;

    // Doppler effect state
    DopplerLine dopplerL_, dopplerR_;
    float prevDistUU_ = -1.0f;         // Previous frame distance (for velocity)
    float dopplerPitchRatio_ = 1.0f;   // Current pitch multiplier
    static constexpr float DOPPLER_EXAGGERATION = 1.2f;  // Subtle exaggeration — realistic feel
    static constexpr float DOPPLER_SMOOTH = 0.00005f;    // Extremely slow smoothing — silky gradual pitch glide

    // Smooth interpolation for gains and panning
    SmoothParam smoothGainL_, smoothGainR_;
    SmoothParam smoothDelayL_, smoothDelayR_;
    SmoothParam smoothVolume_;
    SmoothParam smoothReverbSend_;
    SmoothParam smoothDopplerPitch_;

    bool firstFrame_ = true;   // Snap parameters on first frame
};
