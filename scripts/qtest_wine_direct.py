#!/usr/bin/env python3
"""Direct linux /usr/bin/wine smoke (no fork)."""
import json, os, socket, subprocess, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SER = "/tmp/com1-wine-direct.log"
QMP = "/tmp/qmp-wine-direct.sock"
for p in (SER, QMP):
    os.path.exists(p) and os.remove(p)

qemu = subprocess.Popen([
    "qemu-system-x86_64", "-m", "256M",
    f"-cdrom", f"{ROOT}/nexxon-os.img",
    "-drive", f"id=d,file={ROOT}/nexxon-data.img,format=raw,if=none",
    "-device", "ich9-ahci,id=ahci",
    "-device", "ide-hd,drive=d,bus=ahci.0",
    "-boot", "order=d", "-vga", "std", "-display", "none", "-no-reboot",
    "-serial", f"file:{SER}",
    "-qmp", f"unix:{QMP},server=on,wait=off",
    "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
])

def q(sock, cmd):
    sock.sendall((json.dumps(cmd) + "\n").encode())
    while True:
        for part in sock.recv(4096).decode().splitlines():
            if part.strip():
                m = json.loads(part)
                if "return" in m or "error" in m:
                    return m

def sk(sock, text):
    for ch in text:
        k = "ret" if ch == "\n" else ("spc" if ch == " " else ch)
        q(sock, {"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})

try:
    for _ in range(100):
        time.sleep(0.1)
        if os.path.exists(QMP):
            break
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(QMP)
    sock.recv(4096)
    q(sock, {"execute": "qmp_capabilities"})
    dl = time.time() + 90
    while time.time() < dl:
        with open(SER, errors="replace") as f:
            if "entering shell" in f.read():
                break
        time.sleep(0.2)
    time.sleep(1)
    q(sock, {"execute": "input-send-event", "arguments": {
        "device": "mouse", "type": "abs", "value": {"axis": "x", "input": 16000}}})
    q(sock, {"execute": "input-send-event", "arguments": {
        "device": "mouse", "type": "abs", "value": {"axis": "y", "input": 10000}}})
    q(sock, {"execute": "input-send-event", "arguments": {
        "device": "mouse", "type": "btn", "value": {"down": True, "button": "left"}}})
    q(sock, {"execute": "input-send-event", "arguments": {
        "device": "mouse", "type": "btn", "value": {"down": False, "button": "left"}}})
    time.sleep(0.5)
    sk(sock, "linux /usr/bin/wine\n")
    for i in range(20):
        time.sleep(1)
        if qemu.poll() is not None:
            print("qemu died", i)
            break
        with open(SER, errors="replace") as f:
            log = f.read()
        if "finished, code=" in log:
            print("linux finished", i)
            break
    with open(SER, errors="replace") as f:
        for line in f:
            if "linux" in line:
                print(line.rstrip())
finally:
    qemu.terminate()
