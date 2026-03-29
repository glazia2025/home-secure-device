# routes/sensors.py

import time
import uuid
from fastapi import APIRouter, HTTPException, Depends
from models import PairSensorRequest
from auth_helper import get_current_user
import store

router = APIRouter(tags=["Sensors"])


@router.post("/api/homes/{home_id}/sensors/pair")
async def pair_sensor(
    home_id: str,
    body: PairSensorRequest,
    current_user: dict = Depends(get_current_user),
):
    """
    Called by mobile app after scanning the sensor QR.
    1. Validates home ownership and active pairing window.
    2. Creates/updates sensor in store.
    3. Pushes sensor MAC to hub over the open WebSocket instantly.
    """

    # ── 1. Validate home ────────────────────────────────────────────────────
    home = store.homes.get(home_id)
    if not home:
        raise HTTPException(status_code=404, detail="Home not found")
    if home["userId"] != current_user["id"]:
        raise HTTPException(status_code=403, detail="Not your home")

    hub_mac = home.get("hubMacAddress")
    if not hub_mac:
        raise HTTPException(status_code=400, detail="No hub registered for this home")

    # ── 2. Validate pairing window ──────────────────────────────────────────
    hub = store.hubs.get(hub_mac)
    if not hub:
        raise HTTPException(status_code=404, detail="Hub not found")

    if not hub.get("pairingWindowOpen"):
        raise HTTPException(
            status_code=400,
            detail="Hub pairing mode is not active. Press the hub button first."
        )

    if hub.get("pairingExpiresAt") and time.time() > hub["pairingExpiresAt"]:
        hub["pairingWindowOpen"] = False
        raise HTTPException(status_code=400, detail="Pairing window expired. Press hub button again.")

    # ── 3. Check sensor not already paired elsewhere ─────────────────────────
    sensor_mac = body.sensorMacAddress.upper()
    existing = store.sensors.get(sensor_mac)
    if existing and existing["hubMacAddress"] != hub_mac:
        raise HTTPException(status_code=400, detail="Sensor already paired to a different hub")

    # ── 4. Save sensor ───────────────────────────────────────────────────────
    store.sensors[sensor_mac] = {
        "id": str(uuid.uuid4()),
        "macAddress": sensor_mac,
        "name": body.name,
        "type": body.type,
        "zone": body.zone,
        "homeId": home_id,
        "hubMacAddress": hub_mac,
        "pendingEspNow": True,     # hub hasn't connected yet via ESP-NOW
        "pairedAt": time.time(),
    }

    # ── 5. Push sensor MAC to hub over WebSocket ────────────────────────────
    ws = store.hub_ws_connections.get(hub_mac)
    if ws:
        try:
            import json
            await ws.send_text(json.dumps({
                "event": "SENSOR_PAIRED",
                "sensorMacAddress": sensor_mac,
                "sensorName": body.name,
                "zone": body.zone,
            }))
            print(f"[WS] Pushed SENSOR_PAIRED to hub {hub_mac} → sensor {sensor_mac}")
        except Exception as e:
            print(f"[WS] Failed to push to hub {hub_mac}: {e}")
    else:
        print(f"[WS] No active WebSocket for hub {hub_mac} — hub may not be in pairing mode")

    # ── 6. Close pairing window ──────────────────────────────────────────────
    hub["pairingWindowOpen"] = False

    return {
        "message": "Sensor paired successfully",
        "hubMacAddress": hub_mac,
        "sensorMacAddress": sensor_mac,
        "sensor": store.sensors[sensor_mac],
    }
