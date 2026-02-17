# Leo's Rocket Proximity Chat — Relay Server

Lightweight WebSocket relay server for the [Leo's Rocket Proximity Chat](https://github.com/youpaki/LeoProximityChat) BakkesMod plugin.

Groups players by Rocket League match ID and forwards audio/position data between peers in the same match.

---

## Quick Start (Standalone Exe)

1. Download `LeoProxChatServer.exe` from [Releases](https://github.com/youpaki/LeoProxChatServer/releases)
2. (Optional) Place a `.env` file next to the exe to configure settings
3. Double-click `LeoProxChatServer.exe`

The server will start listening on `ws://0.0.0.0:9587`.

## Quick Start (Node.js)

```bash
npm install
npm start
```

Requires Node.js 18+.

## Configuration (.env)

Create a `.env` file next to `server.js` (or the exe):

```env
PORT=9587              # WebSocket port
MAX_ROOM_SIZE=10       # Max players per room
HEARTBEAT_MS=30000     # Heartbeat interval (ms)
MAX_AUDIO_BYTES=2048   # Max audio packet size
```

## Deploying to Production

### VPS / Cloud (recommended)

```bash
# On your server (Ubuntu, Debian, etc.)
git clone https://github.com/youpaki/LeoProxChatServer.git
cd LeoProxChatServer
npm install

# Run with PM2 for auto-restart
npm install -g pm2
pm2 start server.js --name proxchat
pm2 save
pm2 startup
```

### Docker

```dockerfile
FROM node:20-alpine
WORKDIR /app
COPY package*.json ./
RUN npm ci --production
COPY server.js .env* ./
EXPOSE 9587
CMD ["node", "server.js"]
```

### SSL/WSS (behind nginx)

```nginx
server {
    listen 443 ssl;
    server_name proxchat.yourdomain.com;

    ssl_certificate     /etc/letsencrypt/live/proxchat.yourdomain.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/proxchat.yourdomain.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:9587;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

Then set the plugin server URL to `wss://proxchat.yourdomain.com`.

## Protocol

| Direction | Format |
|---|---|
| Client → Server (text) | JSON: `{"type":"join","matchId":"...","playerName":"...","steamId":"..."}` |
| Client → Server (binary) | `[0x03][pos_x:f32le][pos_y:f32le][pos_z:f32le][opus_data...]` |
| Server → Client (binary) | `[0x03][sender_id:u64le][pos_x:f32le][pos_y:f32le][pos_z:f32le][opus_data...]` |

## License

MIT
