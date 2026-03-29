# main.py
# Entry point — ties all routes together and starts the server.

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from routes import auth, homes, sensors, device_hub, notifications

app = FastAPI(
    title="Glazia Home Secure — Mock Server",
    description="Prototype mock server for validating the full hub + sensor pairing flow.",
    version="0.1.0",
)

# Allow requests from any origin (phone browser, Termux, ESP32)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── Register all route groups ────────────────────────────────────────────────
app.include_router(auth.router)
app.include_router(homes.router)
app.include_router(sensors.router)
app.include_router(device_hub.router)
app.include_router(notifications.router)


@app.get("/")
def root():
    return {
        "status": "Glazia mock server is running",
        "docs": "Open /docs in your browser to test all endpoints",
    }
