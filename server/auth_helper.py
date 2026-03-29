# auth_helper.py
# JWT creation and verification helpers.

import time
from jose import jwt, JWTError
from fastapi import HTTPException, Header
from typing import Optional
import store

SECRET = "glazia-dev-secret"
ALGORITHM = "HS256"
EXPIRES_IN = 7 * 24 * 3600  # 7 days in seconds


def create_token(user_id: str) -> str:
    payload = {
        "sub": user_id,
        "exp": time.time() + EXPIRES_IN
    }
    return jwt.encode(payload, SECRET, algorithm=ALGORITHM)


def decode_token(token: str) -> str:
    """Returns userId or raises HTTPException."""
    try:
        payload = jwt.decode(token, SECRET, algorithms=[ALGORITHM])
        return payload["sub"]
    except JWTError:
        raise HTTPException(status_code=401, detail="Invalid or expired token")


def get_current_user(authorization: Optional[str] = Header(None)) -> dict:
    """
    FastAPI dependency — call this in any route that needs auth.
    Reads the Authorization: Bearer <token> header,
    verifies it, and returns the user dict from the store.
    """
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing Authorization header")

    token = authorization.split(" ")[1]
    user_id = decode_token(token)

    user = store.users.get(user_id)
    if not user:
        raise HTTPException(status_code=401, detail="User not found")

    return user


DEVICE_API_KEY = "glazia-dev-key"

def verify_device_key(x_device_api_key: Optional[str] = Header(None)):
    """
    FastAPI dependency — verifies the device API key header.
    Used by hub routes so random devices can't call device endpoints.
    """
    if x_device_api_key != DEVICE_API_KEY:
        raise HTTPException(status_code=403, detail="Invalid device API key")


def verify_hub(
    x_hub_mac_address: Optional[str] = Header(None),
    x_hub_secret: Optional[str] = Header(None),
) -> dict:
    """
    FastAPI dependency — verifies hub MAC + secret combo.
    Returns the hub dict from the store.
    """
    verify_device_key()

    if not x_hub_mac_address:
        raise HTTPException(status_code=400, detail="Missing X-Hub-Mac-Address header")

    mac = x_hub_mac_address.upper()
    hub = store.hubs.get(mac)

    if not hub:
        raise HTTPException(status_code=404, detail="Hub not found")

    if hub["secret"] != x_hub_secret:
        raise HTTPException(status_code=403, detail="Invalid hub secret")

    return hub
