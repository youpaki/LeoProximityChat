#pragma once
#include <cstdint>
#include <vector>
#include <string>

/**
 * Network protocol definitions for Leo's Rocket Proximity Chat.
 *
 * Binary audio packet layout:
 *   Outgoing (plugin → server):
 *     [0x03] [pos_x:f32le] [pos_y:f32le] [pos_z:f32le] [opus_data...]
 *     Total header: 1 + 12 = 13 bytes
 *
 *   Incoming (server → plugin):
 *     [0x03] [steam_id:u64le] [pos_x:f32le] [pos_y:f32le] [pos_z:f32le] [opus_data...]
 *     Total header: 1 + 8 + 12 = 21 bytes
 */

namespace Protocol {

    // Message type byte in binary packets
    constexpr uint8_t MSG_AUDIO = 0x03;

    // Sizes
    constexpr size_t OUTGOING_HEADER_SIZE = 1 + 12;          // type + 3 floats
    constexpr size_t INCOMING_HEADER_SIZE = 1 + 8 + 12;      // type + steamId + 3 floats
    constexpr size_t MAX_OPUS_FRAME_BYTES = 1024;             // Max Opus frame size

    // Audio constants
    constexpr int    SAMPLE_RATE      = 48000;
    constexpr int    CHANNELS_MONO    = 1;
    constexpr int    CHANNELS_STEREO  = 2;
    constexpr int    FRAME_DURATION_MS = 20;                  // 20ms frames
    constexpr int    FRAME_SIZE       = SAMPLE_RATE * FRAME_DURATION_MS / 1000; // 960 samples
    constexpr int    OPUS_BITRATE     = 32000;                // 32 kbps - good for voice
    constexpr int    OPUS_COMPLEXITY  = 5;                    // 0-10, balanced

    // Distance/spatial defaults (Unreal Units — RL field is ~10240 x 8192)
    constexpr float  DEFAULT_MAX_DISTANCE      = 15000.0f;   // Almost whole map audible
    constexpr float  DEFAULT_FULL_VOL_DISTANCE  = 2500.0f;    // Full volume within this
    constexpr float  DEFAULT_MASTER_VOLUME      = 1.5f;
    constexpr float  DEFAULT_MIC_VOLUME         = 1.2f;
    constexpr float  DEFAULT_VOICE_THRESHOLD    = 0.01f;     // RMS threshold for VAD
    constexpr float  DEFAULT_HOLD_TIME_MS       = 500.0f;    // VAD hold time
    constexpr float  DEFAULT_ROLLOFF_FACTOR     = 1.0f;      // Distance rolloff exponent

    // Network defaults
    constexpr const char* DEFAULT_SERVER_URL = "ws://localhost:9587";
    constexpr int    RECONNECT_DELAY_MS = 3000;
    constexpr int    POSITION_UPDATE_MS = 50;                 // Send position updates every 50ms

    /** 3D position */
    struct Vec3 {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }

        float length() const { return std::sqrt(x * x + y * y + z * z); }
        float lengthSq() const { return x * x + y * y + z * z; }
        float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

        Vec3 normalized() const {
            float len = length();
            if (len < 1e-6f) return { 0, 0, 0 };
            return { x / len, y / len, z / len };
        }
    };

    /** Rotation in Unreal rotation units (0-65535 = 0-360°) */
    struct Rot {
        int pitch = 0, yaw = 0, roll = 0;
    };

    /** Represents a remote peer */
    struct PeerInfo {
        std::string steamId;
        std::string playerName;
        Vec3 position;
        Rot  rotation;
        bool isSpeaking = false;
        std::chrono::steady_clock::time_point lastHeard;
    };

    /** Encoded audio packet with position */
    struct AudioPacket {
        std::string senderSteamId;
        Vec3 senderPosition;
        std::vector<uint8_t> opusData;
    };

    // ─── Packet building helpers ────────────────────────────────────────

    /** Build an outgoing binary audio packet (client → server) */
    inline std::vector<uint8_t> buildOutgoingAudioPacket(
        const Vec3& pos, const uint8_t* opusData, size_t opusLen)
    {
        std::vector<uint8_t> packet(OUTGOING_HEADER_SIZE + opusLen);
        packet[0] = MSG_AUDIO;
        std::memcpy(packet.data() + 1,  &pos.x, 4);
        std::memcpy(packet.data() + 5,  &pos.y, 4);
        std::memcpy(packet.data() + 9,  &pos.z, 4);
        std::memcpy(packet.data() + 13, opusData, opusLen);
        return packet;
    }

    /** Parse an incoming binary audio packet (server → client) */
    inline bool parseIncomingAudioPacket(
        const uint8_t* data, size_t len, AudioPacket& out)
    {
        if (len < INCOMING_HEADER_SIZE) return false;
        if (data[0] != MSG_AUDIO) return false;

        // Read steamId (uint64 LE)
        uint64_t steamIdNum = 0;
        std::memcpy(&steamIdNum, data + 1, 8);
        out.senderSteamId = std::to_string(steamIdNum);

        // Read position
        std::memcpy(&out.senderPosition.x, data + 9,  4);
        std::memcpy(&out.senderPosition.y, data + 13, 4);
        std::memcpy(&out.senderPosition.z, data + 17, 4);

        // Opus data
        size_t opusLen = len - INCOMING_HEADER_SIZE;
        if (opusLen > 0) {
            out.opusData.assign(data + INCOMING_HEADER_SIZE, data + len);
        }
        return true;
    }

} // namespace Protocol
