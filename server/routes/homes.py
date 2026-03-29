# routes/homes.py

import uuid
import time
import secrets
from fastapi import APIRouter, HTTPException, Depends
from models import SetupHubRequest
from auth_helper import get_current_user
import store

router = APIRouter(prefix="/api/homes", tags=["Homes"])

SESSION_TTL = 300  # provisioning token valid for 5 minutes


@router.post("/setup-hub")
def setup_hub(body: SetupHubRequest, current_user: dict = Depends(get_current_user)):
    """
    Mobile app calls this before BLE provisioning.
    Creates a temporary session and returns a provisioning token
    that the app will pass to the hub over BLE.
    """
    mac = body.hubMacAddress.upper()
    token = secrets.token_hex(16)   # random provisioning token
    home_id = str(uuid.uuid4())     # reserved homeId for when hub registers
    expires_at = time.time() + SESSION_TTL

    store.setup_sessions[token] = {
        "token": token,
        "hubMacAddress": mac,
        "homeId": home_id,
        "userId": current_user["id"],
        "homeName": body.homeName,
        "location": body.location,
        "expiresAt": expires_at,
    }

    return {
        "setupSessionId": home_id,
        "hubMacAddress": mac,
        "provisioningToken": token,
        "expiresAt": expires_at,
    }


@router.get("")
def get_homes(current_user: dict = Depends(get_current_user)):
    """Returns all homes belonging to the logged-in user."""
    return {"homes": _build_homes_for_user(current_user["id"])}


@router.get("/{home_id}")
def get_home(home_id: str, current_user: dict = Depends(get_current_user)):
    home = store.homes.get(home_id)
    if not home:
        raise HTTPException(status_code=404, detail="Home not found")
    if home["userId"] != current_user["id"]:
        raise HTTPException(status_code=403, detail="Not your home")

    hub = store.hubs.get(home.get("hubMacAddress", ""))
    home_sensors = [s for s in store.sensors.values() if s["homeId"] == home_id]

    return {**home, "hub": hub, "sensors": home_sensors}


# ── Helper ──────────────────────────────────────────────────────────────────

def _build_homes_for_user(user_id: str):
    result = []
    for home in store.homes.values():
        if home["userId"] != user_id:
            continue
        hub = store.hubs.get(home.get("hubMacAddress", ""))
        home_sensors = [s for s in store.sensors.values() if s["homeId"] == home["id"]]
        result.append({**home, "hub": hub, "sensors": home_sensors})
    return result
