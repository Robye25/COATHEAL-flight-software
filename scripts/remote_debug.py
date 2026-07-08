"""Deploy the current bundle to the Pi, build, and launch in debug mode.

Safe-guarded: stops any prior debug run, refuses to start a second instance,
and verifies liveness by hitting the command port.
"""
import getpass
import os
import subprocess

import paramiko

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.environ.get("COATHEAL_PI_HOST", "169.254.10.10")
USER = os.environ.get("COATHEAL_PI_USER", "coatheal")
PASSWORD = os.environ.get("COATHEAL_PI_PASSWORD") or getpass.getpass("Pi password: ")
BUNDLE = os.environ.get("COATHEAL_BUNDLE", os.path.join(ROOT, "coatheal.bundle"))
REMOTE_BUNDLE = "/tmp/coatheal.bundle"
REMOTE_DIR = "/home/coatheal/COATHEAL-flight-software"
try:
    CURRENT_REF = subprocess.check_output(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=ROOT, text=True).strip()
except subprocess.CalledProcessError:
    CURRENT_REF = "HEAD"
BUNDLE_REF = os.environ.get(
    "COATHEAL_BUNDLE_REF", CURRENT_REF if CURRENT_REF != "HEAD" else "HEAD")

if not os.path.exists(BUNDLE):
    refs = ["HEAD", "main"]
    if BUNDLE_REF and BUNDLE_REF not in refs:
        refs.append(BUNDLE_REF)
    subprocess.run(["git", "bundle", "create", BUNDLE, *refs],
                   cwd=ROOT, check=True)

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, username=USER, password=PASSWORD, timeout=15, banner_timeout=15)

print(f"uploading {BUNDLE} -> {REMOTE_BUNDLE}")
sftp = c.open_sftp(); sftp.put(BUNDLE, REMOTE_BUNDLE); sftp.close()

COMMANDS = [
    # Stop any prior debug run before updating the tree.
    f"cd {REMOTE_DIR} && bash scripts/run_debug.sh --stop 2>&1 || true",

    # Sync to the freshly-pushed main.
    f"cd {REMOTE_DIR} && git fetch {REMOTE_BUNDLE} {BUNDLE_REF}:main-remote && "
    f"git reset --hard main-remote && git log --oneline -3",

    # Rebuild with the debug-friendly sources.
    f"cd {REMOTE_DIR} && cmake --build build -j$(nproc) 2>&1 | tail -15",

    # Make the launcher executable (Windows-created files lose +x).
    f"cd {REMOTE_DIR} && chmod +x scripts/run_debug.sh",

    # Launch in the background.
    f"cd {REMOTE_DIR} && bash scripts/run_debug.sh --bg",

    # Give it a moment to initialize, then check.
    f"cd {REMOTE_DIR} && sleep 1 && bash scripts/run_debug.sh --status",

    # Tail the stdout log to confirm startup.
    f"cd {REMOTE_DIR} && tail -20 logs/onboard_debug.stdout.log 2>&1 || true",

    # Probe the command port — PING should return pong if everything is live.
    f"cd {REMOTE_DIR} && (printf 'PING\\n' | nc -w 2 127.0.0.1 5000) 2>&1 || echo '(ncat probe skipped)'",
]

for cmd in COMMANDS:
    print(f"\n### {cmd[:220]}")
    _, out, err = c.exec_command(cmd, timeout=300)
    rc = out.channel.recv_exit_status()
    print(out.read().decode(errors="replace"))
    e = err.read().decode(errors="replace")
    if e.strip():
        print("STDERR:", e)
    print(f"[exit {rc}]")

c.close()
