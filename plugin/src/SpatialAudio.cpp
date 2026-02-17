#include "SpatialAudio.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// =============================================================================
//  Constructor / Reset
// =============================================================================

SpatialAudio::SpatialAudio() {
    reverb_.init(static_cast<float>(Protocol::SAMPLE_RATE));
    reset();
}

void SpatialAudio::reset() {
    delayL_ = {};
    delayR_ = {};
    dopplerL_.reset();
    dopplerR_.reset();
    prevDistUU_ = -1.0f;
    headFilterL_.reset();
    headFilterR_.reset();
    airAbsL_.reset();
    airAbsR_.reset();
    airAbsMono_.reset();
    reverb_.clear();
    smoothGainL_.snap(0.5f);
    smoothGainR_.snap(0.5f);
    smoothDelayL_.snap(0.0f);
    smoothDelayR_.snap(0.0f);
    smoothVolume_.snap(0.0f);
    smoothReverbSend_.snap(0.0f);
    smoothDopplerPitch_.snap(1.0f);
    firstFrame_ = true;
}

void SpatialAudio::setDistanceParams(float inner, float outer, float rolloff) {
    innerRadius_ = inner;
    outerRadius_ = outer;
    rolloff_ = std::max(rolloff, 0.1f);
}

// =============================================================================
//  Utility
// =============================================================================

float SpatialAudio::smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0 + 1e-9f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float SpatialAudio::yawToRadians(int yaw) {
    // UE4 rotator: 0-65535 → 0-2PI
    return static_cast<float>(yaw) * (2.0f * static_cast<float>(M_PI) / 65536.0f);
}

// =============================================================================
//  Head Shadow Filter (frequency-dependent ILD)
//  Models the shadowing effect of the head for the far ear.
//  Uses a simple 2nd-order low-shelf design controlled by angle.
// =============================================================================

void SpatialAudio::HeadShadowFilter::setCoeffs(float angle, float sampleRate) {
    // angle: 0 = front, PI = directly behind, PI/2 = side
    // Shadow increases as source moves to the opposite ear
    // Alpha controls how much high frequency is removed (0=none, 1=full)
    float shadow = std::clamp(std::sin(angle) * 0.5f + 0.5f, 0.0f, 1.0f);

    // Design a simple one-pole low-pass whose cutoff depends on shadow
    // More shadow → lower cutoff — stronger filtering = more stereo perception
    // Wider range (2kHz–16kHz) for very dramatic head shadow
    float fc = 16000.0f - shadow * 14000.0f; // Range: 2kHz to 16kHz
    fc = std::clamp(fc, 2000.0f, 18000.0f);

    float wc = 2.0f * static_cast<float>(M_PI) * fc / sampleRate;
    float g = std::tan(wc * 0.5f);

    // Bilinear transform one-pole LP
    b0 = g / (1.0f + g);
    b1 = b0;
    b2 = 0.0f;
    a1 = (g - 1.0f) / (1.0f + g);
    a2 = 0.0f;
}

float SpatialAudio::HeadShadowFilter::process(float in) {
    float out = b0 * in + b1 * z1 + b2 * z2 - a1 * z1 - a2 * z2;
    // Simplified: since b2=a2=0, this is effectively a one-pole
    // but we keep the interface for future upgrade to biquad
    z2 = z1;
    z1 = out;
    return out;
}

// =============================================================================
//  Reverb Engine Initialization
//  Stadium-like environment: large room, reflective surfaces
// =============================================================================

void SpatialAudio::ReverbEngine::init(float sampleRate) {
    float srFactor = sampleRate / 44100.0f; // Scale delays for sample rate

    // Comb filter delay times (in samples) — carefully chosen primes to
    // avoid metallic resonance. These simulate a ~30-50m space.
    const int combDelays[4] = {
        static_cast<int>(1557 * srFactor),
        static_cast<int>(1617 * srFactor),
        static_cast<int>(1491 * srFactor),
        static_cast<int>(1422 * srFactor)
    };
    // Slightly different delays for right channel (stereo width)
    const int combDelaysR[4] = {
        static_cast<int>(1557 * srFactor + 23),
        static_cast<int>(1617 * srFactor + 17),
        static_cast<int>(1491 * srFactor + 31),
        static_cast<int>(1422 * srFactor + 13)
    };

    float feedback = 0.84f;  // RT60 ~1.8s (stadium-like)
    float damp = 0.3f;       // Some high-freq damping

    for (int i = 0; i < 4; i++) {
        combsL[i].init(combDelays[i], feedback, damp);
        combsR[i].init(combDelaysR[i], feedback, damp);
    }

    // Allpass delays (smaller, for diffusion)
    const int apDelays[2] = {
        static_cast<int>(556 * srFactor),
        static_cast<int>(441 * srFactor)
    };
    const int apDelaysR[2] = {
        static_cast<int>(556 * srFactor + 11),
        static_cast<int>(441 * srFactor + 7)
    };

    for (int i = 0; i < 2; i++) {
        allpassL[i].init(apDelays[i], 0.5f);
        allpassR[i].init(apDelaysR[i], 0.5f);
    }

    // Early reflections delay line
    earlyDelayLine.resize(EARLY_DELAY_SIZE, 0.0f);
    earlyWritePos = 0;

    // 6 early reflection taps simulating a stadium environment:
    //   Floor, ceiling, left wall, right wall, far wall, near wall
    // Delays in samples, gains simulate distance attenuation & absorption
    earlyTaps[0] = { static_cast<int>(171 * srFactor), 0.45f, 0.45f };  // Floor (centered)
    earlyTaps[1] = { static_cast<int>(353 * srFactor), 0.38f, 0.38f };  // Ceiling (centered)
    earlyTaps[2] = { static_cast<int>(557 * srFactor), 0.55f, 0.22f };  // Left wall
    earlyTaps[3] = { static_cast<int>(619 * srFactor), 0.22f, 0.55f };  // Right wall
    earlyTaps[4] = { static_cast<int>(857 * srFactor), 0.30f, 0.28f };  // Far wall
    earlyTaps[5] = { static_cast<int>(1187 * srFactor), 0.18f, 0.20f }; // Back wall (subtle)
}

void SpatialAudio::ReverbEngine::process(float monoIn, float& outL, float& outR) {
    // ── Early Reflections ──
    earlyDelayLine[earlyWritePos] = monoIn;
    earlyWritePos = (earlyWritePos + 1) % EARLY_DELAY_SIZE;

    float earlyL = 0.0f, earlyR = 0.0f;
    for (auto& tap : earlyTaps) {
        int readPos = (earlyWritePos - tap.delaySamples + EARLY_DELAY_SIZE) % EARLY_DELAY_SIZE;
        float sample = earlyDelayLine[readPos];
        earlyL += sample * tap.gainL;
        earlyR += sample * tap.gainR;
    }

    // ── Late Reverb (parallel combs → series allpass) ──
    float lateL = 0.0f, lateR = 0.0f;
    for (auto& c : combsL) lateL += c.process(monoIn);
    for (auto& c : combsR) lateR += c.process(monoIn);

    // Normalize comb mix
    lateL *= 0.25f;
    lateR *= 0.25f;

    // Diffuse through allpass filters
    for (auto& ap : allpassL) lateL = ap.process(lateL);
    for (auto& ap : allpassR) lateR = ap.process(lateR);

    // Combine early + late
    outL = earlyL * 0.6f + lateL * 0.4f;
    outR = earlyR * 0.6f + lateR * 0.4f;
}

void SpatialAudio::ReverbEngine::clear() {
    for (auto& c : combsL)  c.clear();
    for (auto& c : combsR)  c.clear();
    for (auto& ap : allpassL) ap.clear();
    for (auto& ap : allpassR) ap.clear();
    std::fill(earlyDelayLine.begin(), earlyDelayLine.end(), 0.0f);
    earlyWritePos = 0;
}

// =============================================================================
//  MAIN PROCESSING: Full 3D Spatialization Pipeline
// =============================================================================

float SpatialAudio::process(const float* monoIn, int frameSize, float* stereoOut,
                            const Protocol::Vec3& listenerPos, int listenerYaw,
                            const Protocol::Vec3& sourcePos) {
    // Default: silence
    std::memset(stereoOut, 0, sizeof(float) * frameSize * 2);

    if (!enabled_) {
        // Pass-through center-panned
        for (int i = 0; i < frameSize; i++) {
            stereoOut[i * 2]     = monoIn[i] * masterVolume_;
            stereoOut[i * 2 + 1] = monoIn[i] * masterVolume_;
        }
        return masterVolume_;
    }

    // ─── 1. Geometry: relative position in listener-centric frame ────────

    Protocol::Vec3 delta = sourcePos - listenerPos;
    float distUU = delta.length();
    float distMeters = unitsToMeters(distUU);

    // Listener forward direction (yaw only, in XY plane)
    // UE4: yaw=0 → +X, yaw=16384(90°) → +Y
    float yawRad = yawToRadians(listenerYaw);
    float cosYaw = std::cos(yawRad);
    float sinYaw = std::sin(yawRad);

    // Project delta into listener-local frame:
    //   forward = (cosYaw, sinYaw)  → dot gives forward component
    //   right   = (sinYaw, -cosYaw) → dot gives right component (perpendicular CW)
    float localForward = delta.x * cosYaw + delta.y * sinYaw;
    float localRight   = delta.x * sinYaw - delta.y * cosYaw;

    // Azimuth angle: 0 = front, +PI/2 = right, -PI/2 = left, ±PI = behind
    float azimuth = std::atan2(localRight, localForward + 1e-9f); // -PI to PI
    float absAzimuth = std::abs(azimuth);

    // ─── 2. Distance Attenuation ─────────────────────────────────────────

    float distVolume;
    if (distUU <= innerRadius_) {
        distVolume = 1.0f;
    } else if (distUU >= outerRadius_) {
        distVolume = 0.0f;
    } else {
        // Gentle logarithmic rolloff instead of aggressive smoothstep+pow
        // This keeps voices audible much further away
        float ratio = (distUU - innerRadius_) / (outerRadius_ - innerRadius_ + 1e-9f);
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        // Log rolloff: stays loud longer, fades slowly
        distVolume = 1.0f - std::pow(ratio, 0.6f);
        // Minimum floor — always slightly audible within outer radius
        distVolume = std::max(distVolume, 0.04f);
    }

    if (distVolume <= 0.0f) return 0.0f;

    // ─── 3. HRTF Binaural Rendering ─────────────────────────────────────

    // Interaural Time Delay (ITD):
    // Woodworth formula: ITD = (r/c) * (sin(θ) + θ)  for |θ| ≤ π/2
    // Simplified: ITD ≈ HEAD_RADIUS/c * sin(azimuth) for the near ear
    float sinAz = std::sin(azimuth);
    float itdSeconds = HEAD_RADIUS_M / SPEED_OF_SOUND * sinAz;
    // Convert to samples
    float itdSamples = std::abs(itdSeconds) * static_cast<float>(Protocol::SAMPLE_RATE);
    itdSamples = std::min(itdSamples, static_cast<float>(MAX_ITD_SAMPLES));

    // Assign delay to each ear — INVERTED: source on right → RIGHT ear delayed
    float targetDelayL, targetDelayR;
    if (azimuth >= 0.0f) {
        // Source on right: invert → right ear delayed
        targetDelayL = 0.0f;
        targetDelayR = itdSamples;
    } else {
        targetDelayL = itdSamples;
        targetDelayR = 0.0f;
    }

    // Interaural Level Difference (ILD):
    // Very strong stereo separation — 60% max attenuation for dramatic L/R
    float ildFactor = 1.0f - 0.60f * std::abs(sinAz);  // Max 60% ILD attenuation
    float targetGainL, targetGainR;

    if (azimuth >= 0.0f) {
        // Source on right → INVERTED: left ear louder
        targetGainL = 1.0f;
        targetGainR = ildFactor;
    } else {
        targetGainR = 1.0f;
        targetGainL = ildFactor;
    }

    // Rear attenuation: gentle — sounds behind are slightly softer
    float rearFactor = 1.0f;
    if (absAzimuth > static_cast<float>(M_PI) * 0.5f) {
        float rearness = (absAzimuth - static_cast<float>(M_PI) * 0.5f) /
                         (static_cast<float>(M_PI) * 0.5f);
        rearFactor = 1.0f - rearness * 0.30f; // Up to 30% rear attenuation (more spatial)
    }
    targetGainL *= rearFactor;
    targetGainR *= rearFactor;

    // Apply distance volume + master volume + output gain boost
    constexpr float OUTPUT_GAIN_BOOST = 1.8f;
    targetGainL *= distVolume * masterVolume_ * OUTPUT_GAIN_BOOST;
    targetGainR *= distVolume * masterVolume_ * OUTPUT_GAIN_BOOST;

    // Configure head shadow filters for each ear — INVERTED to match L/R swap
    float angleToLeftEar  = static_cast<float>(M_PI) * 0.5f + azimuth;  // Swapped
    float angleToRightEar = static_cast<float>(M_PI) * 0.5f - azimuth;  // Swapped
    headFilterL_.setCoeffs(std::clamp(angleToLeftEar, 0.0f, static_cast<float>(M_PI)),
                           static_cast<float>(Protocol::SAMPLE_RATE));
    headFilterR_.setCoeffs(std::clamp(angleToRightEar, 0.0f, static_cast<float>(M_PI)),
                           static_cast<float>(Protocol::SAMPLE_RATE));

    // Air absorption: gentle high-frequency rolloff over distance
    // alpha → 1 means no filtering, lower = more low-pass
    float airAlpha;
    if (distMeters < 5.0f) {
        airAlpha = 1.0f;  // No absorption at close range
    } else {
        // Very gentle absorption — just slight muffling at distance
        airAlpha = 1.0f / (1.0f + 0.008f * distMeters); // was 0.04 — 5x gentler
        airAlpha = std::clamp(airAlpha, 0.15f, 1.0f);   // never go below 0.15
    }

    // Reverb send amount increases with distance
    float targetReverbSend = 0.0f;
    if (reverbEnabled_) {
        if (distUU <= innerRadius_) {
            targetReverbSend = 0.15f; // Noticeable room ambience even up close
        } else {
            float t = smoothstep(innerRadius_, outerRadius_, distUU);
            targetReverbSend = 0.15f + t * 0.85f; // Scale up to 100%
        }
        targetReverbSend *= reverbMix_;
    }

    // ─── Doppler Effect ──────────────────────────────────────────────────
    // Compute radial velocity (rate of change of distance)
    // Positive = source moving away, Negative = source approaching
    float targetDopplerPitch = 1.0f;
    if (prevDistUU_ >= 0.0f) {
        // Velocity in UU per frame (20ms at 48kHz = 960 samples)
        float radialVelocityUU = distUU - prevDistUU_;
        // Convert to m/s: UU/frame → m/s (1 UU = 0.01m, 1 frame = 0.02s)
        float radialVelocityMS = (radialVelocityUU * 0.01f) / 0.02f;

        // Doppler formula: f' = f * c / (c + v_s)
        // With exaggeration factor for dramatic effect
        float exaggeratedV = radialVelocityMS * DOPPLER_EXAGGERATION;
        // Clamp to avoid extreme values (max ±0.8c)
        exaggeratedV = std::clamp(exaggeratedV, -SPEED_OF_SOUND * 0.8f, SPEED_OF_SOUND * 0.8f);
        targetDopplerPitch = SPEED_OF_SOUND / (SPEED_OF_SOUND + exaggeratedV);
        // Clamp pitch ratio to sane range
        targetDopplerPitch = std::clamp(targetDopplerPitch, 0.88f, 1.12f); // Very tight range — subtle and smooth
    }
    prevDistUU_ = distUU;

    // Set smooth targets
    smoothGainL_.set(targetGainL);
    smoothGainR_.set(targetGainR);
    smoothDelayL_.set(targetDelayL);
    smoothDelayR_.set(targetDelayR);
    smoothVolume_.set(distVolume);
    smoothReverbSend_.set(targetReverbSend);
    smoothDopplerPitch_.set(targetDopplerPitch);

    // On first frame, snap to avoid initial sweep
    if (firstFrame_) {
        smoothGainL_.snap(targetGainL);
        smoothGainR_.snap(targetGainR);
        smoothDelayL_.snap(targetDelayL);
        smoothDelayR_.snap(targetDelayR);
        smoothVolume_.snap(distVolume);
        smoothReverbSend_.snap(targetReverbSend);
        smoothDopplerPitch_.snap(targetDopplerPitch);
        firstFrame_ = false;
    }

    // ─── 4. Per-sample Processing (with Doppler pitch shifting) ─────────

    // Smoothing coefficient: smooth but responsive enough to feel the stereo
    // 0.0004 at 48kHz ≈ ~55ms time constant
    const float kSmooth = 0.0004f;
    const float kDopplerSmooth = DOPPLER_SMOOTH;

    for (int i = 0; i < frameSize; i++) {
        float mono = monoIn[i];

        // Smoothly interpolate parameters
        float gL = smoothGainL_.smooth(kSmooth);
        float gR = smoothGainR_.smooth(kSmooth);
        float dL = smoothDelayL_.smooth(kSmooth);
        float dR = smoothDelayR_.smooth(kSmooth);
        float reverbSend = smoothReverbSend_.smooth(kSmooth);
        float pitchRatio = smoothDopplerPitch_.smooth(kDopplerSmooth);

        // ── Gentle air absorption on mono signal ──
        float absorbed = airAbsMono_.process(mono, airAlpha);

        // ── Write into Doppler delay lines ──
        dopplerL_.write(absorbed);
        dopplerR_.write(absorbed);

        // ── Doppler pitch shifting: advance read position at variable rate ──
        // pitchRatio > 1 = higher pitch (approaching), < 1 = lower pitch (receding)
        dopplerL_.readPos += static_cast<double>(pitchRatio);
        dopplerR_.readPos += static_cast<double>(pitchRatio);

        // Keep read position from falling too far behind write position
        double maxLag = DOPPLER_BUF_SIZE - 64;
        double writeD = static_cast<double>(dopplerL_.writePos);
        if (writeD - dopplerL_.readPos > maxLag) dopplerL_.readPos = writeD - maxLag;
        if (writeD - dopplerR_.readPos > maxLag) dopplerR_.readPos = writeD - maxLag;
        if (dopplerL_.readPos > writeD - 1.0) dopplerL_.readPos = writeD - 1.0;
        if (dopplerR_.readPos > writeD - 1.0) dopplerR_.readPos = writeD - 1.0;

        // Read Doppler-shifted samples
        float dopplerSampleL = dopplerL_.readAt(dopplerL_.readPos);
        float dopplerSampleR = dopplerR_.readAt(dopplerR_.readPos);

        // ── Write into ITD delay lines ──
        delayL_.write(dopplerSampleL);
        delayR_.write(dopplerSampleR);

        // ── Read with fractional delay (ITD) ──
        float sampleL = delayL_.read(dL);
        float sampleR = delayR_.read(dR);

        // Head shadow: blend filtered vs dry — 95% filtered for strong stereo
        float filteredL = headFilterL_.process(sampleL);
        float filteredR = headFilterR_.process(sampleR);
        sampleL = sampleL * 0.05f + filteredL * 0.95f;
        sampleR = sampleR * 0.05f + filteredR * 0.95f;

        // ── Apply ILD gains ──
        float dryL = sampleL * gL;
        float dryR = sampleR * gR;

        // ── Reverb processing ──
        float wetL = 0.0f, wetR = 0.0f;
        if (reverbEnabled_ && reverbSend > 0.001f) {
            reverb_.process(absorbed * distVolume, wetL, wetR);
            wetL *= reverbSend;
            wetR *= reverbSend;
        }

        // ── Mix dry + wet ──
        stereoOut[i * 2]     = dryL + wetL;
        stereoOut[i * 2 + 1] = dryR + wetR;
    }

    return distVolume;
}

// =============================================================================
//  Additive Mix
// =============================================================================

void SpatialAudio::mixInto(float* mixBuffer, const float* source, int stereoSamples) {
    for (int i = 0; i < stereoSamples * 2; i++) {
        mixBuffer[i] += source[i];
    }
}
