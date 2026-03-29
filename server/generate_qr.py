# generate_qr.py
# Run this once to generate the sensor QR code image.
# Open the image on your laptop, then scan it with your phone.
#
# Usage:
#   python generate_qr.py
#   python generate_qr.py --mac 11:22:33:44:55:66

import argparse
import qrcode
from PIL import Image
import os

DEFAULT_MAC = "11:22:33:44:55:66"   # replace with your actual sensor MAC


def get_sensor_mac() -> str:
    """
    Try to auto-detect ESP32-C3 MAC address.
    If can't detect, use the default or the one passed via --mac flag.
    """
    parser = argparse.ArgumentParser(description="Generate sensor QR code")
    parser.add_argument("--mac", default=DEFAULT_MAC, help="Sensor MAC address")
    args = parser.parse_args()
    return args.mac.upper()


def generate_qr(mac: str):
    print(f"Generating QR for sensor MAC: {mac}")

    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_H,
        box_size=12,
        border=4,
    )
    qr.add_data(mac)
    qr.make(fit=True)

    img = qr.make_image(fill_color="black", back_color="white")

    output_path = f"sensor_qr_{mac.replace(':', '-')}.png"
    img.save(output_path)

    print(f"QR image saved: {output_path}")
    print(f"Open this file on your laptop and scan it with your phone.")
    print(f"The QR encodes: {mac}")

    # Try to auto-open the image
    try:
        import subprocess, sys
        if sys.platform == "darwin":        # macOS
            subprocess.run(["open", output_path])
        elif sys.platform == "linux":       # Linux
            subprocess.run(["xdg-open", output_path])
        elif sys.platform == "win32":       # Windows
            os.startfile(output_path)
    except Exception:
        print("(Could not auto-open image — open it manually)")


if __name__ == "__main__":
    mac = get_sensor_mac()
    generate_qr(mac)
