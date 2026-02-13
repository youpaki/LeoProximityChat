#include "pch.h"
#include "NetworkManager.h"

using json = nlohmann::json;

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() {
    disconnect();
}

bool NetworkManager::connect(const std::string& serverUrl) {
    if (state_ == ConnectionState::Connected || state_ == ConnectionState::Connecting) {
        disconnect();
    }

    serverUrl_ = serverUrl;
    setState(ConnectionState::Connecting, "Connecting to " + serverUrl);

    // Configure WebSocket
    webSocket_.setUrl(serverUrl);

    // Auto-reconnect settings
    if (autoReconnect_) {
        // IXWebSocket handles reconnection internally
        webSocket_.enableAutomaticReconnection();
        ix::WebSocketPerMessageDeflateOptions deflateOpts;
        webSocket_.setPerMessageDeflateOptions(deflateOpts);
    } else {
        webSocket_.disableAutomaticReconnection();
    }

    // Set message handler
    webSocket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        onMessage(msg);
    });

    // Set ping interval for keepalive
    webSocket_.setPingInterval(15); // seconds

    // Start connection (non-blocking)
    webSocket_.start();

    return true;
}

void NetworkManager::disconnect() {
    leaveRoom();
    webSocket_.stop();
    setState(ConnectionState::Disconnected);

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.clear();
    }

    currentMatchId_.clear();
    localSteamId_.clear();
}

void NetworkManager::joinRoom(const std::string& matchId, const std::string& playerName, const std::string& steamId) {
    if (state_ != ConnectionState::Connected) return;

    currentMatchId_   = matchId;
    localPlayerName_  = playerName;
    localSteamId_     = steamId;

    json msg = {
        {"type",       "join"},
        {"matchId",    matchId},
        {"playerName", playerName},
        {"steamId",    steamId}
    };

    webSocket_.send(msg.dump());
}

void NetworkManager::leaveRoom() {
    if (state_ == ConnectionState::Connected && !currentMatchId_.empty()) {
        json msg = {{"type", "leave"}};
        webSocket_.send(msg.dump());
    }
    currentMatchId_.clear();

    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Send
// ═════════════════════════════════════════════════════════════════════════════

void NetworkManager::sendAudioPacket(const std::vector<uint8_t>& packet) {
    if (state_ != ConnectionState::Connected || currentMatchId_.empty()) return;

    // Send as binary
    std::string binaryData(reinterpret_cast<const char*>(packet.data()), packet.size());
    webSocket_.sendBinary(binaryData);

    bytesSent_ += packet.size();
}

void NetworkManager::sendPositionUpdate(const Protocol::Vec3& pos, int yaw, int pitch) {
    if (state_ != ConnectionState::Connected || currentMatchId_.empty()) return;

    json msg = {
        {"type",  "position"},
        {"x",     pos.x},
        {"y",     pos.y},
        {"z",     pos.z},
        {"yaw",   yaw},
        {"pitch", pitch}
    };

    webSocket_.send(msg.dump());
}

// ═════════════════════════════════════════════════════════════════════════════
// Message Handling
// ═════════════════════════════════════════════════════════════════════════════

void NetworkManager::onMessage(const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            setState(ConnectionState::Connected, "Connected to " + serverUrl_);
            break;

        case ix::WebSocketMessageType::Close:
            setState(autoReconnect_ ? ConnectionState::Reconnecting : ConnectionState::Disconnected,
                     "Connection closed: " + msg->closeInfo.reason);
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                peers_.clear();
            }
            break;

        case ix::WebSocketMessageType::Error:
            setError(msg->errorInfo.reason);
            setState(ConnectionState::Error, msg->errorInfo.reason);
            break;

        case ix::WebSocketMessageType::Message:
            if (msg->binary) {
                handleBinaryMessage(msg->str);
            } else {
                handleTextMessage(msg->str);
            }
            break;

        case ix::WebSocketMessageType::Ping:
        case ix::WebSocketMessageType::Pong:
        case ix::WebSocketMessageType::Fragment:
            break;
    }
}

void NetworkManager::handleTextMessage(const std::string& text) {
    try {
        json msg = json::parse(text);
        std::string type = msg.value("type", "");

        if (type == "welcome") {
            // Server acknowledged our join, gives us list of existing peers
            if (msg.contains("peers") && msg["peers"].is_array()) {
                std::lock_guard<std::mutex> lock(peersMutex_);
                for (const auto& peer : msg["peers"]) {
                    std::string sid = peer.value("steamId", "");
                    std::string name = peer.value("playerName", "Unknown");
                    if (!sid.empty()) {
                        peers_[sid] = { sid, name };
                        if (peerJoinedCb_) peerJoinedCb_(sid, name);
                    }
                }
            }
        }
        else if (type == "peer_joined") {
            std::string sid = msg.value("steamId", "");
            std::string name = msg.value("playerName", "Unknown");
            if (!sid.empty()) {
                {
                    std::lock_guard<std::mutex> lock(peersMutex_);
                    peers_[sid] = { sid, name };
                }
                if (peerJoinedCb_) peerJoinedCb_(sid, name);
            }
        }
        else if (type == "peer_left") {
            std::string sid = msg.value("steamId", "");
            std::string name;
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                auto it = peers_.find(sid);
                if (it != peers_.end()) {
                    name = it->second.playerName;
                    peers_.erase(it);
                }
            }
            if (peerLeftCb_) peerLeftCb_(sid, name);
        }
        else if (type == "peer_position") {
            // Position update from a peer (when they're not sending audio)
            // Could be used to update spatial position even when silent
        }
        else if (type == "error") {
            std::string errMsg = msg.value("message", "Unknown error");
            setError("Server: " + errMsg);
        }
        else if (type == "pong") {
            // Server pong response — connection is alive
        }
    }
    catch (const json::exception& e) {
        setError(std::string("JSON parse error: ") + e.what());
    }
}

void NetworkManager::handleBinaryMessage(const std::string& data) {
    bytesReceived_ += data.size();

    Protocol::AudioPacket packet;
    if (Protocol::parseIncomingAudioPacket(
            reinterpret_cast<const uint8_t*>(data.data()), data.size(), packet))
    {
        // Don't process our own audio (shouldn't happen, but safety check)
        if (packet.senderSteamId != localSteamId_ && audioReceivedCb_) {
            audioReceivedCb_(packet);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// State
// ═════════════════════════════════════════════════════════════════════════════

void NetworkManager::setState(ConnectionState state, const std::string& info) {
    state_ = state;
    if (stateChangedCb_) {
        stateChangedCb_(state, info);
    }
}

std::string NetworkManager::getStateString() const {
    switch (state_.load()) {
        case ConnectionState::Disconnected:  return "Disconnected";
        case ConnectionState::Connecting:    return "Connecting...";
        case ConnectionState::Connected:     return "Connected";
        case ConnectionState::Reconnecting:  return "Reconnecting...";
        case ConnectionState::Error:         return "Error";
        default:                             return "Unknown";
    }
}

std::vector<NetworkManager::PeerInfo> NetworkManager::getConnectedPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    for (const auto& [sid, info] : peers_) {
        result.push_back(info);
    }
    return result;
}
