# Leo's Rocket Proximity Chat

A BakkesMod plugin for Rocket League that adds **proximity voice chat** with full **3D spatial audio**. Hear other players who have the plugin installed — their voice is positioned in 3D space based on their car's location relative to yours.

---

## Features

| Feature | Details |
|---|---|
| **Proximity Voice Chat** | Hear players near you; volume fades with distance |
| **3D Spatial Audio** | Sound is panned left/right based on car positions with equal-power panning |
| **Distance Attenuation** | Configurable inner (full volume) and outer (silence) radius with smooth rolloff |
| **Low-Pass Distance Filter** | Far-away voices sound slightly muffled for realism |
| **Push-to-Talk / Open Mic** | Choose your preferred mode |
| **Voice Activity Detection** | Adjustable sensitivity + hold time to avoid cutting off mid-sentence |
| **Device Selection** | Choose your microphone and output device from the settings UI |
| **Opus Codec** | Low-latency, bandwidth-efficient voice encoding (32 kbps) |
| **Forward Error Correction** | Opus FEC handles packet loss gracefully |
| **Packet Loss Concealment** | Smooth audio even when packets are dropped |
| **Auto-Reconnect** | WebSocket reconnects automatically if connection drops |
| **Per-Player Decoders** | Each peer gets independent Opus decoder + spatial processor |
| **Rear Attenuation** | Subtle volume reduction for sounds behind you |
| **Soft Clipping** | `tanh` saturation prevents harsh audio clipping when many peers are mixed |
| **ImGui Settings UI** | Full tabbed settings panel inside BakkesMod |
| **Fallback .set File** | Basic settings accessible even without the ImGui window |

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
│  │ Spatial   │  Position data from BakkesMod game hooks        │
│  │  Audio    │◀──────────────────────────────────────────────── │
│  │ (3D Mix)  │                                                 │
│  └──────────┘                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Data Flow

```
Microphone → PortAudio Capture → Opus Encode → WebSocket Send → Relay Server
                                                                      │
Relay Server → WebSocket Receive → Opus Decode → 3D Spatialize → Mix → Speakers
```

### Networking

The plugin uses a **lightweight WebSocket relay server** (Node.js). Players are grouped by match ID — when you join a Rocket League match, the plugin connects to the relay and joins a "room" identified by the match GUID. Audio packets are relayed only to other players in the same room.

**Why not P2P?** Rocket League doesn't expose player IP addresses to plugins. A relay server solves discovery and connectivity cleanly.

---

## Project Structure

```
├── server/                     # Relay server (Node.js)
│   ├── package.json
│   ├── server.js               # WebSocket relay with room management
│   └── .env                    # Server configuration
│
└── plugin/                     # BakkesMod plugin (C++)
    ├── CMakeLists.txt          # Build configuration
    ├── vcpkg.json              # C++ dependencies
    ├── src/
    │   ├── pch.h/cpp           # Precompiled header
    │   ├── version.h           # Version constants
    │   ├── Protocol.h          # Network protocol & shared types
    │   ├── ThreadSafeQueue.h   # Lock-based bounded queue
    │   ├── VoiceCodec.h/cpp    # Opus encoder/decoder wrapper
    │   ├── AudioEngine.h/cpp   # PortAudio I/O, VAD, mixing
    │   ├── SpatialAudio.h/cpp  # 3D panning, distance attenuation
    │   ├── NetworkManager.h/cpp# WebSocket client, room management
    │   └── LeoProximityChat.h/cpp # Main plugin class
    └── settings/
        └── leo_proximity_chat.set  # BakkesMod settings UI
```

---

## Building

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 | 17.x+ | With C++ desktop workload |
| CMake | 3.20+ | |
| vcpkg | Latest | For C++ dependencies |
| Node.js | 18+ | For relay server |
| BakkesMod SDK | Latest | See below |

### 1. Install BakkesMod SDK

The SDK is typically at:
```
%APPDATA%\bakkesmod\bakkesmod\bakkesmodsdk
```

If not, download it from the [BakkesMod website](https://www.bakkesmod.com/) or the [BakkesMod SDK repo](https://github.com/bakkesmodorg/BakkesModSDK).

### 2. Install vcpkg Dependencies

```powershell
# Clone vcpkg if you haven't
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

# Install dependencies (x64 Windows)
.\vcpkg install portaudio:x64-windows
.\vcpkg install opus:x64-windows
.\vcpkg install ixwebsocket:x64-windows
.\vcpkg install nlohmann-json:x64-windows
```

### 3. Build the Plugin

```powershell
cd plugin
mkdir build && cd build

cmake .. -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" ^
         -DBAKKESMOD_SDK_PATH="%APPDATA%/bakkesmod/bakkesmod/bakkesmodsdk" ^
         -G "Visual Studio 17 2022" -A x64

cmake --build . --config Release
```

The DLL will be automatically copied to `%APPDATA%\bakkesmod\bakkesmod\plugins\` if `AUTO_DEPLOY` is ON (default).

### 4. Set Up the Relay Server

```powershell
cd server
npm install
npm start
```

The server listens on `ws://localhost:9587` by default. Edit `.env` to change the port.

**For remote play**, deploy the server to a VPS/cloud instance and update the server URL in the plugin settings.

---

## Configuration

### In-Game Settings (BakkesMod F2 Menu)

Open the BakkesMod menu (F2) → Plugins → Leo's Rocket Proximity Chat

| Tab | Setting | Description |
|---|---|---|
| **Audio** | Master Volume | Output volume (0-200%) |
| | Mic Volume | Microphone gain (0-300%) |
| | Mute Microphone | Toggle mic mute |
| | Input/Output Device | Select audio devices |
| **Voice** | Push to Talk | Enable PTT mode |
| | PTT Key | Key binding for PTT (default: F3) |
| | Voice Threshold | Open mic sensitivity (0-100) |
| | Hold Time | How long to keep transmitting after voice stops |
| **Proximity** | 3D Spatial Audio | Enable/disable 3D positioning |
| | Max Hearing Distance | Beyond this, silence (default: 8000 uu) |
| | Full Volume Distance | Within this, full volume (default: 1500 uu) |
| | Rolloff Curve | Volume dropoff sharpness |
| **Network** | Server URL | Relay server WebSocket URL |
| | Reconnect | Force reconnect |
| **Status** | Live info | Connection state, peers, bandwidth, etc. |

### Console Commands

```
leo_proxchat_enabled 1              # Enable/disable plugin
leo_proxchat_server_url ws://...    # Set server URL
leo_proxchat_reconnect              # Force reconnect
leo_proxchat_refresh_devices        # Re-enumerate audio devices
leo_proxchat_master_volume 100      # Set master volume
leo_proxchat_mic_volume 100         # Set mic volume
leo_proxchat_push_to_talk 1         # Enable PTT
leo_proxchat_voice_threshold 5      # Set VAD threshold
leo_proxchat_max_distance 8000      # Set max hearing distance
leo_proxchat_3d_audio 1             # Enable 3D audio
```

### Push-to-Talk Key Binding

In the BakkesMod console (F6):
```
bind F3 "leo_proxchat_ptt_pressed"
bind_release F3 "leo_proxchat_ptt_released"
```

---

## Network Protocol

### Control Messages (JSON, text WebSocket frames)

```jsonc
// Client → Server: Join a match room
{"type": "join", "matchId": "...", "playerName": "Leo", "steamId": "76561198..."}

// Client → Server: Leave the room
{"type": "leave"}

// Server → Client: Welcome + existing peers
{"type": "welcome", "yourSteamId": "...", "peers": [{"steamId": "...", "playerName": "..."}]}

// Server → Client: Peer joined
{"type": "peer_joined", "steamId": "...", "playerName": "..."}

// Server → Client: Peer left
{"type": "peer_left", "steamId": "...", "playerName": "..."}
```

### Audio Packets (binary WebSocket frames)

```
Client → Server (13 + N bytes):
  [0x03] [pos_x:f32le] [pos_y:f32le] [pos_z:f32le] [opus_data:N bytes]

Server → Client (21 + N bytes):
  [0x03] [sender_steam_id:u64le] [pos_x:f32le] [pos_y:f32le] [pos_z:f32le] [opus_data:N bytes]
```

---

## Technical Details

### Audio Pipeline
- **Sample Rate**: 48,000 Hz
- **Frame Size**: 960 samples (20ms)
- **Codec**: Opus VOIP mode, 32 kbps, complexity 5
- **FEC**: Opus inband FEC enabled (handles ~5% packet loss)
- **DTX**: Discontinuous transmission (saves bandwidth during silence)

### 3D Spatial Audio
- **Panning**: Equal-power stereo panning based on angle to source
- **Distance**: Smooth rolloff with configurable inner/outer radius
- **Rear Attenuation**: ~20% volume reduction for sounds behind the listener
- **Low-Pass Filter**: One-pole IIR filter, more aggressive at greater distances
- **Mixing**: Additive mixing with `tanh` soft saturation to prevent clipping

### Distances (Unreal Units in Rocket League)
- RL field length: ~10,240 uu
- RL field width: ~8,192 uu
- Default max hearing distance: 8,000 uu (most of the field)
- Default full volume distance: 1,500 uu (about one car-length cluster)

---

## Troubleshooting

| Issue | Solution |
|---|---|
| No audio output | Check output device selection; ensure relay server is running |
| Can't hear other players | Verify both players have the plugin and are connected to the same server |
| High latency | Use a relay server geographically close to players |
| Echo/feedback | Use headphones instead of speakers |
| Mic not working | Check input device selection; ensure mic permissions in Windows |
| Connection failed | Verify server URL; check firewall for WebSocket port (default 9587) |
| Plugin not loading | Ensure DLL is in `%APPDATA%\bakkesmod\bakkesmod\plugins\` |

---

## Deployment (Production Server)

For use across the internet, deploy the relay server:

```bash
# On a VPS (e.g., DigitalOcean, AWS, etc.)
git clone <this-repo>
cd server
npm install
PORT=9587 node server.js

# Or with PM2 for process management
npm install -g pm2
pm2 start server.js --name proxchat
```

Then update the plugin setting:
```
leo_proxchat_server_url ws://your-server-ip:9587
```

For HTTPS/WSS, put the server behind nginx with SSL termination.

---

## License

This project is provided as-is for educational and personal use.
