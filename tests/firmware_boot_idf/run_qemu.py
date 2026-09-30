"""Exercise real bootloader/otadata/NVS across resets, without physical devices."""
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time

build = Path(sys.argv[1])
sys.path.insert(0, str(Path(os.environ['IDF_PATH']) / 'components/partition_table'))
import gen_esp32part

args = json.loads((build / 'flasher_args.json').read_text())
image = bytearray(b'\xff' * (4 * 1024 * 1024))
for offset, filename in args['flash_files'].items():
    data = (build / filename).read_bytes()
    start = int(offset, 0)
    assert start + len(data) <= len(image)
    image[start:start + len(data)] = data
table = gen_esp32part.PartitionTable.from_binary(
    (build / args['partition-table']['file']).read_bytes())
app = (build / args['app']['file']).read_bytes()
# Both slots run the same test; stage/slot checks distinguish the candidates.
for part in table:
    if part.type == gen_esp32part.TYPES['app']:
        assert len(app) <= part.size
        image[part.offset:part.offset + len(app)] = app
flash = build / 'ota-test-flash.bin'
flash.write_bytes(image)
qemu = os.environ.get('QEMU_XTENSA') or shutil.which('qemu-system-xtensa')
if not qemu:
    raise SystemExit('Set QEMU_XTENSA to the Espressif qemu-system-xtensa executable')
process = subprocess.Popen([qemu, '-nographic', '-machine', 'esp32', '-drive',
                            f'file={flash},if=mtd,format=raw'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
log = bytearray()
try:
    deadline = time.monotonic() + 50
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], 1)[0]:
            chunk = process.stdout.read1(8192)
            if not chunk:
                break
            log.extend(chunk)
            if any(marker in log for marker in (b'BOOT_TEST: ALL PASS', b'assert failed',
                                                 b'ESP_ERROR_CHECK failed', b'Guru Meditation')):
                break
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
(build / 'qemu.log').write_bytes(log)
print(log.decode(errors='replace'))
if b'BOOT_TEST: ALL PASS' not in log:
    raise SystemExit('Firmware boot contract failed or timed out')
