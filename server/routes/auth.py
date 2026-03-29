# routes/auth.py

import uuid
from fastapi import APIRouter, HTTPException, Depends
from models import RegisterRequest, LoginRequest
from auth_helper import create_token, get_current_user
import store

router = APIRouter(prefix="/api/auth", tags=["Auth"])


@router.post("/register")
def register(body: RegisterRequest):
    # Check if email already used
    if body.email in store.email_to_user_id:
        raise HTTPException(status_code=400, detail="Email already registered")

    user_id = str(uuid.uuid4())
    user = {
        "id": user_id,
        "name": body.name,
        "email": body.email,
        "password": body.password,   # plain text — fine for a prototype
    }
    store.users[user_id] = user
    store.email_to_user_id[body.email] = user_id

    return {"message": "Registered successfully", "userId": user_id}


@router.post("/login")
def login(body: LoginRequest):
    user_id = store.email_to_user_id.get(body.email)
    if not user_id:
        raise HTTPException(status_code=401, detail="Email not found")

    user = store.users[user_id]
    if user["password"] != body.password:
        raise HTTPException(status_code=401, detail="Wrong password")

    token = create_token(user_id)

    # Build user's homes list to return with login response
    user_homes = _get_user_homes(user_id)

    return {
        "token": token,
        "user": {"id": user["id"], "name": user["name"], "email": user["email"]},
        "homes": user_homes,
    }


@router.get("/me")
def me(current_user: dict = Depends(get_current_user)):
    user_homes = _get_user_homes(current_user["id"])
    return {
        "user": {
            "id": current_user["id"],
            "name": current_user["name"],
            "email": current_user["email"],
        },
        "homes": user_homes,
    }


# ── Helper ──────────────────────────────────────────────────────────────────

def _get_user_homes(user_id: str):
    result = []
    for home in store.homes.values():
        if home["userId"] != user_id:
            continue

        hub = store.hubs.get(home.get("hubMacAddress", ""))
        home_sensors = [
            s for s in store.sensors.values()
            if s["homeId"] == home["id"]
        ]

        result.append({
            **home,
            "hub": hub,
            "sensors": home_sensors,
        })
    return result
