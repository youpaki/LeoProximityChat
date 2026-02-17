#include "pch.h"
#include "LeoProximityChat.h"
#include "version.h"

#include <algorithm>
#include <vector>

// ImGui (provided by BakkesMod)
#include "imgui.h"

// BakkesMod wrappers needed for game state access
#include "bakkesmod/wrappers/includes.h"
#include "bakkesmod/wrappers/ArrayWrapper.h"
#include "bakkesmod/wrappers/GameEvent/ServerWrapper.h"
#include "bakkesmod/wrappers/GameObject/PriWrapper.h"

// Allow plugin in ALL game modes (online, freeplay, private, etc.)
BAKKESMOD_PLUGIN(LeoProximityChat, "Leo's Rocket Proximity Chat", PLUGIN_VERSION, PERMISSION_ALL)

// ═════════════════════════════════════════════════════════════════════════════
// Plugin Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::onLoad() {
    log("Loading Leo's Rocket Proximity Chat v" + std::string(PLUGIN_VERSION));

    registerCVars();
    initSubsystems();

    // ── Tick hook — fires every game tick for position updates ────────────
    gameWrapper->HookEvent(
        "Function TAGame.Car_TA.SetVehicleInput",
        std::bind(&LeoProximityChat::onTick, this, std::placeholders::_1)
    );

    // ── Match lifecycle hooks — cover ALL game modes ─────────────────────
    // Soccar (casual, competitive, extra modes)
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

    // Generic game event hooks (covers private matches, LAN, etc.)
    gameWrapper->HookEvent(
        "Function GameEvent_TA.Countdown.BeginState",
        std::bind(&LeoProximityChat::onMatchJoined, this, std::placeholders::_1)
    );

    // Freeplay detection
    gameWrapper->HookEvent(
        "Function TAGame.GameEvent_TrainingEditor_TA.StartPlayTest",
        std::bind(&LeoProximityChat::onMatchJoined, this, std::placeholders::_1)
    );
    gameWrapper->HookEvent(
        "Function TAGame.Mutator_Freeplay_TA.Init",
        std::bind(&LeoProximityChat::onMatchJoined, this, std::placeholders::_1)
    );

    // Leaving to main menu
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_MainMenu_TA.MainMenuAdded",
        std::bind(&LeoProximityChat::onMatchLeft, this, std::placeholders::_1)
    );

    // ── Notifier commands ────────────────────────────────────────────────
    cvarManager->registerNotifier("leo_proxchat_reconnect", [this](std::vector<std::string>) {
        log("Manual reconnect requested");
        disconnectFromServer();
        connectToServer();
    }, "Reconnect to proximity chat server", PERMISSION_ALL);

    cvarManager->registerNotifier("leo_proxchat_refresh_devices", [this](std::vector<std::string>) {
        if (audioEngine_) {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            cachedInputDevices_ = audioEngine_->getInputDevices();
            cachedOutputDevices_ = audioEngine_->getOutputDevices();
            lastDeviceRefresh_ = std::chrono::steady_clock::now();
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

    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.InitGame");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed");
    gameWrapper->UnhookEvent("Function GameEvent_TA.Countdown.BeginState");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_TrainingEditor_TA.StartPlayTest");
    gameWrapper->UnhookEvent("Function TAGame.Mutator_Freeplay_TA.Init");
    gameWrapper->UnhookEvent("Function TAGame.GFxData_MainMenu_TA.MainMenuAdded");
}

// ═════════════════════════════════════════════════════════════════════════════
// CVar Registration
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::registerCVars() {
    cvarManager->registerCvar("leo_proxchat_enabled", "1", "Enable proximity chat", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            enabled_ = cvar.getBoolValue();
            if (!enabled_) {
                if (audioEngine_) audioEngine_->stopStreams();
                disconnectFromServer();
            }
        });

    cvarManager->registerCvar("leo_proxchat_server_url",
        Protocol::DEFAULT_SERVER_URL, "Relay server URL");

    cvarManager->registerCvar("leo_proxchat_master_volume", "100", "Master volume", true, true, 0, true, 200)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setOutputVolume(cvar.getFloatValue() / 100.0f);
        });

    cvarManager->registerCvar("leo_proxchat_mic_volume", "100", "Microphone volume", true, true, 0, true, 300)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setMicVolume(cvar.getFloatValue() / 100.0f);
        });

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

    cvarManager->registerCvar("leo_proxchat_max_distance", "8000", "Maximum hearing distance", true, true, 500, true, 15000)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) {
                auto& sa = audioEngine_->getSpatialAudio();
                auto innerCvar = cvarManager->getCvar("leo_proxchat_full_vol_distance");
                if (innerCvar) sa.setDistanceParams(innerCvar.getFloatValue(), cvar.getFloatValue());
            }
        });

    cvarManager->registerCvar("leo_proxchat_full_vol_distance", "1500", "Full volume distance", true, true, 0, true, 5000)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) {
                auto& sa = audioEngine_->getSpatialAudio();
                auto outerCvar = cvarManager->getCvar("leo_proxchat_max_distance");
                if (outerCvar) sa.setDistanceParams(cvar.getFloatValue(), outerCvar.getFloatValue());
            }
        });

    cvarManager->registerCvar("leo_proxchat_3d_audio", "1", "Enable 3D spatial audio", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->getSpatialAudio().setEnabled(cvar.getBoolValue());
        });

    cvarManager->registerCvar("leo_proxchat_rolloff", "10", "Distance rolloff factor (1-20)", true, true, 1, true, 20)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) {
                auto& sa = audioEngine_->getSpatialAudio();
                auto innerCvar = cvarManager->getCvar("leo_proxchat_full_vol_distance");
                auto outerCvar = cvarManager->getCvar("leo_proxchat_max_distance");
                if (innerCvar && outerCvar)
                    sa.setDistanceParams(innerCvar.getFloatValue(), outerCvar.getFloatValue(), cvar.getFloatValue() / 10.0f);
            }
        });

    cvarManager->registerCvar("leo_proxchat_input_device", "-1", "Input audio device ID");
    cvarManager->registerCvar("leo_proxchat_output_device", "-1", "Output audio device ID");

    cvarManager->registerCvar("leo_proxchat_mic_muted", "0", "Mute microphone", true, true, 0, true, 1)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            if (audioEngine_) audioEngine_->setMicMuted(cvar.getBoolValue());
        });
}

void LeoProximityChat::applyCVarSettings() {
    if (!audioEngine_) return;

    auto getCvar = [this](const char* name) -> CVarWrapper { return cvarManager->getCvar(name); };

    auto enabledCvar = getCvar("leo_proxchat_enabled");
    if (enabledCvar) enabled_ = enabledCvar.getBoolValue();

    auto masterCvar = getCvar("leo_proxchat_master_volume");
    if (masterCvar) audioEngine_->setOutputVolume(masterCvar.getFloatValue() / 100.0f);

    auto micCvar = getCvar("leo_proxchat_mic_volume");
    if (micCvar) audioEngine_->setMicVolume(micCvar.getFloatValue() / 100.0f);

    auto pttCvar = getCvar("leo_proxchat_push_to_talk");
    if (pttCvar) audioEngine_->setPushToTalk(pttCvar.getBoolValue());

    auto threshCvar = getCvar("leo_proxchat_voice_threshold");
    if (threshCvar) audioEngine_->setVoiceThreshold(threshCvar.getFloatValue() / 100.0f);

    auto holdCvar = getCvar("leo_proxchat_hold_time");
    if (holdCvar) audioEngine_->setHoldTimeMs(holdCvar.getFloatValue());

    auto mutedCvar = getCvar("leo_proxchat_mic_muted");
    if (mutedCvar) audioEngine_->setMicMuted(mutedCvar.getBoolValue());

    auto& sa = audioEngine_->getSpatialAudio();
    auto spatialCvar = getCvar("leo_proxchat_3d_audio");
    if (spatialCvar) sa.setEnabled(spatialCvar.getBoolValue());

    auto innerCvar = getCvar("leo_proxchat_full_vol_distance");
    auto outerCvar = getCvar("leo_proxchat_max_distance");
    auto rollCvar  = getCvar("leo_proxchat_rolloff");
    if (innerCvar && outerCvar && rollCvar)
        sa.setDistanceParams(innerCvar.getFloatValue(), outerCvar.getFloatValue(), rollCvar.getFloatValue() / 10.0f);

    auto pttKeyCvar = getCvar("leo_proxchat_ptt_key");
    if (pttKeyCvar) pttKeyName_ = pttKeyCvar.getStringValue();

    auto inputCvar = getCvar("leo_proxchat_input_device");
    if (inputCvar && inputCvar.getIntValue() >= 0) audioEngine_->setInputDevice(inputCvar.getIntValue());

    auto outputCvar = getCvar("leo_proxchat_output_device");
    if (outputCvar && outputCvar.getIntValue() >= 0) audioEngine_->setOutputDevice(outputCvar.getIntValue());
}

// ═════════════════════════════════════════════════════════════════════════════
// Subsystem Init/Shutdown
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::initSubsystems() {
    if (subsystemsInitialized_) return;

    audioEngine_ = std::make_unique<AudioEngine>();
    if (!audioEngine_->initialize()) {
        logError("Audio engine failed to initialize: " + audioEngine_->getLastError());
    } else {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        cachedInputDevices_ = audioEngine_->getInputDevices();
        cachedOutputDevices_ = audioEngine_->getOutputDevices();
        lastDeviceRefresh_ = std::chrono::steady_clock::now();
    }

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

    networkManager_->setPeerJoinedCallback([this](const std::string& steamId, const std::string& name) {
        log("Peer joined: " + name + " (" + steamId + ")");
    });

    networkManager_->setPeerLeftCallback([this](const std::string& steamId, const std::string& name) {
        log("Peer left: " + name + " (" + steamId + ")");
    });

    networkManager_->setStateChangedCallback([this](NetworkManager::ConnectionState state, const std::string& info) {
        std::string stateStr;
        switch (state) {
            case NetworkManager::ConnectionState::Connected:     stateStr = "Connected"; break;
            case NetworkManager::ConnectionState::Disconnected:  stateStr = "Disconnected"; break;
            case NetworkManager::ConnectionState::Connecting:    stateStr = "Connecting"; break;
            case NetworkManager::ConnectionState::Reconnecting:  stateStr = "Reconnecting"; break;
            case NetworkManager::ConnectionState::Error:         stateStr = "Error"; break;
        }
        log("Network: " + stateStr + " - " + info);

        // When connected and in a match, join the room (must dispatch to game thread)
        if (state == NetworkManager::ConnectionState::Connected && inMatch_) {
            gameWrapper->Execute([this](GameWrapper*) {
                std::string matchId = getMatchId_GameThread();
                if (!matchId.empty()) {
                    networkManager_->joinRoom(matchId, getLocalPlayerName_GameThread(), getLocalSteamId_GameThread());
                }
            });
        }
    });

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
// Game Event Handlers (GAME THREAD)
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::onTick(std::string /*eventName*/) {
    if (!enabled_ || !inMatch_) return;

    // Use CAMERA position/rotation for 3D audio listener (not the car)
    Protocol::Vec3 camPos = getCameraPosition_GameThread();
    int camYaw = getCameraYaw_GameThread();

    // Use car position for the outgoing audio packet (other players hear you from your car)
    Protocol::Vec3 carPos = getLocalCarPosition_GameThread();

    if (audioEngine_) {
        audioEngine_->setListenerState(camPos, camYaw);
        audioEngine_->setLocalPosition(carPos);
    }

    // Refresh cached state for UI display (every tick is fine, it's cheap)
    refreshCachedGameState();

    // Auto-join room if connected but not yet in a room
    // (handles race conditions where connection happens after match join)
    if (networkManager_ && networkManager_->isConnected() &&
        networkManager_->getCurrentMatchId().empty()) {
        std::string matchId = getMatchId_GameThread();
        if (!matchId.empty()) {
            networkManager_->joinRoom(matchId, getLocalPlayerName_GameThread(), getLocalSteamId_GameThread());
            log("Auto-joined room: " + matchId);
        }
    }
}

void LeoProximityChat::onMatchJoined(std::string /*eventName*/) {
    if (!enabled_) return;
    if (inMatch_) return; // Avoid duplicate joins

    inMatch_ = true;
    log("Match detected - starting proximity chat");

    // Refresh cached state immediately
    refreshCachedGameState();

    // Start audio streams
    if (audioEngine_ && audioEngine_->isInitialized()) {
        if (!audioEngine_->isStreaming()) {
            if (!audioEngine_->startStreams()) {
                logError("Failed to start audio streams: " + audioEngine_->getLastError());
            }
        }
    }

    connectToServer();
}

void LeoProximityChat::onMatchLeft(std::string /*eventName*/) {
    if (!inMatch_) return;

    inMatch_ = false;
    log("Match ended - stopping proximity chat");

    if (audioEngine_) {
        audioEngine_->stopStreams();
    }

    if (networkManager_ && networkManager_->isConnected()) {
        networkManager_->leaveRoom();
    }

    // Clear cached match state
    {
        std::lock_guard<std::mutex> lock(cachedStateMutex_);
        cachedMatchId_.clear();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Network Connection
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::connectToServer() {
    if (!networkManager_) return;

    auto urlCvar = cvarManager->getCvar("leo_proxchat_server_url");
    std::string serverUrl = urlCvar ? urlCvar.getStringValue() : Protocol::DEFAULT_SERVER_URL;
    if (serverUrl.empty()) serverUrl = Protocol::DEFAULT_SERVER_URL;

    if (!networkManager_->isConnected()) {
        networkManager_->connect(serverUrl);
    }

    // Join room if already connected
    if (networkManager_->isConnected()) {
        std::string matchId = getMatchId_GameThread();
        if (!matchId.empty()) {
            networkManager_->joinRoom(matchId, getLocalPlayerName_GameThread(), getLocalSteamId_GameThread());
        }
    }
}

void LeoProximityChat::disconnectFromServer() {
    if (networkManager_) {
        networkManager_->disconnect();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Game State Helpers (GAME THREAD ONLY — never call from UI/render)
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::refreshCachedGameState() {
    // Called from game thread (onTick / onMatchJoined)
    std::lock_guard<std::mutex> lock(cachedStateMutex_);
    cachedMatchId_    = getMatchId_GameThread();
    cachedPlayerName_ = getLocalPlayerName_GameThread();
    cachedSteamId_    = getLocalSteamId_GameThread();
    cachedCarPos_     = getLocalCarPosition_GameThread();
    cachedCarYaw_     = getLocalCarYaw_GameThread();
}

std::string LeoProximityChat::getMatchId_GameThread() const {
    if (!gameWrapper) return "leo_global";

    // Try online game first
    ServerWrapper server = gameWrapper->GetOnlineGame();
    if (!server) {
        // Try generic current game state (freeplay, private, etc.)
        server = gameWrapper->GetCurrentGameState();
    }
    if (!server) return "leo_global";

    // Use match GUID if available (most reliable for online)
    std::string matchGUID;
    try { matchGUID = server.GetMatchGUID(); } catch (...) {}

    if (!matchGUID.empty() && matchGUID != "No Match GUID" && matchGUID != "0") {
        return matchGUID;
    }

    // Build a deterministic room from sorted player UIDs so all players in the
    // same match end up in the same room regardless of who joins first
    try {
        auto players = server.GetPRIs();
        if (players.Count() > 1) {
            std::vector<std::string> uids;
            for (int i = 0; i < players.Count(); i++) {
                auto pri = players.Get(i);
                if (pri) {
                    try {
                        auto uid = pri.GetUniqueIdWrapper();
                        uint64_t id = uid.GetUID();
                        if (id != 0) uids.push_back(std::to_string(id));
                    } catch (...) {}
                }
            }
            if (uids.size() > 1) {
                std::sort(uids.begin(), uids.end());
                std::string combined;
                for (auto& u : uids) combined += u + "_";
                return "rl_private_" + combined;
            }
        }
    } catch (...) {}

    // Final fallback: a single global room — everyone on this server
    // can hear each other. This is the safest option for private servers.
    return "leo_global";
}

std::string LeoProximityChat::getLocalSteamId_GameThread() const {
    if (!gameWrapper) return generateUniqueId();
    try {
        auto uid = gameWrapper->GetUniqueID();
        uint64_t id = uid.GetUID();
        if (id != 0) return std::to_string(id);

        // GetUID() returned 0 — try getting it from the player controller PRI
        auto pc = gameWrapper->GetPlayerController();
        if (pc) {
            auto pri = pc.GetPRI();
            if (pri) {
                auto priUid = pri.GetUniqueIdWrapper();
                uint64_t priId = priUid.GetUID();
                if (priId != 0) return std::to_string(priId);
            }
        }
    } catch (...) {}

    // Last resort: generate a persistent random ID for this session
    return generateUniqueId();
}

std::string LeoProximityChat::generateUniqueId() {
    // Generate once per process lifetime — persistent across reconnects
    static std::string cachedId;
    if (cachedId.empty()) {
        // Use a combination of time + random for uniqueness
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        cachedId = "leo_" + std::to_string(ms % 999999999) + "_" + std::to_string(rand() % 99999);
    }
    return cachedId;
}

std::string LeoProximityChat::getLocalPlayerName_GameThread() const {
    if (!gameWrapper) return "Unknown";
    try {
        auto pc = gameWrapper->GetPlayerController();
        if (!pc) return "Unknown";
        auto pri = pc.GetPRI();
        if (!pri) return "Unknown";
        return pri.GetPlayerName().ToString();
    } catch (...) { return "Unknown"; }
}

Protocol::Vec3 LeoProximityChat::getLocalCarPosition_GameThread() const {
    if (!gameWrapper) return {};
    try {
        auto car = gameWrapper->GetLocalCar();
        if (!car) return {};
        Vector loc = car.GetLocation();
        return { loc.X, loc.Y, loc.Z };
    } catch (...) { return {}; }
}

int LeoProximityChat::getLocalCarYaw_GameThread() const {
    if (!gameWrapper) return 0;
    try {
        auto car = gameWrapper->GetLocalCar();
        if (!car) return 0;
        Rotator rot = car.GetRotation();
        return rot.Yaw;
    } catch (...) { return 0; }
}

Protocol::Vec3 LeoProximityChat::getCameraPosition_GameThread() const {
    if (!gameWrapper) return getLocalCarPosition_GameThread();
    try {
        CameraWrapper cam = gameWrapper->GetCamera();
        if (cam.IsNull()) {
            return getLocalCarPosition_GameThread();
        }
        // GetPOV() returns the exact rendered camera viewpoint,
        // works correctly for ballcam, freecam, replays, etc.
        POV pov = cam.GetPOV();
        return { pov.location.X, pov.location.Y, pov.location.Z };
    } catch (...) {
        return getLocalCarPosition_GameThread();
    }
}

int LeoProximityChat::getCameraYaw_GameThread() const {
    if (!gameWrapper) return getLocalCarYaw_GameThread();
    try {
        CameraWrapper cam = gameWrapper->GetCamera();
        if (cam.IsNull()) {
            return getLocalCarYaw_GameThread();
        }
        POV pov = cam.GetPOV();
        return pov.rotation.Yaw;
    } catch (...) {
        return getLocalCarYaw_GameThread();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Settings UI (ImGui) — runs on RENDER thread, NO gameWrapper access
// ═════════════════════════════════════════════════════════════════════════════

void LeoProximityChat::SetImGuiContext(uintptr_t ctx) {
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

void LeoProximityChat::RenderSettings() {
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Leo's Rocket Proximity Chat v%s", PLUGIN_VERSION);
    ImGui::Separator();

    // Enable toggle — use CVar directly (thread-safe in BakkesMod)
    auto enabledCvar = cvarManager->getCvar("leo_proxchat_enabled");
    if (!enabledCvar) return;

    bool isEnabled = enabledCvar.getBoolValue();
    if (ImGui::Checkbox("Enable Proximity Chat", &isEnabled)) {
        enabledCvar.setValue(isEnabled);
    }

    if (!isEnabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Plugin is disabled");
        return;
    }

    ImGui::Spacing();

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

    // Refresh devices periodically (guarded by mutex)
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        bool needRefresh = std::chrono::duration_cast<std::chrono::seconds>(now - lastDeviceRefresh_).count() > 10;
        if (needRefresh && audioEngine_ && audioEngine_->isInitialized()) {
            cachedInputDevices_ = audioEngine_->getInputDevices();
            cachedOutputDevices_ = audioEngine_->getOutputDevices();
            lastDeviceRefresh_ = now;
        }
    }

    // Input device combo
    auto inputCvar = cvarManager->getCvar("leo_proxchat_input_device");
    if (inputCvar) {
        int inputId = inputCvar.getIntValue();
        ImGui::Text("Microphone:");
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            renderDeviceCombo("##InputDevice", inputId, cachedInputDevices_);
        }
        if (inputId != inputCvar.getIntValue()) {
            inputCvar.setValue(inputId);
            // Schedule device change on game thread to avoid racing
            gameWrapper->Execute([this, inputId](GameWrapper*) {
                if (audioEngine_) audioEngine_->setInputDevice(inputId);
            });
        }
    }

    // Output device combo
    auto outputCvar = cvarManager->getCvar("leo_proxchat_output_device");
    if (outputCvar) {
        int outputId = outputCvar.getIntValue();
        ImGui::Text("Speakers/Headphones:");
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            renderDeviceCombo("##OutputDevice", outputId, cachedOutputDevices_);
        }
        if (outputId != outputCvar.getIntValue()) {
            outputCvar.setValue(outputId);
            gameWrapper->Execute([this, outputId](GameWrapper*) {
                if (audioEngine_) audioEngine_->setOutputDevice(outputId);
            });
        }
    }

    if (ImGui::Button("Refresh Devices")) {
        cvarManager->executeCommand("leo_proxchat_refresh_devices");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Volume");

    auto masterCvar = cvarManager->getCvar("leo_proxchat_master_volume");
    if (masterCvar) {
        float masterVal = masterCvar.getFloatValue();
        if (ImGui::SliderFloat("Master Volume", &masterVal, 0.0f, 200.0f, "%.0f%%")) {
            masterCvar.setValue(masterVal);
        }
    }

    auto micCvar = cvarManager->getCvar("leo_proxchat_mic_volume");
    if (micCvar) {
        float micVal = micCvar.getFloatValue();
        if (ImGui::SliderFloat("Mic Volume", &micVal, 0.0f, 300.0f, "%.0f%%")) {
            micCvar.setValue(micVal);
        }
    }

    auto mutedCvar = cvarManager->getCvar("leo_proxchat_mic_muted");
    if (mutedCvar) {
        bool muted = mutedCvar.getBoolValue();
        if (ImGui::Checkbox("Mute Microphone", &muted)) {
            mutedCvar.setValue(muted);
        }
    }

    // Mic level meter (atomic reads, safe from any thread)
    if (audioEngine_) {
        float level = audioEngine_->getCurrentInputLevel();
        ImGui::Text("Mic Level:");
        ImGui::SameLine();
        ImGui::ProgressBar(std::min(level * 10.0f, 1.0f), ImVec2(-1, 0),
                           audioEngine_->isSpeaking() ? "SPEAKING" : "");
    }
}

void LeoProximityChat::renderVoiceSettings() {
    ImGui::Text("Voice Activation");
    ImGui::Separator();

    auto pttCvar = cvarManager->getCvar("leo_proxchat_push_to_talk");
    if (!pttCvar) return;

    bool ptt = pttCvar.getBoolValue();
    if (ImGui::Checkbox("Push to Talk", &ptt)) {
        pttCvar.setValue(ptt);
    }

    if (ptt) {
        auto pttKeyCvar = cvarManager->getCvar("leo_proxchat_ptt_key");
        if (pttKeyCvar) {
            std::string key = pttKeyCvar.getStringValue();
            char keyBuf[32] = {};
            strncpy_s(keyBuf, key.c_str(), sizeof(keyBuf) - 1);
            ImGui::Text("PTT Key:");
            ImGui::SameLine();
            if (ImGui::InputText("##PTTKey", keyBuf, sizeof(keyBuf))) {
                pttKeyCvar.setValue(std::string(keyBuf));
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Bind in console: bind %s \"leo_proxchat_ptt_pressed\"", keyBuf);
        }
    } else {
        ImGui::Text("Open Mic Settings");
        ImGui::Spacing();

        auto threshCvar = cvarManager->getCvar("leo_proxchat_voice_threshold");
        if (threshCvar) {
            float thresh = threshCvar.getFloatValue();
            if (ImGui::SliderFloat("Voice Threshold", &thresh, 0.0f, 100.0f, "%.1f")) {
                threshCvar.setValue(thresh);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Lower = more sensitive. Increase if transmitting background noise.");
        }

        auto holdCvar = cvarManager->getCvar("leo_proxchat_hold_time");
        if (holdCvar) {
            float hold = holdCvar.getFloatValue();
            if (ImGui::SliderFloat("Hold Time (ms)", &hold, 0.0f, 2000.0f, "%.0f ms")) {
                holdCvar.setValue(hold);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "How long to keep transmitting after voice stops.");
        }
    }
}

void LeoProximityChat::renderProximitySettings() {
    ImGui::Text("3D Proximity Audio");
    ImGui::Separator();

    auto spatialCvar = cvarManager->getCvar("leo_proxchat_3d_audio");
    if (!spatialCvar) return;

    bool spatial = spatialCvar.getBoolValue();
    if (ImGui::Checkbox("Enable 3D Spatial Audio", &spatial)) {
        spatialCvar.setValue(spatial);
    }

    if (spatial) {
        auto maxDistCvar = cvarManager->getCvar("leo_proxchat_max_distance");
        if (maxDistCvar) {
            float maxDist = maxDistCvar.getFloatValue();
            if (ImGui::SliderFloat("Max Hearing Distance", &maxDist, 500.0f, 15000.0f, "%.0f uu")) {
                maxDistCvar.setValue(maxDist);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Beyond this distance, you won't hear the player. (Field ~10240 uu long)");
        }

        auto fullDistCvar = cvarManager->getCvar("leo_proxchat_full_vol_distance");
        if (fullDistCvar) {
            float fullDist = fullDistCvar.getFloatValue();
            if (ImGui::SliderFloat("Full Volume Distance", &fullDist, 0.0f, 5000.0f, "%.0f uu")) {
                fullDistCvar.setValue(fullDist);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Within this distance, voice is at full volume.");
        }

        auto rolloffCvar = cvarManager->getCvar("leo_proxchat_rolloff");
        if (rolloffCvar) {
            float rolloff = rolloffCvar.getFloatValue();
            if (ImGui::SliderFloat("Rolloff Curve", &rolloff, 1.0f, 20.0f, "%.1f")) {
                rolloffCvar.setValue(rolloff);
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Higher = sharper volume dropoff with distance.");
        }
    }
}

void LeoProximityChat::renderNetworkSettings() {
    ImGui::Text("Server Connection");
    ImGui::Separator();

    auto urlCvar = cvarManager->getCvar("leo_proxchat_server_url");
    if (urlCvar) {
        std::string url = urlCvar.getStringValue();
        char urlBuf[256] = {};
        strncpy_s(urlBuf, url.c_str(), sizeof(urlBuf) - 1);
        ImGui::Text("Server URL:");
        if (ImGui::InputText("##ServerURL", urlBuf, sizeof(urlBuf))) {
            urlCvar.setValue(std::string(urlBuf));
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Reconnect")) {
        // Dispatch to game thread
        gameWrapper->Execute([this](GameWrapper*) {
            disconnectFromServer();
            connectToServer();
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        gameWrapper->Execute([this](GameWrapper*) {
            disconnectFromServer();
        });
    }

    // Connection info (read from network manager, atomic/mutex-protected)
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

    // Plugin state (atomics, safe)
    ImGui::Text("Plugin: %s", enabled_.load() ? "Enabled" : "Disabled");
    ImGui::Text("In Match: %s", inMatch_.load() ? "Yes" : "No");

    // Audio state (atomic reads, safe)
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

    // Network state (atomic/mutex reads, safe)
    if (networkManager_) {
        ImGui::Spacing();
        ImGui::Text("Network: %s", networkManager_->getStateString().c_str());
        ImGui::Text("Sent: %.1f KB", networkManager_->getBytesSent() / 1024.0f);
        ImGui::Text("Received: %.1f KB", networkManager_->getBytesReceived() / 1024.0f);

        auto peers = networkManager_->getConnectedPeers();
        ImGui::Spacing();
        ImGui::Text("Connected Peers (%zu):", peers.size());
        for (const auto& peer : peers) {
            ImGui::BulletText("%s (%s)", peer.playerName.c_str(), peer.steamId.c_str());
        }
    }

    // Local player info — READ FROM CACHE (safe from UI thread)
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Local Player");
    {
        std::lock_guard<std::mutex> lock(cachedStateMutex_);
        ImGui::Text("Name: %s", cachedPlayerName_.c_str());
        ImGui::Text("Steam ID: %s", cachedSteamId_.c_str());
        ImGui::Text("Position: (%.0f, %.0f, %.0f)", cachedCarPos_.x, cachedCarPos_.y, cachedCarPos_.z);
        ImGui::Text("Match ID: %s", cachedMatchId_.empty() ? "(none)" : cachedMatchId_.c_str());
    }
}

void LeoProximityChat::renderDeviceCombo(
    const char* label, int& currentId,
    const std::vector<AudioEngine::DeviceInfo>& devices)
{
    std::string preview = "Default";
    for (const auto& dev : devices) {
        if (dev.id == currentId) {
            preview = dev.name;
            if (dev.isDefault) preview += " (Default)";
            break;
        }
    }

    if (ImGui::BeginCombo(label, preview.c_str())) {
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
