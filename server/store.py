# store.py
# This is our fake in-memory database.
# Everything lives in Python dicts/lists.
# Restarting the server wipes all data — that is fine for a prototype.

from typing import Dict, Any
from starlette.websockets import WebSocket

# ── Users ──────────────────────────────────────────────────────────────────
# Key: userId (str)
# Value: { id, name, email, password, token }
users: Dict[str, Dict[str, Any]] = {}

# Quick lookup: email → userId
email_to_user_id: Dict[str, str] = {}

# ── Homes ──────────────────────────────────────────────────────────────────
# Key: homeId (str)
# Value: { id, name, location, userId, hubMacAddress }
homes: Dict[str, Dict[str, Any]] = {}

# ── Hubs ───────────────────────────────────────────────────────────────────
# Key: hubMacAddress (str, uppercase)
# Value: { macAddress, secret, homeId,
#          pairingWindowOpen (bool), pairingExpiresAt (float | None) }
hubs: Dict[str, Dict[str, Any]] = {}

# ── Sensors ────────────────────────────────────────────────────────────────
# Key: sensorMacAddress (str, uppercase)
# Value: { macAddress, name, type, zone, homeId, hubMacAddress, pendingEspNow }
sensors: Dict[str, Dict[str, Any]] = {}

# ── Hub Setup Sessions ──────────────────────────────────────────────────────
# Key: provisioningToken (str)
# Value: { token, hubMacAddress, homeId, userId, expiresAt (float) }
setup_sessions: Dict[str, Dict[str, Any]] = {}

# ── Notifications ───────────────────────────────────────────────────────────
# A simple list. Each item:
# { id, userId, homeId, sensorMacAddress, eventType, severity, payload, read, createdAt }
notifications = []

# ── Activity Logs ───────────────────────────────────────────────────────────
# Same shape as notifications but never deleted — full history
activity_logs = []

# ── WebSocket Connections ───────────────────────────────────────────────────
# Key: hubMacAddress (str)
# Value: WebSocket object (live connection from hub)
# When we need to push sensor MAC to hub, we look it up here.
hub_ws_connections: Dict[str, WebSocket] = {}

# ── SSE Connections ─────────────────────────────────────────────────────────
# Key: userId (str)
# Value: asyncio.Queue — we push notification dicts into this queue,
#        the SSE endpoint reads from it and streams to the phone.
sse_queues: Dict[str, Any] = {}
