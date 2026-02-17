#pragma once
#include "pch.h"
#include "version.h"
#include "AudioEngine.h"
#include "NetworkManager.h"
#include "SpatialAudio.h"
#include "Protocol.h"

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <string>

/**
 * Leo's Rocket Proximity Chat — Main BakkesMod Plugin
 *
 * All gameWrapper access is done on the game thread (hooks/callbacks).
 * The ImGui UI only reads cached values protected by mutex.
 */
class LeoProximityChat
    : public BakkesMod::Plugin::BakkesModPlugin
    , public BakkesMod::Plugin::PluginSettingsWindow
{
public:
    void onLoad() override;
    void onUnload() override;

    std::string GetPluginName() override { return "Leo's Rocket Proximity Chat"; }
    void SetImGuiContext(uintptr_t ctx) override;
    void RenderSettings() override;

private:
    // ── Game event handlers (game thread only) ───────────────────────────
    void onTick(std::string eventName);
    void onMatchJoined(std::string eventName);
    void onMatchLeft(std::string eventName);

    void registerCVars();
    void applyCVarSettings();

    void initSubsystems();
    void shutdownSubsystems();
    void connectToServer();
    void disconnectFromServer();

    // ── Game state helpers (game thread ONLY) ────────────────────────────
    std::string getMatchId_GameThread() const;
    std::string getLocalSteamId_GameThread() const;
    static std::string generateUniqueId();
    std::string getLocalPlayerName_GameThread() const;
    Protocol::Vec3 getLocalCarPosition_GameThread() const;
    int getLocalCarYaw_GameThread() const;
    Protocol::Vec3 getCameraPosition_GameThread() const;
    int getCameraYaw_GameThread() const;
    void refreshCachedGameState();

    // ── ImGui helpers ────────────────────────────────────────────────────
    void renderAudioSettings();
    void renderVoiceSettings();
    void renderProximitySettings();
    void renderNetworkSettings();
    void renderStatusPanel();
    void renderDeviceCombo(const char* label, int& currentId,
                           const std::vector<AudioEngine::DeviceInfo>& devices);

    // ── Subsystems ───────────────────────────────────────────────────────
    std::unique_ptr<AudioEngine>    audioEngine_;
    std::unique_ptr<NetworkManager> networkManager_;

    // ── State ────────────────────────────────────────────────────────────
    std::atomic<bool> enabled_{true};
    std::atomic<bool> inMatch_{false};
    bool subsystemsInitialized_ = false;

    // ── Cached game state (written game thread, read UI thread) ──────────
    mutable std::mutex cachedStateMutex_;
    std::string cachedMatchId_;
    std::string cachedPlayerName_ = "Unknown";
    std::string cachedSteamId_ = "0";
    Protocol::Vec3 cachedCarPos_{};
    int cachedCarYaw_ = 0;

    // ── Cached device lists for UI ───────────────────────────────────────
    mutable std::mutex deviceMutex_;
    std::vector<AudioEngine::DeviceInfo> cachedInputDevices_;
    std::vector<AudioEngine::DeviceInfo> cachedOutputDevices_;
    std::chrono::steady_clock::time_point lastDeviceRefresh_;

    // ── PTT key state ────────────────────────────────────────────────────
    std::string pttKeyName_ = "F3";
    bool pttKeyDown_ = false;

    // ── Logging helper ───────────────────────────────────────────────────
    void log(const std::string& msg) const;
    void logError(const std::string& msg) const;
};
