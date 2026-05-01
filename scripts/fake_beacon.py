"""Send a synthetic GS_BEACON to the onboard's discovery port for diagnosis.

Usage:  python3 fake_beacon.py [laptop_ip]
"""
import socket
import sys

gs_ip = sys.argv[1] if len(sys.argv) > 1 else "169.254.251.200"

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
msg = b"GS_BEACON,diagnonce,4000,5000,100\n"

s.sendto(msg, ("127.0.0.1", 4100))
print("sent to 127.0.0.1:4100 (loopback)")

try:
    s.sendto(msg, (gs_ip, 4100))
    print(f"sent to {gs_ip}:4100 (unicast)")
except OSError as exc:
    print(f"unicast to {gs_ip} failed: {exc}")

try:
    s.sendto(msg, ("255.255.255.255", 4100))
    print("sent to 255.255.255.255:4100 (broadcast)")
except OSError as exc:
    print(f"broadcast failed: {exc}")
