===============================================
  Leo's Rocket Proximity Chat - Installation
===============================================

Talk to nearby players in Rocket League with 3D spatial audio!
Features: HRTF spatialization, Doppler effect, reverb, push-to-talk / open mic.

-----------------------------------------------
  REQUIREMENTS
-----------------------------------------------
- Rocket League (Epic or Steam)
- BakkesMod installed and running
  Download: https://bakkesmod.com

-----------------------------------------------
  PLUGIN INSTALLATION (Client)
-----------------------------------------------
1. Copy "LeoProximityChat.dll" into:
       %APPDATA%\bakkesmod\bakkesmod\plugins\

2. Copy "leo_proximity_chat.set" into:
       %APPDATA%\bakkesmod\bakkesmod\plugins\settings\

3. Open BakkesMod (F2) > Plugins > Plugin Manager
   Click "Open pluginsfolder", verify the DLL is there.

4. In the BakkesMod console (F6), type:
       plugin load leoproximitychat

5. To auto-load on startup, add this line to:
       %APPDATA%\bakkesmod\bakkesmod\cfg\plugins.cfg
   
       plugin load leoproximitychat

Done! Open F2 > Plugins and you'll see "Leo's Rocket Proximity Chat" settings.

-----------------------------------------------
  SERVER SETUP (Host or Community)
-----------------------------------------------
Someone must run the relay server so players can hear each other.

Option A - Standalone (no Node.js needed):
   1. Open the "server" folder
   2. Edit ".env" to change port/settings if needed (default: 9587)
   3. Double-click "start-server.bat" (or run LeoProxChatServer.exe)

Option B - From source (requires Node.js 18+):
   1. cd server-source/
   2. npm install
   3. node server.js

The server runs on ws://localhost:9587 by default.

-----------------------------------------------
  CONNECTING TO A SERVER
-----------------------------------------------
In BakkesMod (F2) > Plugins > Leo's Rocket Proximity Chat:
   - Set "Server URL" to ws://<server-ip>:9587
   - Click "Reconnect"

For LAN play, use ws://192.168.x.x:9587
For online play, the host must port-forward TCP 9587.

-----------------------------------------------
  SETTINGS OVERVIEW
-----------------------------------------------
- Master Volume      : Overall voice volume (0-200%)
- Mic Volume          : Your mic input level (0-300%)
- Mute Microphone     : Quick mute toggle
- Push to Talk        : Enable PTT mode (otherwise open mic with VAD)
- PTT Key             : Key binding for push-to-talk
- Voice Threshold     : VAD sensitivity for open mic mode (0-100)
- 3D Spatial Audio    : Enable/disable HRTF spatialization
- Max Hearing Distance: How far you can hear others (Unreal units)
- Rolloff Curve       : How quickly volume drops with distance

-----------------------------------------------
  TROUBLESHOOTING
-----------------------------------------------
- No sound?  Check that your output device is working and volume > 0.
- Can't hear others?  Make sure everyone is connected to the same server
  and in the same match.
- Crackling?  Try lowering the rolloff or restarting the plugin.
- Plugin not loading?  Verify BakkesMod is injected (F2 works in-game).

-----------------------------------------------
  FOLDER CONTENTS
-----------------------------------------------
LeoProximityChat.dll          Plugin file (goes in plugins/)
leo_proximity_chat.set        Settings UI (goes in plugins/settings/)
server/
  LeoProxChatServer.exe       Standalone relay server (~40 MB)
  start-server.bat            Quick-start script
  .env                        Server configuration
README.txt                    This file

===============================================
  Created by Leo  |  github.com/youpaki
===============================================
