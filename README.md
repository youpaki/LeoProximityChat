# Leo's Rocket Proximity Chat

A BakkesMod plugin for Rocket League that adds **proximity voice chat** with full **3D spatial audio**. Hear other players who have the plugin installed — their voice is positioned in 3D space based on their car's location relative to yours.

## Download

- **Plugin**: Download from [BakkesPlugins](https://bakkesplugins.com/) or grab the latest [Release](https://github.com/youpaki/LeoProximityChat/releases)
- **Server**: Download the relay server exe from [LeoProxChatServer Releases](https://github.com/youpaki/LeoProxChatServer/releases)

## Installation

1. Install [BakkesMod](https://bakkesmod.com/)
2. Download `LeoProximityChat.dll` and place it in `%APPDATA%\bakkesmod\bakkesmod\plugins\`
3. Download `leo_proximity_chat.set` and place it in `%APPDATA%\bakkesmod\bakkesmod\plugins\settings\`
4. Launch Rocket League → Open BakkesMod console (F6) → Type `plugin load LeoProximityChat`
5. Someone needs to host the relay server (see [Server Setup](#server-setup))

---

## Features

| Feature | Details |
|---|---|
| **Proximity Voice Chat** | Hear players near you; volume fades with distance |
| **3D Binaural Spatial Audio** | Full HRTF rendering with ITD, ILD, head shadow |
| **Doppler Effect** | Smooth pitch shifting when cars approach/recede |
| **Reverb Engine** | Schroeder reverb with early reflections (stadium-like) |
| **Camera-Based Listener** | Audio follows your camera POV (ballcam/freecam) |
| **Distance Attenuation** | Configurable inner/outer radius with smooth rolloff |
| **Air Absorption** | High-frequency rolloff over distance for realism |
| **Push-to-Talk / Open Mic** | Choose your preferred mode |
| **Voice Activity Detection** | Adjustable sensitivity + hold time |
| **Device Selection** | Choose microphone and output device in settings |
| **Opus Codec** | Low-latency voice encoding (32 kbps, 48 kHz) |
| **Forward Error Correction** | Opus FEC handles packet loss gracefully |
| **Auto-Reconnect** | WebSocket auto-reconnects if connection drops |
| **Per-Player Audio** | Independent decoder + spatial processor per peer |
| **ImGui Settings UI** | Full tabbed settings panel inside BakkesMod |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     BakkesMod Plugin (DLL)                      │
│                                                                 │
│  ┌──────────┐   ┌──────────┐   ┌──────────────┐   ┌─────────┐ │
│  │  Audio    │──▶│  Opus    │──▶│   Network    │──▶│  Relay  │ │
│  │  Engine   │   │  Codec   │   │   Manager    │   │  Server │ │
│  │(PortAudio)│◀──│(Encoder/ │◀──│ (WebSocket)  │◀──│ (Node)  │ │
│  │          │   │ Decoder) │   │              │   │         │ │
│  └──────────┘   └──────────┘   └──────────────┘   └─────────┘ │
│       │                                                        │
│       ▼                                                        │
│  ┌──────────┐                                                  │
│  │ Spatial   │  Camera POV from BakkesMod                      │
│  │  Audio    │◀──────────────────────────────────────────────── │
│  │ (HRTF+   │                                                  │
│  │ Doppler) │                                                  │
│  └──────────┘                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Server Setup

The relay server groups players by match ID. You need ONE server for your group.

### Option A: Standalone Exe (easiest)

1. Download `LeoProxChatServer.exe` from [Releases](https://github.com/youpaki/LeoProxChatServer/releases)
2. Double-click to run (listens on port `9587`)
3. Set plugin server URL to `ws://YOUR_IP:9587`

### Option B: Node.js

```bash
git clone https://github.com/youpaki/LeoProxChatServer.git
cd LeoProxChatServer
npm install
npm start
```

### Option C: VPS / Cloud

See the [Server README](https://github.com/youpaki/LeoProxChatServer) for production deployment with PM2, Docker, and nginx/SSL.

---

## Project Structure

```
├── plugin/                     # BakkesMod plugin (C++)
│   ├── CMakeLists.txt          # Build configuration
│   ├── vcpkg.json              # C++ dependencies
│   ├── src/
│   │   ├── pch.h/cpp           # Precompiled header
│   │   ├── version.h           # Version constants
│   │   ├── Protocol.h          # Network protocol & shared types
│   │   ├── ThreadSafeQueue.h   # Lock-based bounded queue
│   │   ├── VoiceCodec.h/cpp    # Opus encoder/decoder wrapper
│   │   ├── AudioEngine.h/cpp   # PortAudio I/O, VAD, mixing
│   │   ├── SpatialAudio.h/cpp  # HRTF binaural, Doppler, reverb
│   │   ├── NetworkManager.h/cpp# WebSocket client, room management
│   │   └── LeoProximityChat.h/cpp # Main plugin class
│   └── settings/
│       └── leo_proximity_chat.set  # BakkesMod settings UI
│
├── server/                     # Relay server (Node.js)
│   ├── server.js               # WebSocket relay with room management
│   ├── package.json
│   └── .env.example            # Configuration template
│
└── README.md
```

---

## Building from Source

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2019/2022 | With C++ desktop workload | MSVC v142+ |
| CMake | 3.20+ | |
| vcpkg | Latest | C++ package manager |
| BakkesMod SDK | Latest | Auto-detected at `%APPDATA%\bakkesmod\bakkesmod\bakkesmodsdk` |

### Build Steps

```powershell
# 1. Install vcpkg dependencies (x64-windows-static)
vcpkg install portaudio:x64-windows-static opus:x64-windows-static ixwebsocket:x64-windows-static nlohmann-json:x64-windows-static

# 2. Build
cd plugin
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" -G "Visual Studio 16 2019" -A x64
cmake --build . --config Release
```

The DLL is output to `plugin/build/bin/Release/LeoProximityChat.dll`.

---

## Configuration

### In-Game Settings (BakkesMod F2 Menu)

Open BakkesMod menu (F2) → Plugins → Leo's Rocket Proximity Chat

| Tab | Setting | Description |
|---|---|---|
| **Audio** | Master Volume | Output volume (0-200%) |
| | Mic Volume | Microphone gain (0-300%) |
| | Mute Microphone | Toggle mic mute |
| | Input/Output Device | Select audio devices |
| **Voice** | Push to Talk | Enable PTT mode |
| | PTT Key | Key binding for PTT |
| | Voice Threshold | Open mic sensitivity (0-100) |
| | Hold Time | How long to keep transmitting after voice stops |
| **Proximity** | 3D Spatial Audio | Enable/disable 3D positioning |
| | Max Hearing Distance | Beyond this, silence (default: 15000 uu) |
| | Full Volume Distance | Within this, full volume (default: 2500 uu) |
| | Rolloff Curve | Volume dropoff sharpness |
| **Network** | Server URL | Relay server WebSocket URL |
| | Reconnect | Force reconnect |

### Console Commands

```
leo_proxchat_enabled 1
leo_proxchat_server_url ws://your-server:9587
leo_proxchat_reconnect
leo_proxchat_master_volume 100
leo_proxchat_mic_volume 100
leo_proxchat_push_to_talk 1
leo_proxchat_3d_audio 1
leo_proxchat_max_distance 15000
```

---

## Technical Details

### Audio Pipeline
- **Sample Rate**: 48,000 Hz
- **Frame Size**: 960 samples (20ms)
- **Codec**: Opus VOIP mode, 32 kbps, complexity 5
- **FEC**: Inband forward error correction

### 3D Spatial Audio
- **HRTF**: Binaural rendering with Woodworth ITD and frequency-dependent ILD
- **Head Shadow**: Two-pole filter modeling head obstruction (2-16kHz range)
- **Doppler**: Variable-rate delay line with smooth pitch shifting
- **Reverb**: Schroeder engine (4 comb + 2 allpass + 6 early reflections)
- **Air Absorption**: Distance-dependent high-frequency rolloff
- **Listener**: Camera POV (supports ballcam/freecam)

### Network Protocol
```
Audio: Client → Server (13+N bytes): [0x03][pos:3×f32le][opus_data]
Relay: Server → Client (21+N bytes): [0x03][sender:u64le][pos:3×f32le][opus_data]
Control: JSON text frames (join, leave, position, ping)
```

---

## Troubleshooting

| Issue | Solution |
|---|---|
| No audio output | Check output device; ensure relay server is running |
| Can't hear others | Both players need the plugin + same server |
| High latency | Use a geographically close relay server |
| Echo/feedback | Use headphones |
| Mic not working | Check input device selection in settings |
| Connection failed | Verify server URL; check firewall port 9587 |
| Plugin won't load | Ensure DLL is in `%APPDATA%\bakkesmod\bakkesmod\plugins\` |

---

## License

MIT License — see [LICENSE](LICENSE)
