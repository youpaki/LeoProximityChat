#include "pch.h"
#include "LeoProximityChat.h"
#include "version.h"

// ImGui (provided by BakkesMod)
#include "imgui/imgui.h"

BAKKESMOD_PLUGIN(LeoProximityChat, "Leo's Rocket Proximity Chat", PLUGIN_VERSION, PLUGINTYPE_FREEPLAY)

// ═════════════════════════════════════════════════════════════════════════════
// Plugin Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::onLoad() {
    log("Loading Leo's Rocket Proximity Chat v" + std::string(PLUGIN_VERSION));

    registerCVars();
    initSubsystems();

    // ── Hook game events ─────────────────────────────────────────────────
    // Tick hook — fires every game tick for position updates
    gameWrapper->HookEvent(
        "Function TAGame.Car_TA.SetVehicleInput",
        std::bind(&LeoProximityChat::onTick, this, std::placeholders::_1)
    );

    // Match lifecycle hooks
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.InitGame",
        std::bind(&LeoProximityChat::onMatchJoined, this, std::placeholders::_1)
    );
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
        std::bind(&LeoProximityChat::onMatchLeft, this, std::placeholders::_1)
    );
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_Soccar_TA.Destroyed",
        std::bind(&LeoProximityChat::onMatchLeft, this, std::placeholders::_1)
    );

    // Also hook online game events as fallback
    gameWrapper->HookEvent(
        "Function Engine.GameInfo.PostLogin",
        std::bind(&LeoProximityChat::onMatchJoined, this, std::placeholders::_1)
    );

    // ── Notifier commands ────────────────────────────────────────────────
    cvarManager->registerNotifier("leo_proxchat_reconnect", [this](std::vector<std::string>) {
        log("Manual reconnect requested");
        disconnectFromServer();
        connectToServer();
    }, "Reconnect to proximity chat server", PERMISSION_ALL);

    cvarManager->registerNotifier("leo_proxchat_refresh_devices", [this](std::vector<std::string>) {
        if (audioEngine_) {
            cachedInputDevices_ = audioEngine_->getInputDevices();
            cachedOutputDevices_ = audioEngine_->getOutputDevices();
            log("Audio devices refreshed");
        }
    }, "Refresh audio device list", PERMISSION_ALL);

    cvarManager->registerNotifier("leo_proxchat_ptt_pressed", [this](std::vector<std::string>) {
        pttKeyDown_ = true;
        if (audioEngine_) audioEngine_->setPTTActive(true);
    }, "PTT key pressed", PERMISSION_ALL);

    cvarManager->registerNotifier("leo_proxchat_ptt_released", [this](std::vector<std::string>) {
        pttKeyDown_ = false;
        if (audioEngine_) audioEngine_->setPTTActive(false);
    }, "PTT key released", PERMISSION_ALL);

    log("Plugin loaded successfully");
}

void LeoProximityChat::onUnload() {
    log("Unloading Leo's Rocket Proximity Chat");

    shutdownSubsystems();

    // Unhook events
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.InitGame");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed");
    gameWrapper->UnhookEvent("Function Engine.GameInfo.PostLogin");
}

// ═════════════════════════════════════════════════════════════════════════════
// CVar Registration
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::registerCVars() {
    // Enable/disable
    cvarManager->registerCvar("leo_proxchat_enabled", "1", "Enable proximity chat", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            enabled_ = cvar.getBoolValue();
            if (!enabled_) {
                if (audioEngine_) audioEngine_->stopStreams();
                disconnectFromServer();
            }
        });

    // Server URL
    cvarManager->registerCvar("leo_proxchat_server_url",
        Protocol::DEFAULT_SERVER_URL, "Relay server URL");

    // Audio volumes
    cvarManager->registerCvar("leo_proxchat_master_volume", "100", "Master volume", true, true, 0, true, 200)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setOutputVolume(cvar.getFloatValue() / 100.0f);
        });

    cvarManager->registerCvar("leo_proxchat_mic_volume", "100", "Microphone volume", true, true, 0, true, 300)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setMicVolume(cvar.getFloatValue() / 100.0f);
        });

    // Voice settings
    cvarManager->registerCvar("leo_proxchat_push_to_talk", "0", "Enable push-to-talk mode", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setPushToTalk(cvar.getBoolValue());
        });

    cvarManager->registerCvar("leo_proxchat_ptt_key", "F3", "Push-to-talk key");

    cvarManager->registerCvar("leo_proxchat_voice_threshold", "1", "Voice activation threshold (0-100)", true, true, 0, true, 100)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setVoiceThreshold(cvar.getFloatValue() / 100.0f);
        });

    cvarManager->registerCvar("leo_proxchat_hold_time", "500", "Voice hold time (ms)", true, true, 0, true, 2000)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setHoldTimeMs(cvar.getFloatValue());
        });

    // Proximity settings
    cvarManager->registerCvar("leo_proxchat_max_distance", "8000", "Maximum hearing distance", true, true, 500, true, 15000)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) {
                auto& sa = audioEngine_->getSpatialAudio();
                float inner = cvarManager->getCvar("leo_proxchat_full_vol_distance").getFloatValue();
                sa.setDistanceParams(inner, cvar.getFloatValue());
            }
        });

    cvarManager->registerCvar("leo_proxchat_full_vol_distance", "1500", "Full volume distance", true, true, 0, true, 5000)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) {
                auto& sa = audioEngine_->getSpatialAudio();
                float outer = cvarManager->getCvar("leo_proxchat_max_distance").getFloatValue();
                sa.setDistanceParams(cvar.getFloatValue(), outer);
            }
        });

    cvarManager->registerCvar("leo_proxchat_3d_audio", "1", "Enable 3D spatial audio", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->getSpatialAudio().setEnabled(cvar.getBoolValue());
        });

    cvarManager->registerCvar("leo_proxchat_rolloff", "10", "Distance rolloff factor (1-20)", true, true, 1, true, 20)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            // rolloff stored as int 1-20, used as float 0.1-2.0
            if (audioEngine_) {
                auto& sa = audioEngine_->getSpatialAudio();
                float inner = cvarManager->getCvar("leo_proxchat_full_vol_distance").getFloatValue();
                float outer = cvarManager->getCvar("leo_proxchat_max_distance").getFloatValue();
                sa.setDistanceParams(inner, outer, cvar.getFloatValue() / 10.0f);
            }
        });

    // Device selection (-1 = default)
    cvarManager->registerCvar("leo_proxchat_input_device", "-1", "Input audio device ID");
    cvarManager->registerCvar("leo_proxchat_output_device", "-1", "Output audio device ID");

    // Mic mute
    cvarManager->registerCvar("leo_proxchat_mic_muted", "0", "Mute microphone", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setMicMuted(cvar.getBoolValue());
        });
}

void LeoProximityChat::applyCVarSettings() {
    if (!audioEngine_) return;

    auto getCvar = [this](const char* name) { return cvarManager->getCvar(name); };

    enabled_ = getCvar("leo_proxchat_enabled").getBoolValue();

    audioEngine_->setOutputVolume(getCvar("leo_proxchat_master_volume").getFloatValue() / 100.0f);
    audioEngine_->setMicVolume(getCvar("leo_proxchat_mic_volume").getFloatValue() / 100.0f);
    audioEngine_->setPushToTalk(getCvar("leo_proxchat_push_to_talk").getBoolValue());
    audioEngine_->setVoiceThreshold(getCvar("leo_proxchat_voice_threshold").getFloatValue() / 100.0f);
    audioEngine_->setHoldTimeMs(getCvar("leo_proxchat_hold_time").getFloatValue());
    audioEngine_->setMicMuted(getCvar("leo_proxchat_mic_muted").getBoolValue());

    auto& sa = audioEngine_->getSpatialAudio();
    sa.setEnabled(getCvar("leo_proxchat_3d_audio").getBoolValue());
    sa.setDistanceParams(
        getCvar("leo_proxchat_full_vol_distance").getFloatValue(),
        getCvar("leo_proxchat_max_distance").getFloatValue(),
        getCvar("leo_proxchat_rolloff").getFloatValue() / 10.0f
    );

    pttKeyName_ = getCvar("leo_proxchat_ptt_key").getStringValue();

    // Device selection
    int inputDev = getCvar("leo_proxchat_input_device").getIntValue();
    int outputDev = getCvar("leo_proxchat_output_device").getIntValue();
    if (inputDev >= 0) audioEngine_->setInputDevice(inputDev);
    if (outputDev >= 0) audioEngine_->setOutputDevice(outputDev);
}

// ═════════════════════════════════════════════════════════════════════════════
// Subsystem Init/Shutdown
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::initSubsystems() {
    if (subsystemsInitialized_) return;

    // ── Audio Engine ─────────────────────────────────────────────────────
    audioEngine_ = std::make_unique<AudioEngine>();
    if (!audioEngine_->initialize()) {
        logError("Audio engine failed to initialize: " + audioEngine_->getLastError());
        // Continue — network may still work, and audio can be retried
    } else {
        // Cache device list
        cachedInputDevices_ = audioEngine_->getInputDevices();
        cachedOutputDevices_ = audioEngine_->getOutputDevices();
        lastDeviceRefresh_ = std::chrono::steady_clock::now();
    }

    // ── Network Manager ──────────────────────────────────────────────────
    networkManager_ = std::make_unique<NetworkManager>();

    // Wire audio output → network send
    audioEngine_->setPacketReadyCallback([this](const std::vector<uint8_t>& packet) {
        if (networkManager_ && networkManager_->isConnected()) {
            networkManager_->sendAudioPacket(packet);
        }
    });

    // Wire network receive → audio input
    networkManager_->setAudioReceivedCallback([this](const Protocol::AudioPacket& packet) {
        if (audioEngine_) {
            audioEngine_->feedIncomingPacket(packet);
        }
    });

    // Peer notifications
    networkManager_->setPeerJoinedCallback([this](const std::string& steamId, const std::string& name) {
        log("Peer joined: " + name + " (" + steamId + ")");
    });

    networkManager_->setPeerLeftCallback([this](const std::string& steamId, const std::string& name) {
        log("Peer left: " + name + " (" + steamId + ")");
    });

    // Connection state changes
    networkManager_->setStateChangedCallback([this](NetworkManager::ConnectionState state, const std::string& info) {
        std::string stateStr;
        switch (state) {
            case NetworkManager::ConnectionState::Connected:     stateStr = "Connected"; break;
            case NetworkManager::ConnectionState::Disconnected:  stateStr = "Disconnected"; break;
            case NetworkManager::ConnectionState::Connecting:    stateStr = "Connecting"; break;
            case NetworkManager::ConnectionState::Reconnecting:  stateStr = "Reconnecting"; break;
            case NetworkManager::ConnectionState::Error:         stateStr = "Error"; break;
        }
        log("Network: " + stateStr + " — " + info);

        // When connected and in a match, join the room
        if (state == NetworkManager::ConnectionState::Connected && inMatch_) {
            std::string matchId = getMatchId();
            if (!matchId.empty()) {
                networkManager_->joinRoom(matchId, getLocalPlayerName(), getLocalSteamId());
            }
        }
    });

    // Apply saved settings
    applyCVarSettings();

    subsystemsInitialized_ = true;
}

void LeoProximityChat::shutdownSubsystems() {
    if (networkManager_) {
        networkManager_->disconnect();
        networkManager_.reset();
    }

    if (audioEngine_) {
        audioEngine_->shutdown();
        audioEngine_.reset();
    }

    subsystemsInitialized_ = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Game Event Handlers
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::onTick(std::string /*eventName*/) {
    if (!enabled_ || !inMatch_) return;

    // Update listener position for 3D audio
    Protocol::Vec3 pos = getLocalCarPosition();
    int yaw = getLocalCarYaw();

    if (audioEngine_) {
        audioEngine_->setListenerState(pos, yaw);
        audioEngine_->setLocalPosition(pos);
    }
}

void LeoProximityChat::onMatchJoined(std::string /*eventName*/) {
    if (!enabled_) return;
    if (inMatch_) return; // Avoid duplicate joins

    inMatch_ = true;
    log("Match detected — starting proximity chat");

    // Start audio streams
    if (audioEngine_ && audioEngine_->isInitialized()) {
        if (!audioEngine_->isStreaming()) {
            if (!audioEngine_->startStreams()) {
                logError("Failed to start audio streams: " + audioEngine_->getLastError());
            }
        }
    }

    // Connect to server and join room
    connectToServer();
}

void LeoProximityChat::onMatchLeft(std::string /*eventName*/) {
    if (!inMatch_) return;

    inMatch_ = false;
    log("Match ended — stopping proximity chat");

    // Stop audio
    if (audioEngine_) {
        audioEngine_->stopStreams();
    }

    // Leave room (but keep connection for quick rejoin)
    if (networkManager_ && networkManager_->isConnected()) {
        networkManager_->leaveRoom();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Network Connection
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::connectToServer() {
    if (!networkManager_) return;

    std::string serverUrl = cvarManager->getCvar("leo_proxchat_server_url").getStringValue();
    if (serverUrl.empty()) {
        serverUrl = Protocol::DEFAULT_SERVER_URL;
    }

    if (!networkManager_->isConnected()) {
        networkManager_->connect(serverUrl);
    }

    // Join room if already connected
    if (networkManager_->isConnected()) {
        std::string matchId = getMatchId();
        if (!matchId.empty()) {
            networkManager_->joinRoom(matchId, getLocalPlayerName(), getLocalSteamId());
        }
    }
}

void LeoProximityChat::disconnectFromServer() {
    if (networkManager_) {
        networkManager_->disconnect();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Game State Helpers
// ═════════════════════════════════════════════════════════════════════════════

std::string LeoProximityChat::getMatchId() const {
    if (!gameWrapper) return "";

    // Try to get the server wrapper for online/offline match info
    ServerWrapper server = gameWrapper->GetCurrentGameState();
    if (!server) return "";

    // Use match GUID if available
    try {
        // Construct a pseudo-match ID from game state
        // In online play, we can combine server info
        auto playlist = server.GetPlaylistId();
        auto matchGUID = server.GetMatchGUID();

        if (!matchGUID.empty()) {
            return matchGUID;
        }

        // Fallback: create ID from player UIDs in the match (sorted)
        ArrayWrapper<PriWrapper> pris = server.GetPRIs();
        std::vector<std::string> ids;
        for (int i = 0; i < pris.Count(); i++) {
            PriWrapper pri = pris.Get(i);
            if (!pri) continue;
            auto uid = pri.GetUniqueIdWrapper();
            ids.push_back(std::to_string(uid.GetUID()));
        }
        std::sort(ids.begin(), ids.end());

        std::string combined = "rl_";
        for (const auto& id : ids) combined += id + "_";
        combined += std::to_string(playlist);
        return combined;
    }
    catch (...) {
        return "unknown_match";
    }
}

std::string LeoProximityChat::getLocalSteamId() const {
    if (!gameWrapper) return "0";

    try {
        auto uid = gameWrapper->GetUniqueID();
        return std::to_string(uid.GetUID());
    }
    catch (...) {
        return "0";
    }
}

std::string LeoProximityChat::getLocalPlayerName() const {
    if (!gameWrapper) return "Unknown";

    try {
        auto pc = gameWrapper->GetPlayerController();
        if (!pc) return "Unknown";
        auto pri = pc.GetPRI();
        if (!pri) return "Unknown";
        return pri.GetPlayerName().ToString();
    }
    catch (...) {
        return "Unknown";
    }
}

Protocol::Vec3 LeoProximityChat::getLocalCarPosition() const {
    if (!gameWrapper) return {};

    try {
        auto car = gameWrapper->GetLocalCar();
        if (!car) return {};
        Vector loc = car.GetLocation();
        return { loc.X, loc.Y, loc.Z };
    }
    catch (...) {
        return {};
    }
}

int LeoProximityChat::getLocalCarYaw() const {
    if (!gameWrapper) return 0;

    try {
        auto car = gameWrapper->GetLocalCar();
        if (!car) return 0;
        Rotator rot = car.GetRotation();
        return rot.Yaw;
    }
    catch (...) {
        return 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Settings UI (ImGui)
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::SetImGuiContext(uintptr_t ctx) {
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

void LeoProximityChat::RenderSettings() {
    // Header
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Leo's Rocket Proximity Chat v%s", PLUGIN_VERSION);
    ImGui::Separator();

    // Enable toggle
    CVarWrapper enabledCvar = cvarManager->getCvar("leo_proxchat_enabled");
    bool isEnabled = enabledCvar.getBoolValue();
    if (ImGui::Checkbox("Enable Proximity Chat", &isEnabled)) {
        enabledCvar.setValue(isEnabled);
    }

    if (!isEnabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Plugin is disabled");
        return;
    }

    ImGui::Spacing();

    // Tabs
    if (ImGui::BeginTabBar("ProxChatTabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            renderAudioSettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voice")) {
            renderVoiceSettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Proximity")) {
            renderProximitySettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Network")) {
            renderNetworkSettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Status")) {
            renderStatusPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void LeoProximityChat::renderAudioSettings() {
    ImGui::Text("Audio Devices");
    ImGui::Separator();

    // Refresh devices periodically or on demand
    auto now = std::chrono::steady_clock::now();
    bool needRefresh = std::chrono::duration_cast<std::chrono::seconds>(now - lastDeviceRefresh_).count() > 10;

    if (needRefresh && audioEngine_ && audioEngine_->isInitialized()) {
        cachedInputDevices_ = audioEngine_->getInputDevices();
        cachedOutputDevices_ = audioEngine_->getOutputDevices();
        lastDeviceRefresh_ = now;
    }

    // Input device combo
    int inputId = cvarManager->getCvar("leo_proxchat_input_device").getIntValue();
    ImGui::Text("Microphone:");
    renderDeviceCombo("##InputDevice", inputId, cachedInputDevices_);
    if (inputId != cvarManager->getCvar("leo_proxchat_input_device").getIntValue()) {
        cvarManager->getCvar("leo_proxchat_input_device").setValue(inputId);
        if (audioEngine_) audioEngine_->setInputDevice(inputId);
    }

    // Output device combo
    int outputId = cvarManager->getCvar("leo_proxchat_output_device").getIntValue();
    ImGui::Text("Speakers/Headphones:");
    renderDeviceCombo("##OutputDevice", outputId, cachedOutputDevices_);
    if (outputId != cvarManager->getCvar("leo_proxchat_output_device").getIntValue()) {
        cvarManager->getCvar("leo_proxchat_output_device").setValue(outputId);
        if (audioEngine_) audioEngine_->setOutputDevice(outputId);
    }

    if (ImGui::Button("Refresh Devices")) {
        cvarManager->executeCommand("leo_proxchat_refresh_devices");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Volume");

    // Master volume slider
    CVarWrapper masterVol = cvarManager->getCvar("leo_proxchat_master_volume");
    float masterVal = masterVol.getFloatValue();
    if (ImGui::SliderFloat("Master Volume", &masterVal, 0.0f, 200.0f, "%.0f%%")) {
        masterVol.setValue(masterVal);
    }

    // Mic volume slider
    CVarWrapper micVol = cvarManager->getCvar("leo_proxchat_mic_volume");
    float micVal = micVol.getFloatValue();
    if (ImGui::SliderFloat("Mic Volume", &micVal, 0.0f, 300.0f, "%.0f%%")) {
        micVol.setValue(micVal);
    }

    // Mic mute
    CVarWrapper micMuted = cvarManager->getCvar("leo_proxchat_mic_muted");
    bool muted = micMuted.getBoolValue();
    if (ImGui::Checkbox("Mute Microphone", &muted)) {
        micMuted.setValue(muted);
    }

    // Show input level meter
    if (audioEngine_) {
        float level = audioEngine_->getCurrentInputLevel();
        ImGui::Text("Mic Level:");
        ImGui::SameLine();
        ImGui::ProgressBar(std::min(level * 10.0f, 1.0f), ImVec2(-1, 0), audioEngine_->isSpeaking() ? "SPEAKING" : "");
    }
}

void LeoProximityChat::renderVoiceSettings() {
    ImGui::Text("Voice Activation");
    ImGui::Separator();

    // PTT toggle
    CVarWrapper pttCvar = cvarManager->getCvar("leo_proxchat_push_to_talk");
    bool ptt = pttCvar.getBoolValue();
    if (ImGui::Checkbox("Push to Talk", &ptt)) {
        pttCvar.setValue(ptt);
    }

    if (ptt) {
        // PTT key binding
        CVarWrapper pttKeyCvar = cvarManager->getCvar("leo_proxchat_ptt_key");
        std::string key = pttKeyCvar.getStringValue();

        char keyBuf[32];
        strncpy_s(keyBuf, key.c_str(), sizeof(keyBuf) - 1);
        ImGui::Text("PTT Key:");
        ImGui::SameLine();
        if (ImGui::InputText("##PTTKey", keyBuf, sizeof(keyBuf))) {
            pttKeyCvar.setValue(std::string(keyBuf));
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Bind in console: bind %s \"leo_proxchat_ptt_pressed\"; unbind_release %s \"leo_proxchat_ptt_released\"",
            keyBuf, keyBuf);
    } else {
        ImGui::Text("Open Mic Settings");
        ImGui::Spacing();

        // Voice threshold
        CVarWrapper threshCvar = cvarManager->getCvar("leo_proxchat_voice_threshold");
        float thresh = threshCvar.getFloatValue();
        if (ImGui::SliderFloat("Voice Threshold", &thresh, 0.0f, 100.0f, "%.1f")) {
            threshCvar.setValue(thresh);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Lower = more sensitive. Increase if transmitting background noise.");

        // Hold time
        CVarWrapper holdCvar = cvarManager->getCvar("leo_proxchat_hold_time");
        float hold = holdCvar.getFloatValue();
        if (ImGui::SliderFloat("Hold Time (ms)", &hold, 0.0f, 2000.0f, "%.0f ms")) {
            holdCvar.setValue(hold);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "How long to keep transmitting after voice stops.");
    }
}

void LeoProximityChat::renderProximitySettings() {
    ImGui::Text("3D Proximity Audio");
    ImGui::Separator();

    // 3D audio toggle
    CVarWrapper spatialCvar = cvarManager->getCvar("leo_proxchat_3d_audio");
    bool spatial = spatialCvar.getBoolValue();
    if (ImGui::Checkbox("Enable 3D Spatial Audio", &spatial)) {
        spatialCvar.setValue(spatial);
    }

    if (spatial) {
        // Max distance
        CVarWrapper maxDistCvar = cvarManager->getCvar("leo_proxchat_max_distance");
        float maxDist = maxDistCvar.getFloatValue();
        if (ImGui::SliderFloat("Max Hearing Distance", &maxDist, 500.0f, 15000.0f, "%.0f uu")) {
            maxDistCvar.setValue(maxDist);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Beyond this distance, you won't hear the player. (Field ~10240 uu long)");

        // Full volume distance
        CVarWrapper fullDistCvar = cvarManager->getCvar("leo_proxchat_full_vol_distance");
        float fullDist = fullDistCvar.getFloatValue();
        if (ImGui::SliderFloat("Full Volume Distance", &fullDist, 0.0f, 5000.0f, "%.0f uu")) {
            fullDistCvar.setValue(fullDist);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Within this distance, voice is at full volume.");

        // Rolloff factor
        CVarWrapper rolloffCvar = cvarManager->getCvar("leo_proxchat_rolloff");
        float rolloff = rolloffCvar.getFloatValue();
        if (ImGui::SliderFloat("Rolloff Curve", &rolloff, 1.0f, 20.0f, "%.1f")) {
            rolloffCvar.setValue(rolloff);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Higher = sharper volume dropoff with distance.");
    }
}

void LeoProximityChat::renderNetworkSettings() {
    ImGui::Text("Server Connection");
    ImGui::Separator();

    // Server URL
    CVarWrapper urlCvar = cvarManager->getCvar("leo_proxchat_server_url");
    std::string url = urlCvar.getStringValue();
    char urlBuf[256];
    strncpy_s(urlBuf, url.c_str(), sizeof(urlBuf) - 1);
    ImGui::Text("Server URL:");
    if (ImGui::InputText("##ServerURL", urlBuf, sizeof(urlBuf))) {
        urlCvar.setValue(std::string(urlBuf));
    }

    ImGui::Spacing();
    if (ImGui::Button("Reconnect")) {
        cvarManager->executeCommand("leo_proxchat_reconnect");
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        disconnectFromServer();
    }

    // Connection info
    if (networkManager_) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Connection Status: %s", networkManager_->getStateString().c_str());

        if (networkManager_->isConnected()) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Connected");
            ImGui::Text("Match Room: %s", networkManager_->getCurrentMatchId().c_str());
        } else {
            std::string err = networkManager_->getLastError();
            if (!err.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", err.c_str());
            }
        }
    }
}

void LeoProximityChat::renderStatusPanel() {
    ImGui::Text("Live Status");
    ImGui::Separator();

    // Plugin state
    ImGui::Text("Plugin: %s", enabled_.load() ? "Enabled" : "Disabled");
    ImGui::Text("In Match: %s", inMatch_.load() ? "Yes" : "No");

    // Audio state
    if (audioEngine_) {
        ImGui::Spacing();
        ImGui::Text("Audio Engine: %s", audioEngine_->isInitialized() ? "OK" : "Not initialized");
        ImGui::Text("Streaming: %s", audioEngine_->isStreaming() ? "Active" : "Stopped");
        ImGui::Text("Speaking: %s", audioEngine_->isSpeaking() ? "Yes" : "No");

        std::string audioErr = audioEngine_->getLastError();
        if (!audioErr.empty()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "Audio: %s", audioErr.c_str());
        }
    }

    // Network state
    if (networkManager_) {
        ImGui::Spacing();
        ImGui::Text("Network: %s", networkManager_->getStateString().c_str());
        ImGui::Text("Sent: %.1f KB", networkManager_->getBytesSent() / 1024.0f);
        ImGui::Text("Received: %.1f KB", networkManager_->getBytesReceived() / 1024.0f);

        // Connected peers
        auto peers = networkManager_->getConnectedPeers();
        ImGui::Spacing();
        ImGui::Text("Connected Peers (%zu):", peers.size());
        for (const auto& peer : peers) {
            ImGui::BulletText("%s (%s)", peer.playerName.c_str(), peer.steamId.c_str());
        }
    }

    // Local player info
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Local Player");
    ImGui::Text("Name: %s", getLocalPlayerName().c_str());
    ImGui::Text("Steam ID: %s", getLocalSteamId().c_str());
    auto pos = getLocalCarPosition();
    ImGui::Text("Position: (%.0f, %.0f, %.0f)", pos.x, pos.y, pos.z);
    ImGui::Text("Match ID: %s", getMatchId().c_str());
}

void LeoProximityChat::renderDeviceCombo(
    const char* label, int& currentId,
    const std::vector<AudioEngine::DeviceInfo>& devices)
{
    // Find current device name for preview
    std::string preview = "Default";
    for (const auto& dev : devices) {
        if (dev.id == currentId) {
            preview = dev.name;
            if (dev.isDefault) preview += " (Default)";
            break;
        }
    }

    if (ImGui::BeginCombo(label, preview.c_str())) {
        // Default option
        bool isDefault = (currentId < 0);
        if (ImGui::Selectable("System Default", isDefault)) {
            currentId = -1;
        }

        for (const auto& dev : devices) {
            std::string displayName = dev.name;
            if (dev.isDefault) displayName += " (Default)";

            bool isSelected = (dev.id == currentId);
            if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                currentId = dev.id;
            }
        }
        ImGui::EndCombo();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Logging
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::log(const std::string& msg) const {
    cvarManager->log("[ProxChat] " + msg);
}

void LeoProximityChat::logError(const std::string& msg) const {
    cvarManager->log("[ProxChat ERROR] " + msg);
}
