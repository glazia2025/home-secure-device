# models.py
# These are the shapes of every request body the server accepts.
# FastAPI uses these to auto-validate incoming JSON.

from pydantic import BaseModel
from typing import Optional, Any, Dict


# ── Auth ────────────────────────────────────────────────────────────────────

class RegisterRequest(BaseModel):
    name: str
    email: str
    password: str

class LoginRequest(BaseModel):
    email: str
    password: str


# ── Homes ───────────────────────────────────────────────────────────────────

class SetupHubRequest(BaseModel):
    hubMacAddress: str
    homeName: str
    location: str


# ── Sensors ─────────────────────────────────────────────────────────────────

class PairSensorRequest(BaseModel):
    sensorMacAddress: str
    name: str
    type: str        # e.g. "contact"
    zone: str        # e.g. "Front Door Frame"


# ── Device — Hub ─────────────────────────────────────────────────────────────

class HubRegisterRequest(BaseModel):
    hubMacAddress: str
    provisioningToken: str

class HubEventRequest(BaseModel):
    sensorMacAddress: str
    eventType: str           # e.g. "contact_open", "contact_closed", "motion_detected"
    severity: str            # e.g. "critical", "warning", "info"
    payload: Optional[Dict[str, Any]] = {}

class SensorAcknowledgeRequest(BaseModel):
    sensorMacAddress: str
