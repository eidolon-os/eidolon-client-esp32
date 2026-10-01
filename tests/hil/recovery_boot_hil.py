"""Bounded four-board boot/soak verification using the installed firmware.

Requires pyserial. Each --device is NAME=PORT. Explicit RTS resets happen once
per round; reconnecting a re-enumerated USB console never sends another reset.
This tests boot and an idle control-room connection, not a spoken conversation.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import json
from pathlib import Path
import re
import time

import serial


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def redact(line: str) -> str:
    line = ANSI.sub("", line)
    line = re.sub(r"(a=ice-(?:ufrag|pwd):).*", r"\1[REDACTED]", line)
    line = re.sub(r"\b(user|psw|password|access_token|token)([:=])[^\s&]+",
                  r"\1\2[REDACTED]", line, flags=re.I)
    return line


def assess(log: str, firmware: str, sdk: str, hub_ip: str) -> dict:
    stamps = re.findall(r"EIDOLON-BUILDSTAMP git=(\S+) sdk=(\S+)", log)
    ready = re.findall(r"\((\d+)\).*visible state=READY", log)
    states = re.findall(r"visible state=(\w+)", log)
    addresses = re.findall(r"LocalMdns:.*address=(\S+)", log)
    activation = re.findall(r"\[dispatch\] type=0 .*run_ms=(\d+)", log)
    heap = [(int(t), int(f)) for t, f in re.findall(
        r"\((\d+)\) SystemInfo: free sram: (\d+)", log)]
    faults = [line for line in log.splitlines() if re.search(
        r"Guru Meditation|panic'ed|CORRUPT HEAP|Stack canary|assert failed|"
        r"Task watchdog|AXP2101 did not answer|SDA is still held low|"
        r"HubHttp:.*(?:error=|failed)|getaddrinfo\(\).*returns|"
        r"LiveKitSession: Failure:|room state=Reconnecting|"
        r"\[dispatch\] dropped", line)]
    reasons = []
    if stamps and any(not g.startswith(firmware) or "+dirty" in g or s != sdk
                      for g, s in stamps):
        reasons.append("installed firmware/SDK differs from requested baseline")
    if len(stamps) > 1:
        reasons.append("additional boot observed without requested reset")
    if faults:
        reasons.append("failure/recovery evidence in serial log")
    if hub_ip and any(a != hub_ip for a in addresses if a != "-"):
        reasons.append("mDNS returned an unexpected Hub address")
    result = "FAIL" if reasons else "PASS"
    if not stamps and not reasons:
        result = "BLOCKED"
        reasons.append("boot build stamp not captured")
    elif not ready and not reasons:
        result = "BLOCKED"
        reasons.append("device is not READY; connection test cannot pass")
    elif ready and (not addresses or not activation) and not reasons:
        result = "BLOCKED"
        reasons.append("activation/mDNS evidence missing")
    # Idle heap is reported, not diagnosed as a leak from a single difference.
    settled = [(t, f) for t, f in heap if ready and t > int(ready[0]) + 20000]
    return dict(result=result, reasons=reasons, stamps=stamps, states=states,
                ready_ms=[int(t) for t in ready], resolved_addresses=addresses,
                activation_dispatch_ms=[int(t) for t in activation],
                heap_samples=heap, settled_heap_delta=(
                    settled[-1][1] - settled[0][1] if len(settled) > 1 else None),
                faults=faults, error_lines=[line for line in log.splitlines()
                    if re.search(r"^E \(", line)])


def run_device(name: str, port: str, args: argparse.Namespace) -> list[dict]:
    results = []
    for round_no in range(1, args.rounds + 1):
        duration = args.seconds + (args.soak_seconds if round_no == args.rounds else 0)
        path = args.out / f"{name}-round-{round_no}.log"
        start = time.monotonic()
        connection = None
        reset_sent = False
        disconnects = 0
        errors = []
        pending = b""
        with path.open("w", encoding="utf-8", buffering=1) as output:
            output.write("CAPTURE " + json.dumps(dict(
                utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                name=name, port=port, round=round_no, duration=duration)) + "\n")
            try:
                while time.monotonic() - start < duration:
                    if connection is None:
                        try:
                            connection = serial.Serial(port=None, baudrate=115200,
                                                       timeout=0.2, exclusive=True)
                            connection.dtr = False
                            connection.rts = False
                            connection.port = port
                            connection.open()
                            if not reset_sent:
                                connection.reset_input_buffer()
                                reset_sent = True
                                connection.rts = True
                                time.sleep(0.15)
                                connection.rts = False
                        except (OSError, serial.SerialException) as error:
                            errors.append(str(error))
                            if connection:
                                connection.close()
                            connection = None
                            time.sleep(0.2)
                            continue
                    try:
                        pending += connection.read(4096)
                        while b"\n" in pending:
                            raw, pending = pending.split(b"\n", 1)
                            output.write(redact(raw.decode("utf-8", "replace").rstrip("\r")) + "\n")
                    except (OSError, serial.SerialException) as error:
                        errors.append(str(error))
                        disconnects += 1
                        connection.close()
                        connection = None
            finally:
                if connection:
                    connection.close()
                if pending:
                    output.write(redact(pending.decode("utf-8", "replace")) + "\n")
        result = assess(path.read_text(), args.firmware, args.sdk,
                        args.hub_ips.get(name, args.default_hub_ip))
        result.update(name=name, port=port, round=round_no, duration_seconds=duration,
                      log=str(path.resolve()), serial_disconnects=disconnects,
                      serial_errors=list(dict.fromkeys(errors)))
        results.append(result)
        print(json.dumps(result, ensure_ascii=False), flush=True)
        if result["result"] == "FAIL" and not args.continue_on_failure:
            break
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", action="append", required=True, metavar="NAME=PORT")
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--hub-ip", action="append", default=[],
                        metavar="IP|NAME=IP", help="repeat for devices on different Hubs")
    parser.add_argument("--continue-on-failure", action="store_true")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=75)
    parser.add_argument("--soak-seconds", type=float, default=180)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    devices = [item.split("=", 1) for item in args.device]
    if (args.rounds < 1 or args.seconds < 30 or args.soak_seconds < 0
            or any(len(d) != 2 or not re.fullmatch(r"[\w-]+", d[0]) for d in devices)
            or len({d[0] for d in devices}) != len(devices)
            or len({d[1] for d in devices}) != len(devices)):
        parser.error("require unique names/ports, positive rounds, >=30s capture and >=0s soak")
    args.default_hub_ip = ""
    args.hub_ips = {}
    for item in args.hub_ip:
        if "=" in item:
            name, address = item.split("=", 1)
            if name not in {d[0] for d in devices} or name in args.hub_ips:
                parser.error("Hub address must name a unique device from --device")
            args.hub_ips[name] = address
        elif not args.default_hub_ip:
            args.default_hub_ip = item
        else:
            parser.error("only one common Hub address is allowed")
    args.out.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(devices)) as pool:
        futures = [pool.submit(run_device, name, port, args) for name, port in devices]
        results = [item for future in futures for item in future.result()]
    (args.out / "checkpoints.json").write_text(
        json.dumps(results, ensure_ascii=False, indent=2) + "\n")
    return 1 if any(r["result"] == "FAIL" for r in results) else (
        2 if any(r["result"] == "BLOCKED" for r in results) else 0)


if __name__ == "__main__":
    raise SystemExit(main())
