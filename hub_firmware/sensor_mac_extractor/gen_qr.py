"""
gen_qr.py — Generate QR code for a Glazia sensor board.

Usage:
    python gen_qr.py --mac AA:BB:CC:DD:EE:FF

The QR encodes just the MAC address string.
Scan it with the Glazia app while the hub is in sensor pairing mode.

Requirements: pip install -r requirements.txt
"""

import argparse
import os
import sys
import qrcode


def generate(mac: str) -> None:
    mac = mac.upper().strip()

    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_H,
        box_size=12,
        border=4,
    )
    qr.add_data(mac)
    qr.make(fit=True)

    img = qr.make_image(fill_color="black", back_color="white")
    filename = f"sensor_qr_{mac.replace(':', '-')}.png"
    img.save(filename)

    print(f"✓ QR saved → {filename}")
    print(f"  Encodes : {mac}")
    print()
    print("  Steps:")
    print("  1. Press hub button 2nd time → hub enters sensor pairing mode")
    print("  2. Open this QR image on your laptop / phone")
    print("  3. Scan it on the Glazia app")
    print("  4. Hub will receive the sensor MAC over WebSocket")
    print("  5. Hub sends HELLO → sensor sends ACK → ESP-NOW link live")

    # Auto-open
    try:
        if sys.platform == "darwin":
            os.system(f"open {filename}")
        elif sys.platform.startswith("linux"):
            os.system(f"xdg-open {filename}")
        elif sys.platform == "win32":
            os.startfile(filename)
    except Exception:
        print(f"\n  Open manually: {filename}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate Glazia sensor QR code")
    parser.add_argument("--mac", required=True,
                        help="Sensor MAC address (e.g. AA:BB:CC:DD:EE:FF)")
    args = parser.parse_args()
    generate(args.mac)
