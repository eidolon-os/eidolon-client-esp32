"""Bounded execution of the production inbox contract on Espressif QEMU."""
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time

image, log_path = map(Path, sys.argv[1:])
qemu = os.environ.get("QEMU_XTENSA") or shutil.which("qemu-system-xtensa")
if not qemu:
    raise SystemExit("Set QEMU_XTENSA to the Espressif qemu-system-xtensa executable")
# Default ESP32 test partition table uses 2 MiB flash. Do not modify physical flash.
with image.open("ab") as output:
    output.write(b"\xff" * max(0, 2 * 1024 * 1024 - image.stat().st_size))
command = [qemu, "-nographic", "-machine", "esp32", "-drive",
           f"file={image},if=mtd,format=raw", "-no-reboot"]
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
log = bytearray()
try:
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], 1)[0]:
            chunk = process.stdout.read1(8192)
            if not chunk:
                break
            log.extend(chunk)
            if b"ALL PASS" in log or b"assert failed" in log or b"Guru Meditation" in log:
                break
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
log_path.write_bytes(log)
print(log.decode(errors="replace"))
if b"INBOX_TEST: ALL PASS" not in log:
    raise SystemExit("Controller inbox contract failed or timed out")
