/**
 * Leo's Rocket Proximity Chat — Relay Server
 * 
 * Lightweight WebSocket relay that groups players by Rocket League match ID
 * and forwards audio/position data between peers in the same match.
 * 
 * Protocol:
 *   Text frames  → JSON control messages (join, leave, position, etc.)
 *   Binary frames → Audio packets with position header
 * 
 * Binary audio format (client → server):
 *   [0x03 (1 byte)] [pos_x f32le] [pos_y f32le] [pos_z f32le] [opus_data ...]
 * 
 * Binary audio format (server → client):
 *   [0x03 (1 byte)] [sender_steam_id 8 bytes LE] [pos_x f32le] [pos_y f32le] [pos_z f32le] [opus_data ...]
 */

require("dotenv").config();
const { WebSocketServer, WebSocket } = require("ws");

// ─── Configuration ──────────────────────────────────────────────────────────
const PORT          = parseInt(process.env.PORT || "9587", 10);
const MAX_ROOM_SIZE = parseInt(process.env.MAX_ROOM_SIZE || "10", 10);
const HEARTBEAT_MS  = parseInt(process.env.HEARTBEAT_MS || "30000", 10);
const MAX_AUDIO_BYTES = parseInt(process.env.MAX_AUDIO_BYTES || "2048", 10);

// ─── State ──────────────────────────────────────────────────────────────────
/** @type {Map<string, Map<string, ClientInfo>>}  matchId → (steamId → client) */
const rooms = new Map();

/** @typedef {{ ws: WebSocket, steamId: string, playerName: string, matchId: string, alive: boolean }} ClientInfo */

// ─── Server ─────────────────────────────────────────────────────────────────
const wss = new WebSocketServer({ port: PORT, maxPayload: MAX_AUDIO_BYTES + 256 });

console.log(`[Leo ProxChat] Relay server listening on ws://0.0.0.0:${PORT}`);
console.log(`[Leo ProxChat] Max room size: ${MAX_ROOM_SIZE}`);

wss.on("connection", (ws, req) => {
    const ip = req.headers["x-forwarded-for"] || req.socket.remoteAddress;
    console.log(`[connect] ${ip}`);

    /** @type {ClientInfo|null} */
    let client = null;

    ws.isAlive = true;
    ws.on("pong", () => { ws.isAlive = true; });

    ws.on("message", (data, isBinary) => {
        try {
            if (isBinary) {
                handleBinary(client, data);
            } else {
                handleText(ws, client, JSON.parse(data.toString("utf8")));
            }
        } catch (err) {
            console.error(`[error] ${err.message}`);
            sendJson(ws, { type: "error", message: err.message });
        }
    });

    ws.on("close", () => {
        if (client) removeClient(client);
        console.log(`[disconnect] ${client ? client.playerName : ip}`);
    });

    ws.on("error", (err) => {
        console.error(`[ws-error] ${err.message}`);
    });

    // ── Text message handler ────────────────────────────────────────────
    function handleText(ws, existingClient, msg) {
        switch (msg.type) {
            case "join": {
                if (existingClient) removeClient(existingClient);

                const { matchId, playerName, steamId } = msg;
                if (!matchId || !steamId) {
                    return sendJson(ws, { type: "error", message: "matchId and steamId required" });
                }

                // Create room if needed
                if (!rooms.has(matchId)) rooms.set(matchId, new Map());
                const room = rooms.get(matchId);

                if (room.size >= MAX_ROOM_SIZE) {
                    return sendJson(ws, { type: "error", message: "Room is full" });
                }

                // Remove existing connection with same steamId (reconnect)
                if (room.has(steamId)) {
                    const old = room.get(steamId);
                    try { old.ws.close(1000, "replaced"); } catch (_) {}
                    room.delete(steamId);
                }

                client = { ws, steamId, playerName: playerName || "Unknown", matchId, alive: true };
                room.set(steamId, client);

                // Notify joiner of existing peers
                const peers = [];
                for (const [sid, peer] of room) {
                    if (sid !== steamId) {
                        peers.push({ steamId: sid, playerName: peer.playerName });
                    }
                }
                sendJson(ws, { type: "welcome", yourSteamId: steamId, peers });

                // Notify existing peers of new joiner
                broadcastToRoom(matchId, steamId, {
                    type: "peer_joined",
                    steamId,
                    playerName: client.playerName
                });

                console.log(`[join] ${playerName} (${steamId}) → room ${matchId} (${room.size} players)`);
                break;
            }

            case "position": {
                // Optional: clients can send position updates as text too
                // (primary position data is embedded in audio packets)
                if (!existingClient) return;
                broadcastToRoom(existingClient.matchId, existingClient.steamId, {
                    type: "peer_position",
                    steamId: existingClient.steamId,
                    x: msg.x || 0,
                    y: msg.y || 0,
                    z: msg.z || 0,
                    yaw: msg.yaw || 0,
                    pitch: msg.pitch || 0
                });
                break;
            }

            case "leave": {
                if (existingClient) removeClient(existingClient);
                client = null;
                break;
            }

            case "ping": {
                sendJson(ws, { type: "pong", ts: Date.now() });
                break;
            }

            default:
                sendJson(ws, { type: "error", message: `Unknown message type: ${msg.type}` });
        }
    }

    // ── Binary (audio) handler ──────────────────────────────────────────
    function handleBinary(existingClient, data) {
        if (!existingClient) return;
        if (data.length < 13) return; // 1 type + 12 position bytes minimum

        const msgType = data[0];
        if (msgType !== 0x03) return; // Only audio type supported

        const room = rooms.get(existingClient.matchId);
        if (!room) return;

        // Build relayed packet: prepend sender's steamId (8 bytes LE)
        const steamIdBuf = Buffer.alloc(8);
        // Store steamId as two 32-bit ints (JS doesn't handle 64-bit ints natively well)
        const steamIdBig = BigInt(existingClient.steamId);
        steamIdBuf.writeBigUInt64LE(steamIdBig);

        // Final format: [0x03][steamId 8B][pos_x 4B][pos_y 4B][pos_z 4B][opus_data...]
        const relayed = Buffer.concat([
            Buffer.from([0x03]),
            steamIdBuf,
            data.subarray(1) // pos_x, pos_y, pos_z, opus_data from original
        ]);

        // Relay to all other peers in the room
        for (const [sid, peer] of room) {
            if (sid !== existingClient.steamId && peer.ws.readyState === WebSocket.OPEN) {
                try {
                    peer.ws.send(relayed, { binary: true });
                } catch (_) {}
            }
        }
    }
});

// ─── Helpers ────────────────────────────────────────────────────────────────
function sendJson(ws, obj) {
    if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(obj));
    }
}

function broadcastToRoom(matchId, excludeSteamId, obj) {
    const room = rooms.get(matchId);
    if (!room) return;
    const msg = JSON.stringify(obj);
    for (const [sid, peer] of room) {
        if (sid !== excludeSteamId && peer.ws.readyState === WebSocket.OPEN) {
            try { peer.ws.send(msg); } catch (_) {}
        }
    }
}

function removeClient(client) {
    const room = rooms.get(client.matchId);
    if (!room) return;

    room.delete(client.steamId);

    // Notify remaining peers
    broadcastToRoom(client.matchId, "", {
        type: "peer_left",
        steamId: client.steamId,
        playerName: client.playerName
    });

    // Clean up empty rooms
    if (room.size === 0) {
        rooms.delete(client.matchId);
        console.log(`[room] Deleted empty room ${client.matchId}`);
    } else {
        console.log(`[leave] ${client.playerName} left room ${client.matchId} (${room.size} remain)`);
    }
}

// ─── Heartbeat (detect dead connections) ────────────────────────────────────
const heartbeatInterval = setInterval(() => {
    wss.clients.forEach((ws) => {
        if (ws.isAlive === false) {
            return ws.terminate();
        }
        ws.isAlive = false;
        ws.ping();
    });
}, HEARTBEAT_MS);

wss.on("close", () => {
    clearInterval(heartbeatInterval);
});

// ─── Stats endpoint (optional simple HTTP) ─────────────────────────────────
// You can add an HTTP server for monitoring if desired

// ─── Graceful shutdown ──────────────────────────────────────────────────────
process.on("SIGINT", () => {
    console.log("\n[shutdown] Closing all connections...");
    wss.clients.forEach((ws) => ws.close(1001, "server shutting down"));
    wss.close(() => {
        console.log("[shutdown] Server stopped.");
        process.exit(0);
    });
});

process.on("uncaughtException", (err) => {
    console.error("[fatal]", err);
});
