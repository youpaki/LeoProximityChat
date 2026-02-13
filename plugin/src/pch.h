#pragma once

// ─── Windows ─────────────────────────────────────────────────────────────────
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// ─── C++ Standard Library ───────────────────────────────────────────────────
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <functional>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <queue>
#include <optional>
#include <sstream>
#include <numeric>

// ─── BakkesMod SDK ──────────────────────────────────────────────────────────
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

// ─── Third-party ────────────────────────────────────────────────────────────
#include <portaudio.h>
#include <opus/opus.h>
#include <nlohmann/json.hpp>
