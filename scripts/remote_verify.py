import sys, paramiko, os

HOST = "169.254.10.10"
USER = "coatheal"
PW = "COTHIL"
BUNDLE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "coatheal.bundle")
REMOTE_BUNDLE = "/tmp/coatheal.bundle"
REMOTE_DIR = "/home/coatheal/COATHEAL-flight-software"

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, username=USER, password=PW, timeout=15, banner_timeout=15)

print(f"SFTP uploading {BUNDLE} -> {REMOTE_BUNDLE}", flush=True)
sftp = c.open_sftp()
sftp.put(BUNDLE, REMOTE_BUNDLE)
sftp.close()
print("upload done", flush=True)

COMMANDS = [
    f"if [ -d {REMOTE_DIR}/.git ]; then cd {REMOTE_DIR} && git fetch {REMOTE_BUNDLE} main:main-remote && git reset --hard main-remote; else git clone {REMOTE_BUNDLE} {REMOTE_DIR} && cd {REMOTE_DIR} && git checkout main || git checkout -b main FETCH_HEAD; fi",
    f"cd {REMOTE_DIR} && git log --oneline -6",
    "which cmake g++ && cmake --version | head -1 && g++ --version | head -1",
    f"cd {REMOTE_DIR} && rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -30",
    f"cd {REMOTE_DIR} && cmake --build build -j$(nproc) 2>&1 | tail -80",
    f"cd {REMOTE_DIR} && ctest --test-dir build --output-on-failure 2>&1 | tail -80",
]

for cmd in COMMANDS:
    print(f"\n### {cmd[:200]}\n", flush=True)
    stdin, stdout, stderr = c.exec_command(cmd, timeout=900)
    rc = stdout.channel.recv_exit_status()
    out = stdout.read().decode(errors="replace")
    err = stderr.read().decode(errors="replace")
    print(out)
    if err.strip():
        print("STDERR:", err)
    print(f"[exit {rc}]", flush=True)

c.close()
