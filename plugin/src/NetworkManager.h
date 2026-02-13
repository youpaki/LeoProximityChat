#pragma once
#include "Protocol.h"
#include "ThreadSafeQueue.h"
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

/**
 * WebSocket-based network manager for communicating with the relay server.
 *
 * Handles:
 *   - Connection to relay server with auto-reconnect
 *   - Room joining/leaving based on match ID
 *   - Sending encoded audio packets (binary)
 *   - Receiving audio packets and forwarding to AudioEngine
 *   - Peer join/leave notifications
 *   - Position updates
 *   - Connection state management
 */
class NetworkManager {
public:
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
        Error
    };

    /** Callbacks */
    using AudioReceivedCallback  = std::function<void(const Protocol::AudioPacket& packet)>;
    using PeerJoinedCallback     = std::function<void(const std::string& steamId, const std::string& name)>;
    using PeerLeftCallback       = std::function<void(const std::string& steamId, const std::string& name)>;
    using StateChangedCallback   = std::function<void(ConnectionState state, const std::string& info)>;

    NetworkManager();
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────
    /** Connect to the relay server. */
    bool connect(const std::string& serverUrl);

    /** Disconnect from the server. */
    void disconnect();

    /** Join a match room on the server. */
    void joinRoom(const std::string& matchId, const std::string& playerName, const std::string& steamId);

    /** Leave the current room. */
    void leaveRoom();

    // ── State ────────────────────────────────────────────────────────────
    ConnectionState getState() const { return state_.load(); }
    bool isConnected() const { return state_.load() == ConnectionState::Connected; }
    std::string getStateString() const;

    const std::string& getCurrentMatchId() const { return currentMatchId_; }
    const std::string& getLocalSteamId() const { return localSteamId_; }

    // ── Send ─────────────────────────────────────────────────────────────
    /** Send a binary audio packet. Thread-safe. */
    void sendAudioPacket(const std::vector<uint8_t>& packet);

    /** Send a position update. */
    void sendPositionUpdate(const Protocol::Vec3& pos, int yaw, int pitch);

    // ── Callbacks ────────────────────────────────────────────────────────
    void setAudioReceivedCallback(AudioReceivedCallback cb)  { audioReceivedCb_ = std::move(cb); }
    void setPeerJoinedCallback(PeerJoinedCallback cb)        { peerJoinedCb_ = std::move(cb); }
    void setPeerLeftCallback(PeerLeftCallback cb)            { peerLeftCb_ = std::move(cb); }
    void setStateChangedCallback(StateChangedCallback cb)    { stateChangedCb_ = std::move(cb); }

    // ── Settings ─────────────────────────────────────────────────────────
    void setAutoReconnect(bool enabled) { autoReconnect_ = enabled; }
    void setReconnectDelay(int ms) { reconnectDelayMs_ = ms; }

    // ── Peer info ────────────────────────────────────────────────────────
    struct PeerInfo {
        std::string steamId;
        std::string playerName;
    };
    std::vector<PeerInfo> getConnectedPeers() const;

    // ── Status ───────────────────────────────────────────────────────────
    uint64_t getBytesSent() const { return bytesSent_.load(); }
    uint64_t getBytesReceived() const { return bytesReceived_.load(); }

    std::string getLastError() const { std::lock_guard<std::mutex> l(errorMutex_); return lastError_; }

private:
    void onMessage(const ix::WebSocketMessagePtr& msg);
    void handleTextMessage(const std::string& text);
    void handleBinaryMessage(const std::string& data);

    void setState(ConnectionState state, const std::string& info = "");

    // WebSocket
    ix::WebSocket webSocket_;
    std::string serverUrl_;

    // State
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::string currentMatchId_;
    std::string localSteamId_;
    std::string localPlayerName_;

    // Peers
    mutable std::mutex peersMutex_;
    std::unordered_map<std::string, PeerInfo> peers_;

    // Callbacks
    AudioReceivedCallback  audioReceivedCb_;
    PeerJoinedCallback     peerJoinedCb_;
    PeerLeftCallback       peerLeftCb_;
    StateChangedCallback   stateChangedCb_;

    // Settings
    bool autoReconnect_ = true;
    int  reconnectDelayMs_ = Protocol::RECONNECT_DELAY_MS;

    // Stats
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};

    // Error
    mutable std::mutex errorMutex_;
    std::string lastError_;

    void setError(const std::string& err) {
        std::lock_guard<std::mutex> l(errorMutex_);
        lastError_ = err;
    }
};
