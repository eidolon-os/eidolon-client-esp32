#!/usr/bin/env python3
"""Standalone device-side AEC qualification runner.

This script builds/flashes a temporary AEC qualification firmware variant, then
captures AECQ serial events and produces a small report. It intentionally keeps
all generated sdkconfig state in the build/output directories so normal app
configuration is left untouched.
"""

from __future__ import annotations

import argparse
import base64
import glob
import json
import math
import os
import re
import select
import shlex
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import wave
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import fcntl
except ImportError:  # pragma: no cover - non-POSIX host fallback
    fcntl = None  # type: ignore[assignment]

try:
    import termios
except ImportError:  # pragma: no cover - non-POSIX host fallback
    termios = None  # type: ignore[assignment]


PROJECT_ROOT = Path(__file__).resolve().parents[2]
BOARDS_DIR = PROJECT_ROOT / "main" / "boards"
KCONFIG = PROJECT_ROOT / "main" / "Kconfig.projbuild"
CMAKE = PROJECT_ROOT / "main" / "CMakeLists.txt"
DEFAULT_NEAR_AUDIO = PROJECT_ROOT / ".cache" / "aec_qualification" / "near_speech_chirp_v1.wav"

AEC_THRESHOLDS = {
    "barge_in_erle_db": 18.0,
    "barge_in_residual_dbfs": -45.0,
    "ptt_erle_db": 10.0,
    "max_clip_ratio": 0.001,
}

THRESHOLD_PROFILE = {
    "name": "v1_engineering_default",
    "source": "engineering_heuristic_not_official_certification",
    "description": (
        "Initial Eidolon admission thresholds for comparing devices under a fixed local "
        "test setup. These are not Espressif official pass/fail thresholds."
    ),
}


@dataclass
class BoardVariant:
    board_path: str
    name: str
    full_name: str
    target: str
    manufacturer: str | None
    board_symbol: str
    sdkconfig_append: list[str] = field(default_factory=list)


def run(cmd: list[str], *, cwd: Path = PROJECT_ROOT) -> None:
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def idf_export_candidates() -> list[Path]:
    candidates: list[Path] = []
    for env_name in ("EIDOLON_IDF_EXPORT",):
        value = os.environ.get(env_name)
        if value:
            candidates.append(Path(value).expanduser())
    for env_name in ("EIDOLON_IDF_PATH", "IDF_PATH"):
        value = os.environ.get(env_name)
        if value:
            candidates.append(Path(value).expanduser() / "export.sh")

    idf_path_file = PROJECT_ROOT / "scripts" / "eidolon" / "idf.path"
    if idf_path_file.exists():
        for line in idf_path_file.read_text(encoding="utf-8").splitlines():
            clean = line.split("#", 1)[0].strip()
            if clean:
                candidates.append(Path(clean).expanduser() / "export.sh")

    home = Path.home()
    candidates.extend(sorted(home.glob(".espressif/v*/esp-idf/export.sh"), reverse=True))
    candidates.extend([home / "esp" / "esp-idf" / "export.sh", home / "esp-idf" / "export.sh"])

    seen: set[str] = set()
    result: list[Path] = []
    for path in candidates:
        key = str(path)
        if key not in seen:
            result.append(path)
            seen.add(key)
    return result


def find_idf_export() -> Path | None:
    for candidate in idf_export_candidates():
        if candidate.exists():
            return candidate
    return None


def run_idf(args: list[str], *, cwd: Path = PROJECT_ROOT) -> None:
    if shutil_which("idf.py"):
        run(["idf.py", *args], cwd=cwd)
        return

    export_sh = find_idf_export()
    if export_sh is None:
        raise SystemExit(
            "idf.py was not found. Source ESP-IDF first, set EIDOLON_IDF_EXPORT, "
            "or write the ESP-IDF root to scripts/eidolon/idf.path."
        )
    quoted = " ".join(shlex.quote(item) for item in ["idf.py", *args])
    shell_cmd = f"source {shlex.quote(str(export_sh))} >/dev/null && {quoted}"
    print(f"+ source {export_sh} && {quoted}")
    subprocess.run(["/bin/zsh", "-lc", shell_cmd], cwd=cwd, check=True)


def shutil_which(name: str) -> str | None:
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        path = Path(directory) / name
        if path.exists() and os.access(path, os.X_OK):
            return str(path)
    return None


def read_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def get_manufacturer(cfg: dict[str, Any]) -> str | None:
    value = cfg.get("manufacturer")
    return value.strip() if isinstance(value, str) and value.strip() else None


def extract_board_config_from_sdkconfig_append(items: list[str]) -> str | None:
    matches: list[str] = []
    pattern = re.compile(r"^(CONFIG_BOARD_TYPE_[A-Z0-9_]+)=y$")
    for item in items:
        match = pattern.match(item.strip())
        if match:
            matches.append(match.group(1))
    uniq = list(dict.fromkeys(matches))
    if len(uniq) > 1:
        raise ValueError(f"multiple board symbols in sdkconfig_append: {uniq}")
    return uniq[0] if uniq else None


def find_board_config_candidates(board_path: str) -> list[str]:
    board_leaf = board_path.split("/")[-1]
    pattern = f'set(BOARD_TYPE "{board_leaf}")'
    lines = CMAKE.read_text(encoding="utf-8").splitlines()
    candidates: list[str] = []
    for idx, line in enumerate(lines):
        if pattern not in line:
            continue
        for back_idx in range(idx - 1, -1, -1):
            back_line = lines[back_idx].strip()
            if "if(CONFIG_BOARD_TYPE_" in back_line:
                candidates.append(back_line.split("if(", 1)[1].split(")", 1)[0])
                break
            if "elseif(CONFIG_BOARD_TYPE_" in back_line:
                candidates.append(back_line.split("elseif(", 1)[1].split(")", 1)[0])
                break
    return candidates


def symbol_supports_target(symbol: str, target: str) -> bool:
    target_flag = f"IDF_TARGET_{target.upper()}"
    lines = KCONFIG.read_text(encoding="utf-8").splitlines()
    in_symbol = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("config "):
            in_symbol = stripped.split("config ", 1)[1].strip() == symbol
            continue
        if in_symbol and stripped.startswith(("choice ", "endchoice", "menu ", "endmenu")):
            break
        if in_symbol and "depends on" in stripped and target_flag in stripped:
            return True
    return False


def resolve_board_symbol(board_path: str, target: str, sdkconfig_append: list[str]) -> str:
    explicit = extract_board_config_from_sdkconfig_append(sdkconfig_append)
    if explicit:
        return explicit

    candidates = find_board_config_candidates(board_path)
    if not candidates:
        raise ValueError(f"cannot resolve board symbol for {board_path}")
    if len(candidates) == 1:
        return candidates[0]

    by_target = [c for c in candidates if symbol_supports_target(c, target)]
    if len(by_target) == 1:
        return by_target[0]

    target_u = target.upper()
    target_short = target_u.replace("ESP32", "")
    by_name = [c for c in candidates if target_u in c or f"_{target_short}" in c]
    if by_name:
        return by_name[0]
    return candidates[0]


def extract_use_device_aec_symbols() -> set[str]:
    text = KCONFIG.read_text(encoding="utf-8")
    match = re.search(r"config USE_DEVICE_AEC.*?depends on USE_AUDIO_PROCESSOR && \((.*?)\)\n\s+help", text, re.S)
    if not match:
        return set()
    return set(re.findall(r"BOARD_TYPE_[A-Za-z0-9_]+", match.group(1)))


def board_has_reference_hint(board_path: str) -> bool:
    board_dir = BOARDS_DIR / board_path
    haystack_parts: list[str] = []
    for src in list(board_dir.glob("*.cc")) + list(board_dir.glob("*.h")):
        try:
            haystack_parts.append(src.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            pass
    haystack = "\n".join(haystack_parts)
    return any(token in haystack for token in ("AUDIO_INPUT_REFERENCE", "input_reference", "GetInputDeviceHandle"))


def collect_boards() -> list[BoardVariant]:
    aec_symbols = extract_use_device_aec_symbols()
    variants: list[BoardVariant] = []

    for cfg_path in sorted(BOARDS_DIR.rglob("config.json")):
        if cfg_path.parent.name == "common":
            continue
        board_path = cfg_path.parent.relative_to(BOARDS_DIR).as_posix()
        cfg = read_json(cfg_path)
        target = cfg.get("target")
        if target not in ("esp32s3", "esp32p4"):
            continue
        manufacturer = get_manufacturer(cfg)
        if not board_has_reference_hint(board_path):
            continue
        for build in cfg.get("builds", []):
            name = build["name"]
            sdkconfig_append = list(build.get("sdkconfig_append", []))
            try:
                symbol = resolve_board_symbol(board_path, target, sdkconfig_append)
            except ValueError:
                continue
            kconfig_symbol = symbol.removeprefix("CONFIG_")
            if kconfig_symbol not in aec_symbols:
                continue
            full_name = f"{manufacturer}-{name}" if manufacturer else name
            variants.append(
                BoardVariant(
                    board_path=board_path,
                    name=name,
                    full_name=full_name,
                    target=target,
                    manufacturer=manufacturer,
                    board_symbol=symbol,
                    sdkconfig_append=sdkconfig_append,
                )
            )
    return variants


def find_board(name: str) -> BoardVariant:
    boards = collect_boards()
    for board in boards:
        if name in (board.name, board.full_name, board.board_path):
            return board
    supported = "\n".join(f"  - {b.full_name} ({b.board_path})" for b in boards)
    raise SystemExit(f"Unsupported AEC qualification board: {name}\nSupported boards:\n{supported}")


def list_ports() -> list[str]:
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.usbserial*",
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
    ]
    ports: list[str] = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))
    return sorted(dict.fromkeys(ports))


def port_score(port: str) -> tuple[int, float, str]:
    priorities = [
        ("/dev/cu.usbmodem", 100),
        ("/dev/ttyACM", 95),
        ("/dev/cu.wchusbserial", 80),
        ("/dev/cu.SLAB_USBtoUART", 75),
        ("/dev/cu.usbserial", 70),
        ("/dev/ttyUSB", 65),
    ]
    score = 0
    for prefix, value in priorities:
        if port.startswith(prefix):
            score = value
            break
    try:
        mtime = Path(port).stat().st_mtime
    except OSError:
        mtime = 0.0
    return score, mtime, port


def format_ports(ports: list[str]) -> str:
    return "\n".join(f"  - {p}" for p in ports)


def resolve_port(port: str | None, *, auto_port: bool = False) -> str:
    if port:
        return port
    ports = list_ports()
    if not ports:
        raise SystemExit("No serial port found; pass --port explicitly.")
    if len(ports) == 1:
        print(f"Auto-selected serial port: {ports[0]}")
        return ports[0]
    if auto_port:
        selected = max(ports, key=port_score)
        print(f"Auto-selected serial port: {selected}")
        print("Detected serial ports:")
        print(format_ports(ports))
        return selected
    raise SystemExit(
        "Multiple serial ports found; pass --port explicitly or use --auto-port:\n"
        f"{format_ports(ports)}"
    )


def write_overlay(board: BoardVariant, out_dir: Path) -> Path:
    overlay = out_dir / f"sdkconfig.aecq.{board.name}"
    lines = [
        "# Generated by scripts/aec_qualification/run.py",
        f"{board.board_symbol}=y",
        "CONFIG_USE_AUDIO_PROCESSOR=y",
        "CONFIG_USE_DEVICE_AEC=y",
        "CONFIG_EIDOLON_AEC_QUALIFICATION=y",
        "CONFIG_EIDOLON_HUB_MODE=y",
        "CONFIG_USE_SERVER_AEC=n",
        "CONFIG_USE_AUDIO_DEBUGGER=n",
        "CONFIG_EIDOLON_WAKE_WORD_ENABLE=n",
    ]
    for item in board.sdkconfig_append:
        key = item.split("=", 1)[0]
        if key.startswith("CONFIG_BOARD_TYPE_"):
            continue
        if key in {
            "CONFIG_USE_SERVER_AEC",
            "CONFIG_USE_AUDIO_DEBUGGER",
            "CONFIG_EIDOLON_WAKE_WORD_ENABLE",
        }:
            continue
        lines.append(item)
    overlay.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return overlay


def build_firmware(board: BoardVariant, build_dir: Path, out_dir: Path) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    overlay = write_overlay(board, out_dir)
    sdkconfig = out_dir / f"sdkconfig.{board.name}"
    defaults = [
        PROJECT_ROOT / "sdkconfig.defaults",
        PROJECT_ROOT / f"sdkconfig.defaults.{board.target}",
        overlay,
    ]
    existing_defaults = [str(p) for p in defaults if p.exists()]
    run_idf(
        [
            "-B",
            str(build_dir),
            f"-DIDF_TARGET={board.target}",
            f"-DSDKCONFIG={sdkconfig}",
            f"-DSDKCONFIG_DEFAULTS={';'.join(existing_defaults)}",
            f"-DBOARD_NAME={board.name}",
            f"-DBOARD_TYPE={board.board_path}",
            "build",
        ]
    )


def flash_firmware(build_dir: Path, port: str) -> None:
    run_idf(["-B", str(build_dir), "-p", port, "flash"])


class PosixSerial:
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 1.0):
        self.port = port
        self.timeout = timeout
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self._buffer = bytearray()
        self._configure(baudrate)

    def _configure(self, baudrate: int) -> None:
        if termios is None:
            raise RuntimeError("termios is unavailable")
        baud_map = {
            9600: termios.B9600,
            19200: termios.B19200,
            38400: termios.B38400,
            57600: termios.B57600,
            115200: termios.B115200,
            230400: getattr(termios, "B230400", termios.B115200),
            460800: getattr(termios, "B460800", termios.B115200),
            921600: getattr(termios, "B921600", termios.B115200),
        }
        baud = baud_map.get(baudrate)
        if baud is None:
            raise ValueError(f"unsupported baudrate for stdlib serial fallback: {baudrate}")

        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = baud | termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = baud
        attrs[5] = baud
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def _set_modem_bit(self, bit_name: str, enabled: bool) -> None:
        if termios is None or fcntl is None:
            return
        bit = getattr(termios, bit_name, None)
        if bit is None:
            return
        request = getattr(termios, "TIOCMBIS" if enabled else "TIOCMBIC", None)
        if request is None:
            return
        try:
            fcntl.ioctl(self.fd, request, struct.pack("I", bit))
        except OSError:
            pass

    def setDTR(self, enabled: bool) -> None:
        self._set_modem_bit("TIOCM_DTR", enabled)

    def setRTS(self, enabled: bool) -> None:
        self._set_modem_bit("TIOCM_RTS", enabled)

    def readline(self) -> bytes:
        newline = self._buffer.find(b"\n")
        if newline >= 0:
            line = bytes(self._buffer[: newline + 1])
            del self._buffer[: newline + 1]
            return line

        readable, _, _ = select.select([self.fd], [], [], self.timeout)
        if not readable:
            return b""
        try:
            chunk = os.read(self.fd, 4096)
        except BlockingIOError:
            return b""
        if not chunk:
            return b""
        self._buffer.extend(chunk)
        newline = self._buffer.find(b"\n")
        if newline < 0:
            return b""
        line = bytes(self._buffer[: newline + 1])
        del self._buffer[: newline + 1]
        return line

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()


def open_serial(port: str, baudrate: int = 115200, timeout: float = 1.0):
    try:
        import serial  # type: ignore

        return serial.Serial(port, baudrate, timeout=timeout)
    except ImportError as exc:
        if os.name == "posix":
            return PosixSerial(port, baudrate, timeout)
        raise SystemExit("pyserial is required for capture on this platform.") from exc


def pulse_reset(serial_port: Any) -> None:
    set_dtr = getattr(serial_port, "setDTR", None)
    set_rts = getattr(serial_port, "setRTS", None)
    if not callable(set_dtr) or not callable(set_rts):
        return

    try:
        set_dtr(False)
        set_rts(True)
        time.sleep(0.08)
        set_rts(False)
        time.sleep(0.08)
        set_dtr(False)
    except OSError:
        return


def wait_for_port(port: str, timeout_s: float = 15.0, stable_s: float = 0.5) -> None:
    deadline = time.time() + timeout_s
    first_seen: float | None = None
    while time.time() < deadline:
        if Path(port).exists():
            if first_seen is None:
                first_seen = time.time()
            if time.time() - first_seen >= stable_s:
                return
        else:
            first_seen = None
        time.sleep(0.1)
    raise SystemExit(f"Serial port did not become ready after flashing: {port}")


def decode_audio_payload(event: dict[str, Any]) -> bytes:
    data = event.get("data", "")
    if not isinstance(data, str):
        return b""
    try:
        return base64.b64decode(data)
    except ValueError:
        return b""


def rms_from_pcm16(payloads: list[bytes]) -> float:
    total_sq = 0
    count = 0
    for payload in payloads:
        usable = len(payload) - (len(payload) % 2)
        for i in range(0, usable, 2):
            sample = int.from_bytes(payload[i : i + 2], "little", signed=True)
            total_sq += sample * sample
            count += 1
    if count == 0:
        return 0.0
    return math.sqrt(total_sq / count)


def dbfs(rms: float) -> float:
    if rms <= 0:
        return -120.0
    return 20.0 * math.log10(rms / 32768.0)


def save_wav(path: Path, payloads: list[bytes], sample_rate: int = 16000, channels: int = 1) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        for payload in payloads:
            wf.writeframes(payload)


def pcm16(value: float) -> bytes:
    sample = max(-32768, min(32767, int(value * 32767.0)))
    return sample.to_bytes(2, "little", signed=True)


def generate_default_near_audio(path: Path) -> Path:
    sample_rate = 48000
    duration_s = 11.0
    chirp_start_s = 0.25
    chirp_duration_s = 0.65
    speech_start_s = 1.15
    f0_values = [118.0, 142.0, 176.0, 132.0, 205.0, 156.0, 124.0, 188.0]
    path.parent.mkdir(parents=True, exist_ok=True)

    frames = bytearray()
    total = int(duration_s * sample_rate)
    for n in range(total):
        t = n / sample_rate
        value = 0.0

        if chirp_start_s <= t < chirp_start_s + chirp_duration_s:
            u = (t - chirp_start_s) / chirp_duration_s
            f0 = 450.0
            f1 = 3900.0
            freq = f0 * ((f1 / f0) ** u)
            phase = 2.0 * math.pi * f0 * chirp_duration_s * (((f1 / f0) ** u - 1.0) / math.log(f1 / f0))
            window = math.sin(math.pi * u) ** 2
            value += 0.42 * window * math.sin(phase)

        if t >= speech_start_s:
            local = t - speech_start_s
            syllable = int(local / 0.42)
            syllable_pos = (local % 0.42) / 0.42
            voiced = 0.5 - 0.5 * math.cos(2.0 * math.pi * min(1.0, syllable_pos / 0.68))
            gap = 1.0 if syllable_pos < 0.72 else 0.0
            envelope = 0.08 + 0.92 * voiced * gap
            f0 = f0_values[syllable % len(f0_values)] * (1.0 + 0.04 * math.sin(2.0 * math.pi * 2.3 * local))
            harmonic = (
                0.70 * math.sin(2.0 * math.pi * f0 * local)
                + 0.42 * math.sin(2.0 * math.pi * f0 * 2.0 * local + 0.2)
                + 0.25 * math.sin(2.0 * math.pi * f0 * 3.0 * local + 0.6)
            )
            formants = (
                0.30 * math.sin(2.0 * math.pi * 720.0 * local + 0.1)
                + 0.22 * math.sin(2.0 * math.pi * 1230.0 * local + 0.9)
                + 0.14 * math.sin(2.0 * math.pi * 2450.0 * local + 1.7)
            )
            consonant = 0.13 * math.sin(2.0 * math.pi * 3150.0 * local) * (1.0 if syllable_pos < 0.18 else 0.0)
            value += 0.26 * envelope * (harmonic + formants + consonant)

        frames.extend(pcm16(value))

    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(frames)
    return path


def prepare_near_audio(source: str | None, out_dir: Path) -> Path:
    if source:
        src = Path(source).expanduser().resolve()
        if not src.exists():
            raise SystemExit(f"Near-end audio file does not exist: {src}")
    else:
        src = DEFAULT_NEAR_AUDIO
        if not src.exists():
            generate_default_near_audio(src)

    test_audio_dir = out_dir / "test_audio"
    test_audio_dir.mkdir(parents=True, exist_ok=True)
    dst = test_audio_dir / src.name
    if src != dst:
        shutil.copyfile(src, dst)
    return dst


def playback_command(path: Path) -> list[str] | None:
    if sys.platform == "darwin" and shutil_which("afplay"):
        return ["afplay", str(path)]
    if shutil_which("paplay"):
        return ["paplay", str(path)]
    if shutil_which("pw-play"):
        return ["pw-play", str(path)]
    if shutil_which("aplay"):
        return ["aplay", str(path)]
    if shutil_which("ffplay"):
        return ["ffplay", "-nodisp", "-autoexit", "-loglevel", "error", str(path)]
    if sys.platform.startswith("win") and shutil_which("powershell"):
        return [
            "powershell",
            "-NoProfile",
            "-Command",
            f"(New-Object Media.SoundPlayer {str(path)!r}).PlaySync()",
        ]
    return None


def start_near_playback(path: Path) -> subprocess.Popen[bytes]:
    cmd = playback_command(path)
    if cmd is None:
        raise SystemExit(
            "No host audio playback command found. Install/use afplay, paplay, pw-play, aplay, "
            "or ffplay, or rerun with --no-near-playback for manual playback."
        )
    print("+ " + " ".join(shlex.quote(item) for item in cmd))
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_playback(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.terminate()
        proc.wait(timeout=1)
    except subprocess.TimeoutExpired:
        proc.kill()


def capture(
    port: str,
    out_dir: Path,
    timeout_s: int,
    near_audio: Path | None,
    near_audio_lead_ms: int,
    reset_before_capture: bool,
    boot_wait_s: float,
) -> dict[str, Any]:
    events: list[dict[str, Any]] = []
    audio: dict[str, dict[str, list[bytes]]] = {}
    playback_proc: subprocess.Popen[bytes] | None = None
    log_path = out_dir / "serial.log"

    print(f"Capturing AECQ serial events from {port}...")
    wait_for_port(port)
    if reset_before_capture:
        print("Resetting device before capture...")
        try:
            reset_port = open_serial(port, 115200, timeout=1)
        except OSError as exc:
            raise SystemExit(f"Cannot open serial port {port}: {exc}") from exc
        with reset_port as ser:
            pulse_reset(ser)
        wait_for_port(port)
        time.sleep(max(0.0, boot_wait_s))

    try:
        serial_port = open_serial(port, 115200, timeout=1)
    except OSError as exc:
        raise SystemExit(f"Cannot open serial port {port}: {exc}") from exc

    with serial_port as ser, log_path.open("wb") as log:
        deadline = time.time() + timeout_s
        first_byte_deadline = time.time() + 8.0
        saw_bytes = False
        while time.time() < deadline:
            line = ser.readline()
            if not line:
                if not saw_bytes and time.time() > first_byte_deadline:
                    print("No serial bytes received after reset; capture will keep waiting until timeout.")
                    first_byte_deadline = float("inf")
                continue
            saw_bytes = True
            log.write(line)
            log.flush()
            try:
                text = line.decode("utf-8", errors="replace").strip()
            except UnicodeDecodeError:
                continue
            if not text.startswith("AECQ "):
                continue
            try:
                event = json.loads(text[5:])
            except json.JSONDecodeError:
                continue
            events.append(event)
            typ = event.get("type")
            if typ == "manual_prompt":
                countdown = int(event.get("countdown_ms", 0)) / 1000.0
                if near_audio:
                    lead_s = max(0.0, near_audio_lead_ms / 1000.0)
                    delay_s = max(0.0, countdown - lead_s)
                    print(
                        f"Manual prompt: host will play near-end audio in {delay_s:.2f}s "
                        f"({near_audio})."
                    )
                    time.sleep(delay_s)
                    playback_proc = start_near_playback(near_audio)
                else:
                    print(f"Manual prompt: start the near-end voice source now ({countdown:.0f}s countdown).")
            elif typ == "case_start":
                print(f"Case start: {event.get('case_id')}")
            elif typ == "case_end":
                print(f"Case end: {event.get('case_id')}")
            elif typ == "done":
                print("Device test complete.")
                stop_playback(playback_proc)
                break
            elif typ == "error":
                raise SystemExit(f"Device error: {event}")
            elif typ == "audio":
                case_id = str(event.get("case_id"))
                stream_id = str(event.get("stream_id"))
                audio.setdefault(case_id, {}).setdefault(stream_id, []).append(decode_audio_payload(event))
    stop_playback(playback_proc)
    return {"events": events, "audio": audio, "near_audio": str(near_audio) if near_audio else None}


def analyze(board: BoardVariant, port: str, capture_data: dict[str, Any], out_dir: Path) -> dict[str, Any]:
    audio: dict[str, dict[str, list[bytes]]] = capture_data["audio"]
    events: list[dict[str, Any]] = capture_data["events"]
    case_end = {
        event.get("case_id"): event
        for event in events
        if event.get("type") == "case_end"
    }
    done = any(event.get("type") == "done" for event in events)
    run_status = "complete" if done else "incomplete"
    if not events:
        run_status = "no_aecq_events"

    raw_dir = out_dir / "raw"
    for case_id, streams in audio.items():
        for stream_id, payloads in streams.items():
            save_wav(raw_dir / f"{case_id}.{stream_id}.wav", payloads)

    metrics: dict[str, Any] = {}
    for case_id, streams in audio.items():
        raw_rms = rms_from_pcm16(streams.get("raw_mic", []))
        aec_rms = rms_from_pcm16(streams.get("aec_out", []))
        ref_rms = rms_from_pcm16(streams.get("reference", []))
        erle = 20.0 * math.log10(raw_rms / aec_rms) if raw_rms > 0 and aec_rms > 0 else 0.0
        metrics[case_id] = {
            "raw_rms": raw_rms,
            "raw_dbfs": dbfs(raw_rms),
            "reference_rms": ref_rms,
            "reference_dbfs": dbfs(ref_rms),
            "aec_rms": aec_rms,
            "aec_dbfs": dbfs(aec_rms),
            "erle_db": erle,
            "device_stats": case_end.get(case_id, {}),
        }

    far = metrics.get("far_playback", {})
    barge = metrics.get("barge_in", {})
    clipped_samples = sum(
        int(event.get("clipped_samples", 0))
        for event in case_end.values()
        if isinstance(event, dict)
    )
    raw_frames = sum(
        int(event.get("raw_frames", 0))
        for event in case_end.values()
        if isinstance(event, dict)
    )
    clip_ratio = clipped_samples / max(1, raw_frames * 320)
    far_erle = float(far.get("erle_db", 0.0))
    far_residual = float(far.get("aec_dbfs", 0.0))
    barge_erle = float(barge.get("erle_db", 0.0))
    far_erle_pass = far_erle >= AEC_THRESHOLDS["barge_in_erle_db"]
    far_residual_pass = far_residual <= AEC_THRESHOLDS["barge_in_residual_dbfs"]
    ptt_erle_pass = far_erle >= AEC_THRESHOLDS["ptt_erle_db"]
    clipping_pass = clip_ratio <= AEC_THRESHOLDS["max_clip_ratio"]

    if run_status != "complete":
        decision = "fail"
    elif far_erle_pass and far_residual_pass and clipping_pass:
        decision = "barge-in"
    elif ptt_erle_pass and clipping_pass:
        decision = "PTT"
    else:
        decision = "fail"

    decision_reasons = build_decision_reasons(
        decision,
        run_status,
        far_erle,
        far_residual,
        barge_erle,
        clip_ratio,
        far_erle_pass,
        far_residual_pass,
        ptt_erle_pass,
        clipping_pass,
    )

    return {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "board": {
            "name": board.name,
            "full_name": board.full_name,
            "board_path": board.board_path,
            "target": board.target,
            "board_symbol": board.board_symbol,
        },
        "port": port,
        "run_status": run_status,
        "thresholds": AEC_THRESHOLDS,
        "threshold_profile": THRESHOLD_PROFILE,
        "test_audio": {
            "near_audio": capture_data.get("near_audio"),
        },
        "metrics": metrics,
        "derived": {
            "far_playback_erle_db": far_erle,
            "far_playback_residual_dbfs": far_residual,
            "barge_in_erle_db": barge_erle,
            "clip_ratio": clip_ratio,
        },
        "decision": decision,
        "decision_reasons": decision_reasons,
        "events": events,
    }


def build_decision_reasons(
    decision: str,
    run_status: str,
    far_erle: float,
    far_residual: float,
    barge_erle: float,
    clip_ratio: float,
    far_erle_pass: bool,
    far_residual_pass: bool,
    ptt_erle_pass: bool,
    clipping_pass: bool,
) -> list[str]:
    if run_status != "complete":
        return [
            f"测试未完整完成，run_status={run_status}；本次结论不能作为 AEC 能力准入结果。",
        ]

    reasons: list[str] = []
    if decision == "barge-in":
        reasons.append(
            "远端播放 ERLE、残余回声和 clipping 均达到 barge-in 门槛，可按 barge-in 模式接入。"
        )
    elif decision == "PTT":
        reasons.append(
            "AEC 有明显效果，达到 PTT 门槛，但未同时达到 barge-in 的远端播放 ERLE/残余回声门槛。"
        )
    else:
        reasons.append("AEC 未达到 PTT 的最低门槛，或存在 clipping/dropout 等基础风险。")

    erle_threshold = AEC_THRESHOLDS["barge_in_erle_db"]
    residual_threshold = AEC_THRESHOLDS["barge_in_residual_dbfs"]
    ptt_threshold = AEC_THRESHOLDS["ptt_erle_db"]
    clip_threshold = AEC_THRESHOLDS["max_clip_ratio"]

    reasons.append(
        f"远端播放 ERLE {far_erle:.2f} dB "
        f"{'达到' if far_erle_pass else '未达到'} barge-in 门槛 {erle_threshold:.2f} dB"
        + ("" if far_erle_pass else f"，差 {erle_threshold - far_erle:.2f} dB")
        + "。"
    )
    reasons.append(
        f"远端播放 AEC 后残余 {far_residual:.2f} dBFS "
        f"{'达到' if far_residual_pass else '未达到'} barge-in 门槛 < {residual_threshold:.2f} dBFS"
        + ("" if far_residual_pass else f"，还高 {far_residual - residual_threshold:.2f} dB")
        + "。"
    )
    reasons.append(
        f"PTT 基线 ERLE 门槛 {ptt_threshold:.2f} dB："
        f"{'通过' if ptt_erle_pass else '未通过'}。"
    )
    reasons.append(
        f"clipping ratio {clip_ratio:.6f}，门槛 <= {clip_threshold:.6f}："
        f"{'通过' if clipping_pass else '未通过'}。"
    )
    reasons.append(f"barge-in 场景 ERLE {barge_erle:.2f} dB，作为 double-talk 参考指标记录。")
    return reasons


def write_summary(report: dict[str, Any], out_dir: Path) -> None:
    derived = report["derived"]
    decision_reasons = report.get("decision_reasons", [])
    lines = [
        "# AEC Qualification Summary",
        "",
        f"- Board: {report['board']['full_name']}",
        f"- Port: {report['port']}",
        f"- Run status: {report.get('run_status', 'unknown')}",
        f"- Decision: {report['decision']}",
        f"- Threshold profile: {report.get('threshold_profile', {}).get('name', 'unknown')} "
        "(engineering default, not an official certification benchmark)",
        f"- Far playback ERLE: {derived['far_playback_erle_db']:.2f} dB",
        f"- Far playback residual: {derived['far_playback_residual_dbfs']:.2f} dBFS",
        f"- Barge-in ERLE: {derived['barge_in_erle_db']:.2f} dB",
        f"- Clip ratio: {derived['clip_ratio']:.6f}",
        "",
        "## Decision Rationale",
        "",
        *[f"- {reason}" for reason in decision_reasons],
        "",
        "Raw WAV previews are under `raw/`.",
    ]
    (out_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def print_decision(report: dict[str, Any]) -> None:
    print(f"Decision: {report['decision']}")
    for reason in report.get("decision_reasons", []):
        print(f"- {reason}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run standalone ESP32 AEC qualification")
    parser.add_argument("--list-boards", action="store_true", help="List AEC qualification candidate boards")
    parser.add_argument("--list-ports", action="store_true", help="List detected serial ports")
    parser.add_argument("--json", action="store_true", help="Use JSON output with --list-boards")
    parser.add_argument("--board", help="Board name/full name/path to test")
    parser.add_argument("--port", help="Serial port")
    parser.add_argument("--auto-port", action="store_true", help="Auto-select a serial port even when multiple ports exist")
    parser.add_argument("--out-dir", default="reports/aec_qualification", help="Output directory root")
    parser.add_argument("--build-dir", default="build/aec_qualification", help="Build directory root")
    parser.add_argument("--build-only", action="store_true", help="Build the qualification firmware and exit")
    parser.add_argument("--skip-build", action="store_true", help="Do not build before flashing")
    parser.add_argument("--skip-flash", action="store_true", help="Do not flash before capture")
    parser.add_argument("--capture-timeout", type=int, default=180, help="Serial capture timeout in seconds")
    parser.add_argument("--no-capture-reset", action="store_true", help="Do not reset the device after opening serial capture")
    parser.add_argument("--capture-boot-wait", type=float, default=1.0, help="Seconds to wait after capture reset")
    parser.add_argument("--near-audio", help="Near-end WAV file to play during barge-in; defaults to generated test audio")
    parser.add_argument("--no-near-playback", action="store_true", help="Disable host playback and use manual near-end audio")
    parser.add_argument(
        "--near-audio-lead-ms",
        type=int,
        default=250,
        help="Start host near-end audio this many milliseconds before the device barge-in window",
    )
    args = parser.parse_args()

    boards = collect_boards()
    if args.list_ports:
        ports = list_ports()
        if args.json:
            print(json.dumps(ports, indent=2, ensure_ascii=False))
        elif ports:
            print(format_ports(ports))
        else:
            print("No serial ports found.")
        return 0

    if args.list_boards:
        if args.json:
            print(json.dumps([b.__dict__ for b in boards], indent=2, ensure_ascii=False))
        else:
            for board in boards:
                print(f"{board.full_name}\t{board.target}\t{board.board_path}\t{board.board_symbol}")
        return 0

    if not args.board:
        parser.error("--board is required unless --list-boards is used")

    board = find_board(args.board)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", board.full_name)
    out_dir = (PROJECT_ROOT / args.out_dir / f"{timestamp}-{safe_name}").resolve()
    build_dir = (PROJECT_ROOT / args.build_dir / safe_name).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if not args.skip_build:
        build_firmware(board, build_dir, out_dir)
    if args.build_only:
        print(f"Build directory: {build_dir}")
        print(f"Generated config/report directory: {out_dir}")
        return 0

    port = resolve_port(args.port, auto_port=args.auto_port)
    if not args.skip_flash:
        flash_firmware(build_dir, port)

    near_audio = None if args.no_near_playback else prepare_near_audio(args.near_audio, out_dir)
    capture_data = capture(
        port,
        out_dir,
        args.capture_timeout,
        near_audio,
        args.near_audio_lead_ms,
        not args.no_capture_reset,
        args.capture_boot_wait,
    )
    report = analyze(board, port, capture_data, out_dir)
    (out_dir / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    write_summary(report, out_dir)

    print(f"Report: {out_dir / 'report.json'}")
    print(f"Summary: {out_dir / 'summary.md'}")
    print_decision(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
