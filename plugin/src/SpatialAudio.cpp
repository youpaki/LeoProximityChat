#include "pch.h"
#include "SpatialAudio.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SpatialAudio::SpatialAudio() = default;

void SpatialAudio::setDistanceParams(float innerRadius, float outerRadius, float rolloff) {
    innerRadius_ = std::max(0.0f, innerRadius);
    outerRadius_ = std::max(innerRadius_ + 1.0f, outerRadius);
    rolloff_     = std::max(0.1f, rolloff);
}

float SpatialAudio::smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float SpatialAudio::yawToRadians(int yaw) {
    // Unreal rotation: 0-65535 maps to 0-360 degrees
    // Convert to radians, with 0 = forward (+X in Unreal)
    return static_cast<float>(yaw) / 65536.0f * 2.0f * static_cast<float>(M_PI);
}

float SpatialAudio::process(
    const float* monoIn, int frameSize, float* stereoOut,
    const Protocol::Vec3& listenerPos, int listenerYaw,
    const Protocol::Vec3& sourcePos)
{
    if (!enabled_) {
        // Bypass: center-panned mono → stereo
        float vol = masterVolume_ * 0.707f; // -3dB for center
        for (int i = 0; i < frameSize; i++) {
            stereoOut[i * 2]     = monoIn[i] * vol;
            stereoOut[i * 2 + 1] = monoIn[i] * vol;
        }
        return vol;
    }

    // ── 1. Calculate relative position ──────────────────────────────────
    Protocol::Vec3 rel = sourcePos - listenerPos;
    float distance = rel.length();

    // ── 2. Distance attenuation ─────────────────────────────────────────
    float volume = 0.0f;
    if (distance <= innerRadius_) {
        volume = 1.0f;
    } else if (distance >= outerRadius_) {
        volume = 0.0f;
    } else {
        // Smooth rolloff between inner and outer radius
        float t = (distance - innerRadius_) / (outerRadius_ - innerRadius_);
        // Apply rolloff exponent for sharper/softer curves
        t = std::pow(t, rolloff_);
        volume = 1.0f - smoothstep(0.0f, 1.0f, t);
    }

    volume *= masterVolume_;

    // Early out if too far
    if (volume < 0.001f) {
        std::memset(stereoOut, 0, frameSize * 2 * sizeof(float));
        return 0.0f;
    }

    // ── 3. Stereo panning ───────────────────────────────────────────────
    // Convert listener yaw to forward and right vectors (2D, XY plane)
    float yawRad = yawToRadians(listenerYaw);
    float fwdX = std::cos(yawRad);
    float fwdY = std::sin(yawRad);
    float rightX = -fwdY;  // Right = perpendicular to forward (rotated 90° CW)
    float rightY = fwdX;

    // Direction to source (normalized, 2D)
    float dirLen2D = std::sqrt(rel.x * rel.x + rel.y * rel.y);
    float pan = 0.0f;
    if (dirLen2D > 1e-4f) {
        float dirX = rel.x / dirLen2D;
        float dirY = rel.y / dirLen2D;
        // Dot product with right vector gives panning: -1 (left) to +1 (right)
        pan = dirX * rightX + dirY * rightY;
    }

    // Also compute front/back factor for subtle rear attenuation
    float frontDot = 0.0f;
    if (dirLen2D > 1e-4f) {
        frontDot = (rel.x / dirLen2D) * fwdX + (rel.y / dirLen2D) * fwdY;
    }
    // Slight rear attenuation (sources behind you are ~20% quieter)
    float rearFactor = 1.0f - 0.2f * std::max(0.0f, -frontDot);

    // Equal-power panning law
    // pan: -1 = full left, 0 = center, +1 = full right
    float panAngle = pan * 0.5f * static_cast<float>(M_PI) * 0.5f; // map to -π/4 to π/4
    float leftGain  = std::cos(panAngle + static_cast<float>(M_PI) * 0.25f);
    float rightGain = std::sin(panAngle + static_cast<float>(M_PI) * 0.25f);

    // Clamp gains
    leftGain  = std::max(0.0f, leftGain)  * volume * rearFactor;
    rightGain = std::max(0.0f, rightGain) * volume * rearFactor;

    // ── 4. Apply gains ──────────────────────────────────────────────────
    for (int i = 0; i < frameSize; i++) {
        stereoOut[i * 2]     = monoIn[i] * leftGain;
        stereoOut[i * 2 + 1] = monoIn[i] * rightGain;
    }

    // ── 5. Distance-based low-pass filter ───────────────────────────────
    if (lowPassEnabled_ && distance > innerRadius_) {
        float distFactor = std::clamp(
            (distance - innerRadius_) / (outerRadius_ - innerRadius_),
            0.0f, 1.0f
        );
        applyLowPass(stereoOut, frameSize, distFactor, lpState_);
    }

    return volume;
}

void SpatialAudio::applyLowPass(float* stereoOut, int frameSize, float distanceFactor, LowPassState& state) {
    // Simple one-pole low-pass filter
    // As distance increases, cutoff decreases (more muffled)
    // alpha = 1.0 means no filtering, alpha → 0 means heavy filtering
    float alpha = 1.0f - (distanceFactor * 0.7f); // Max 70% filtering at max distance
    alpha = std::clamp(alpha, 0.1f, 1.0f);

    for (int i = 0; i < frameSize; i++) {
        float inL = stereoOut[i * 2];
        float inR = stereoOut[i * 2 + 1];

        state.prevL = state.prevL + alpha * (inL - state.prevL);
        state.prevR = state.prevR + alpha * (inR - state.prevR);

        stereoOut[i * 2]     = state.prevL;
        stereoOut[i * 2 + 1] = state.prevR;
    }
}

void SpatialAudio::mixInto(float* mixBuffer, const float* source, int stereoSamples) {
    for (int i = 0; i < stereoSamples; i++) {
        mixBuffer[i] += source[i];
    }
}
