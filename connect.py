# /// script
# requires-python = ">=3.10"
# dependencies = ["requests"]
# ///

import requests
import time

SERVER_URL = "http://10.245.180.6:3000"


class GlaziaOnboarder:
    def __init__(self):
        self.token   = None
        self.headers = {}
        self.home_id = None
        self.hub_mac = None

    # ── Auth ─────────────────────────────────────────────────────────────────

    def register_user(self):
        print("\n=== STEP 1A: USER REGISTRATION ===")
        name     = input("Name: ")
        email    = input("Email: ")
        password = input("Password: ")
        try:
            resp = requests.post(f"{SERVER_URL}/api/auth/register",
                                 json={"name": name, "email": email, "password": password})
            if resp.status_code == 200:
                print("Registered! Please login.")
                return True
            print(f"Registration Failed: {resp.text}")
            return False
        except requests.exceptions.ConnectionError:
            print(f"\n[ERROR] Cannot connect to {SERVER_URL}.")
            return False

    def login(self):
        print("\n=== STEP 1B: USER LOGIN ===")
        email    = input("Email: ")
        password = input("Password: ")
        resp = requests.post(f"{SERVER_URL}/api/auth/login",
                             json={"email": email, "password": password})
        if resp.status_code == 200:
            data = resp.json()
            self.token   = data["token"]
            self.headers = {"Authorization": f"Bearer {self.token}"}
            print(f"Welcome back, {data['user']['name']}!")
            return True
        print(f"Login failed: {resp.text}")
        return False

    # ── Hub setup ─────────────────────────────────────────────────────────────

    def setup_hub(self):
        print("\n=== STEP 2: SETUP HUB ===")
        print("(Pre-requisite: Press the Hub button so it shows 'HUB PAIRING')")
        self.hub_mac = input("Enter Hub MAC (from ESP32 logs): ").upper()
        home_name    = input("Enter a name for this Home: ")

        resp = requests.post(f"{SERVER_URL}/api/homes/setup-hub",
                             json={"hubMacAddress": self.hub_mac,
                                   "homeName": home_name,
                                   "location": "Lab"},
                             headers=self.headers)
        if resp.status_code != 200:
            print(f"Setup failed: {resp.text}")
            return None

        session = resp.json()
        print("\n=========================================")
        print("  PROVISIONING TOKEN GENERATED:")
        print(f"  {session['provisioningToken']}")
        print("=========================================\n")
        return session

    def wait_for_hub_online(self):
        print("Waiting for Hub to register with server (do the BLE setup now)...")
        for _ in range(40):
            time.sleep(3)
            try:
                resp  = requests.get(f"{SERVER_URL}/api/auth/me", headers=self.headers)
                homes = resp.json().get("homes", [])
                for home in homes:
                    if home.get("hubMacAddress") == self.hub_mac:
                        self.home_id = home["id"]
                        print(f"\n✓ Hub is online! Home: '{home['name']}' (id: {self.home_id})")
                        return True
            except Exception:
                pass
            print("  polling...")
        print("\nTimeout — hub didn't check in.")
        return False

    # ── Sensor pairing ────────────────────────────────────────────────────────

    def wait_for_sensor_pairing_mode(self):
        print("\n=== STEP 4: SENSOR PAIRING ===")
        print("Press the Hub button a SECOND TIME to enter sensor pairing mode.")
        print("Waiting for hub to open the pairing window...")

        for _ in range(60):          # wait up to 3 minutes
            time.sleep(3)
            try:
                resp  = requests.get(f"{SERVER_URL}/api/auth/me", headers=self.headers)
                homes = resp.json().get("homes", [])
                for home in homes:
                    if home.get("id") == self.home_id:
                        hub = home.get("hub", {})
                        if hub.get("pairingWindowOpen"):
                            print("\n✓ Sensor pairing mode is ON (2-minute window)!")
                            return True
            except Exception:
                pass
            print("  waiting for button press...")

        print("\nTimeout — pairing window never opened.")
        return False

    def pair_sensor(self):
        print("\nScan the sensor QR code with your phone camera to read the MAC.")
        sensor_mac = input("Enter Sensor MAC: ").upper().strip()

        name = input("Sensor name (e.g. Front Door): ") or "Sensor"
        zone = input("Zone (e.g. living room):       ") or "general"

        resp = requests.post(
            f"{SERVER_URL}/api/homes/{self.home_id}/sensors/pair",
            json={
                "sensorMacAddress": sensor_mac,
                "name": name,
                "type": "pulse",
                "zone": zone,
            },
            headers=self.headers,
        )

        if resp.status_code == 200:
            print(f"\n✓ Sensor MAC sent to server!")
            print("  Server is pushing it to hub over WebSocket...")
            print("  Watch TFT: 'Sensor Found!' → 'Sensor Paired!'")
            print(f"  Sensor: {sensor_mac}  Zone: {zone}")
            return True
        else:
            print(f"\n✗ Pairing failed: {resp.text}")
            return False

    def pair_another(self):
        while True:
            again = input("\nPair another sensor? [y/N]: ").strip().lower()
            if again != 'y':
                break
            if not self.wait_for_sensor_pairing_mode():
                break
            self.pair_sensor()


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    app = GlaziaOnboarder()

    action = input("(R)egister new user or (L)ogin? [R/L]: ").strip().upper()
    if action == 'R':
        if not app.register_user():
            return

    if not app.login():
        return

    # Check if hub already registered (skip setup if reusing session)
    resp  = requests.get(f"{SERVER_URL}/api/auth/me", headers=app.headers)
    homes = resp.json().get("homes", [])

    if homes:
        home = homes[0]
        app.home_id = home["id"]
        app.hub_mac = home.get("hubMacAddress", "")
        print(f"\nExisting home found: '{home['name']}' — skipping hub setup.")
        print("Go straight to sensor pairing (press hub button 2nd time).")
    else:
        session = app.setup_hub()
        if not session:
            return

        print("\n=== STEP 3: BLE SETUP (nRF Connect) ===")
        print("1. Open nRF Connect on your phone.")
        print("2. Connect to 'GlaziaHub'.")
        print("3. Write WiFi SSID  → characteristic 0xFF01")
        print("4. Write WiFi Pass  → characteristic 0xFF02")
        print(f"5. Write token      → characteristic 0xFF03")

        if not app.wait_for_hub_online():
            return

    # Sensor pairing
    if not app.wait_for_sensor_pairing_mode():
        return

    app.pair_sensor()
    app.pair_another()

    print("\nDone. System is active.")


if __name__ == "__main__":
    main()
