# routes/device_hub.py

import time
import uuid
import secrets
import json
from fastapi import APIRouter, HTTPException, WebSocket, WebSocketDisconnect, Header, Query
from typing import Optional
from models import HubRegisterRequest, HubEventRequest, SensorAcknowledgeRequest
from auth_helper import verify_device_key, DEVICE_API_KEY
import store

router = APIRouter(prefix="/api/device/hubs", tags=["Device - Hub"])

PAIRING_WINDOW_SECONDS = 120    # 2 min — matches hub's SENSOR_PAIR_TIMEOUT_MS


# ── Hub Registration ─────────────────────────────────────────────────────────

@router.post("/register")
def register_hub(
    body: HubRegisterRequest,
    x_device_api_key: Optional[str] = Header(None),
):
    """
    Hub calls this after connecting to WiFi.
    Validates provisioning token, creates home + hub in store,
    returns hub secret for future authenticated calls.
    """
    if x_device_api_key != DEVICE_API_KEY:
        raise HTTPException(status_code=403, detail="Invalid device API key")

    mac = body.hubMacAddress.upper()
    token = body.provisioningToken

    # Validate provisioning token
    session = store.setup_sessions.get(token)
    if not session:
        raise HTTPException(status_code=400, detail="Invalid provisioning token")
    if time.time() > session["expiresAt"]:
        raise HTTPException(status_code=400, detail="Provisioning token expired")
    if session["hubMacAddress"] != mac:
        raise HTTPException(status_code=400, detail="MAC address mismatch")

    # Generate hub secret
    hub_secret = secrets.token_hex(32)

    # Create hub
    store.hubs[mac] = {
        "macAddress": mac,
        "secret": hub_secret,
        "homeId": session["homeId"],
        "pairingWindowOpen": False,
        "pairingExpiresAt": None,
        "registeredAt": time.time(),
    }

    # Create home and link to hub
    store.homes[session["homeId"]] = {
        "id": session["homeId"],
        "name": session["homeName"],
        "location": session["location"],
        "userId": session["userId"],
        "hubMacAddress": mac,
        "createdAt": time.time(),
    }

    # Clean up session so token can't be reused
    del store.setup_sessions[token]

    print(f"[HUB] Registered hub {mac} → home '{session['homeName']}'")

    return {
        "message": "Hub registered successfully",
        "hubMacAddress": mac,
        "hubSecret": hub_secret,
        "homeId": session["homeId"],
        "homeName": session["homeName"],
    }


# ── Sensor Pairing Mode ───────────────────────────────────────────────────────

@router.post("/sensor-pairing-mode")
def enable_sensor_pairing(
    x_device_api_key: Optional[str] = Header(None),
    x_hub_mac_address: Optional[str] = Header(None),
    x_hub_secret: Optional[str] = Header(None),
):
    """
    Hub calls this when button is pressed (2nd press).
    Opens a 60-second pairing window.
    """
    if x_device_api_key != DEVICE_API_KEY:
        raise HTTPException(status_code=403, detail="Invalid device API key")

    mac = (x_hub_mac_address or "").upper()
    hub = store.hubs.get(mac)
    if not hub:
        raise HTTPException(status_code=404, detail="Hub not found")
    if hub["secret"] != x_hub_secret:
        raise HTTPException(status_code=403, detail="Invalid hub secret")

    hub["pairingWindowOpen"] = True
    hub["pairingExpiresAt"] = time.time() + PAIRING_WINDOW_SECONDS

    print(f"[HUB] Sensor pairing window opened for hub {mac} — {PAIRING_WINDOW_SECONDS}s")

    return {
        "message": "Sensor pairing mode enabled",
        "expiresAt": hub["pairingExpiresAt"],
        "windowSeconds": PAIRING_WINDOW_SECONDS,
    }


# ── Hub Events (sensor triggered) ────────────────────────────────────────────

@router.post("/events")
async def hub_event(
    body: HubEventRequest,
    x_device_api_key: Optional[str] = Header(None),
    x_hub_mac_address: Optional[str] = Header(None),
    x_hub_secret: Optional[str] = Header(None),
):
    """
    Hub sends this when a sensor detects an event (door open, etc.).
    Backend stores it and pushes to mobile app via SSE.
    """
    if x_device_api_key != DEVICE_API_KEY:
        raise HTTPException(status_code=403, detail="Invalid device API key")

    mac = (x_hub_mac_address or "").upper()
    hub = store.hubs.get(mac)
    if not hub:
        raise HTTPException(status_code=404, detail="Hub not found")
    if hub["secret"] != x_hub_secret:
        raise HTTPException(status_code=403, detail="Invalid hub secret")

    sensor_mac = body.sensorMacAddress.upper()
    sensor = store.sensors.get(sensor_mac)
    if not sensor:
        raise HTTPException(status_code=404, detail="Sensor not found or not paired")

    home = store.homes.get(hub["homeId"])
    user_id = home["userId"] if home else None

    # Build notification
    notification = {
        "id": str(uuid.uuid4()),
        "userId": user_id,
        "homeId": hub["homeId"],
        "homeName": home["name"] if home else "Unknown",
        "sensorMacAddress": sensor_mac,
        "sensorName": sensor.get("name", sensor_mac),
        "zone": sensor.get("zone", ""),
        "eventType": body.eventType,
        "severity": body.severity,
        "payload": body.payload,
        "read": False,
        "createdAt": time.time(),
    }

    store.notifications.append(notification)
    store.activity_logs.append(notification)

    print(f"[EVENT] {body.eventType} from sensor {sensor_mac} in zone '{sensor.get('zone')}'")

    # Push to SSE queue if phone is listening
    if user_id and user_id in store.sse_queues:
        await store.sse_queues[user_id].put(notification)

    return {"message": "Event recorded", "notificationId": notification["id"]}


# ── Sensor Acknowledge (ESP-NOW connected) ────────────────────────────────────

@router.post("/sensors/acknowledge")
def acknowledge_sensor(
    body: SensorAcknowledgeRequest,
    x_device_api_key: Optional[str] = Header(None),
    x_hub_mac_address: Optional[str] = Header(None),
    x_hub_secret: Optional[str] = Header(None),
):
    """
    Hub calls this after successfully connecting to sensor over ESP-NOW.
    Clears the pendingEspNow flag.
    """
    if x_device_api_key != DEVICE_API_KEY:
        raise HTTPException(status_code=403, detail="Invalid device API key")

    mac = (x_hub_mac_address or "").upper()
    hub = store.hubs.get(mac)
    if not hub:
        raise HTTPException(status_code=404, detail="Hub not found")
    if hub["secret"] != x_hub_secret:
        raise HTTPException(status_code=403, detail="Invalid hub secret")

    sensor_mac = body.sensorMacAddress.upper()
    sensor = store.sensors.get(sensor_mac)
    if sensor:
        sensor["pendingEspNow"] = False
        print(f"[HUB] ESP-NOW acknowledged: hub {mac} ↔ sensor {sensor_mac}")

    return {"message": "Sensor acknowledged", "sensorMacAddress": sensor_mac}


# ── WebSocket ─────────────────────────────────────────────────────────────────

@router.websocket("/ws")
async def hub_websocket(
    websocket: WebSocket,
    mac:    Optional[str] = Query(None),   # hub sends as ?mac=...&secret=...
    secret: Optional[str] = Query(None),
):
    """
    Hub connects here after entering sensor pairing mode.
    We keep this connection open.
    When a sensor is paired (via POST /sensors/pair),
    we instantly push the sensor MAC through this socket.
    """
    mac = (mac or "").upper()
    hub = store.hubs.get(mac)

    if not hub or hub["secret"] != secret:
        await websocket.close(code=4003, reason="Unauthorized")
        return

    await websocket.accept()
    store.hub_ws_connections[mac] = websocket
    print(f"[WS] Hub {mac} connected — waiting for sensor pairing")

    try:
        while True:
            # Keep connection alive — listen for messages from hub
            data = await websocket.receive_text()
            msg = json.loads(data)

            if msg.get("event") == "SENSOR_ACKNOWLEDGED":
                sensor_mac = msg.get("sensorMacAddress", "").upper()
                sensor = store.sensors.get(sensor_mac)
                if sensor:
                    sensor["pendingEspNow"] = False
                print(f"[WS] Hub {mac} acknowledged sensor {sensor_mac} over ESP-NOW")

            # You can handle other hub → server messages here in the future

    except WebSocketDisconnect:
        print(f"[WS] Hub {mac} disconnected")
    finally:
        store.hub_ws_connections.pop(mac, None)
