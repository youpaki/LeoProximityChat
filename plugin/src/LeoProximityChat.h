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
#include <string>

/**
 * Leo's Rocket Proximity Chat — Main BakkesMod Plugin
 *
 * Integrates audio engine, network manager and spatial audio with the
 * Rocket League game state via BakkesMod hooks.
 *
 * Features:
 *   - Automatic match detection and server room joining
 *   - Per-tick position updates for 3D audio
 *   - Push-to-talk / open-mic with VAD
 *   - Device selection (input/output)
 *   - Full settings UI via BakkesMod .set file
 *   - Auto-reconnect on connection loss
 *   - Graceful degradation if audio/network fails
 */
class LeoProximityChat
    : public BakkesMod::Plugin::BakkesModPlugin
    , public BakkesMod::Plugin::PluginSettingsWindow
{
public:
    // ── Plugin interface ─────────────────────────────────────────────────
    void onLoad() override;
    void onUnload() override;

    // ── Settings window (PluginSettingsWindow) ───────────────────────────
    std::string GetPluginName() override { return "Leo's Rocket Proximity Chat"; }
    void SetImGuiContext(uintptr_t ctx) override;
    void RenderSettings() override;

private:
    // ── Game event handlers ──────────────────────────────────────────────
    void onTick(std::string eventName);
    void onMatchJoined(std::string eventName);
    void onMatchLeft(std::string eventName);

    // ── CVar registration ────────────────────────────────────────────────
    void registerCVars();
    void applyCVarSettings();

    // ── Audio/Network wiring ─────────────────────────────────────────────
    void initSubsystems();
    void shutdownSubsystems();
    void connectToServer();
    void disconnectFromServer();

    // ── Helpers ──────────────────────────────────────────────────────────
    std::string getMatchId() const;
    std::string getLocalSteamId() const;
    std::string getLocalPlayerName() const;
    Protocol::Vec3 getLocalCarPosition() const;
    int getLocalCarYaw() const;

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

    // ── CVars ────────────────────────────────────────────────────────────
    // These are managed by BakkesMod's CVar system
    std::shared_ptr<bool>   cvarEnabled_;
    std::shared_ptr<std::string> cvarServerUrl_;
    std::shared_ptr<float>  cvarMasterVolume_;
    std::shared_ptr<float>  cvarMicVolume_;
    std::shared_ptr<bool>   cvarPushToTalk_;
    std::shared_ptr<std::string> cvarPTTKey_;
    std::shared_ptr<float>  cvarVoiceThreshold_;
    std::shared_ptr<float>  cvarHoldTime_;
    std::shared_ptr<float>  cvarMaxDistance_;
    std::shared_ptr<float>  cvarFullVolDistance_;
    std::shared_ptr<bool>   cvarSpatialEnabled_;
    std::shared_ptr<bool>   cvarMicMuted_;
    std::shared_ptr<int>    cvarInputDevice_;
    std::shared_ptr<int>    cvarOutputDevice_;
    std::shared_ptr<float>  cvarRolloff_;

    // ── Cached device lists for UI ───────────────────────────────────────
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
