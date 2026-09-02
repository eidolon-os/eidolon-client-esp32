"""One executable answer to "does device admission work right now".

Everything this checks was proved by hand on 2026-09-02 — driving adb, reading
uiautomator dumps, and querying the Hub's sqlite over ssh. That evidence was
real and it was not repeatable, which is its own kind of unproven: nobody could
tell whether a later change had broken it without doing the whole afternoon
again. This file is that afternoon, as checkpoints that pass or fail.

Where it lives, and what it is not: the runner produces its own evidence bundle
and never writes to the SDK requirement manifest, which stays a reviewed static
registry (see the plan's runner rules). Case ids here are the plan's own —
F-016, F-017, F-018, F-020 — so a result maps back to a case without a second
naming scheme to keep in step.

Host facts are read from eidolon_ops' config rather than repeated here. A
second copy of "which machine is the Host" is how a test ends up passing
against a machine nobody is shipping.

Honesty rules this file follows, because a green suite that skips quietly is
worse than no suite:
  * a checkpoint that needs a human hand reports NEEDS-HAND, never PASS;
  * a checkpoint that could not run reports BLOCKED with the reason;
  * every PASS prints the value that decided it.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _ops_config() -> Path:
    """Where ops records which machine is the Host.

    Not "the sibling of this checkout": a linked worktree sits somewhere else
    entirely, and hard-coding the sibling made this runner unusable from one.
    The candidates are the env override, this checkout's sibling, and the
    sibling of the primary worktree — which is where the repository really
    lives however this copy was made.
    """

    override = os.environ.get("EIDOLON_OPS_CONFIG")
    if override:
        return Path(override).expanduser()
    relative = "eidolon_ops/config/eidolon-pi.toml"
    candidates = [ROOT.parent / relative]
    common = _run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute",
                   "--git-common-dir"]).stdout.strip()
    if common:
        candidates.append(Path(common).parent.parent / relative)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]
HUB_DB = "/var/lib/eidolon/hub/eidolon-hub.sqlite3"
ADB_CANDIDATES = (
    "adb",
    "/Users/manson/Developer/Android/sdk/platform-tools/adb",
    str(Path.home() / "Library/Android/sdk/platform-tools/adb"),
)


# --------------------------------------------------------------------------
# results


PASS = "PASS"
FAIL = "FAIL"
BLOCKED = "BLOCKED"
NEEDS_HAND = "NEEDS-HAND"


@dataclass
class Checkpoint:
    case: str
    name: str
    result: str
    detail: str


@dataclass
class Run:
    evidence: Path
    checkpoints: list[Checkpoint] = field(default_factory=list)

    def record(self, case: str, name: str, result: str, detail: str) -> Checkpoint:
        point = Checkpoint(case, name, result, detail)
        self.checkpoints.append(point)
        marker = {PASS: "ok", FAIL: "FAILED", BLOCKED: "blocked", NEEDS_HAND: "by hand"}
        print(f"  [{marker[result]:>8}] {case} {name}: {detail}", flush=True)
        return point

    def worst(self) -> int:
        if any(p.result == FAIL for p in self.checkpoints):
            return 1
        if any(p.result == BLOCKED for p in self.checkpoints):
            return 2
        return 0

    def write(self) -> None:
        self.evidence.mkdir(parents=True, exist_ok=True)
        (self.evidence / "checkpoints.json").write_text(
            json.dumps(
                [p.__dict__ for p in self.checkpoints], ensure_ascii=False, indent=2
            )
            + "\n",
            encoding="utf-8",
        )


class Blocked(RuntimeError):
    """The rig cannot answer this, which is not the same as a failure."""


# --------------------------------------------------------------------------
# the three surfaces this chain crosses


def _run(command: list[str], timeout: float = 60) -> subprocess.CompletedProcess:
    return subprocess.run(
        command, capture_output=True, text=True, timeout=timeout, check=False
    )


class Hub:
    """The Authority's own record, read only, over the link ops already uses."""

    def __init__(self) -> None:
        config = _ops_config()
        if not config.is_file():
            raise Blocked(
                f"{config} is not there; set EIDOLON_OPS_CONFIG to the Host's "
                "ops config"
            )
        text = config.read_text(encoding="utf-8")

        def field_of(name: str) -> str:
            match = re.search(rf'^{name}\s*=\s*"([^"]+)"', text, re.M)
            if match is None:
                raise Blocked(f"{config} has no {name}")
            return match.group(1)

        self.target = f"{field_of('user')}@{field_of('hostname')}"
        self.identity = field_of("identity_file")

    def query(self, sql: str, params: tuple = ()) -> list[tuple]:
        # The query travels inside the script, base64-encoded, because ssh
        # hands its arguments to a remote shell that would word-split both the
        # SQL and the JSON parameters — the same reason ops' own remote agent
        # encodes its payload instead of passing it as argv.
        payload = base64.urlsafe_b64encode(
            json.dumps({"sql": sql, "params": list(params)}).encode()
        ).decode()
        script = (
            "import sqlite3,json,base64\n"
            f"q=json.loads(base64.urlsafe_b64decode('{payload}'))\n"
            f"c=sqlite3.connect('file:{HUB_DB}?mode=ro',uri=True)\n"
            "print(json.dumps([list(r) for r in "
            "c.execute(q['sql'],q['params'])],default=str))"
        )
        result = subprocess.run(
            [
                "ssh", "-i", self.identity, "-o", "BatchMode=yes",
                "-o", "ConnectTimeout=10", self.target,
                "sudo", "-n", "python3", "-",
            ],
            input=script,
            capture_output=True,
            text=True,
            timeout=45,
            check=False,
        )
        if result.returncode != 0:
            raise Blocked(f"Hub is not answering: {result.stderr.strip()[:160]}")
        return [tuple(row) for row in json.loads(result.stdout)]


class Phone:
    """The Controller, driven the way a person drives it: by what is on screen.

    Taps resolve labels, not coordinates. A runner pinned to coordinates
    reports a layout change as a broken product.
    """

    PACKAGE = "live.eidolon.eidolon_client_mobile"

    def __init__(self) -> None:
        self.adb = next((c for c in ADB_CANDIDATES if shutil.which(c) or Path(c).exists()), None)
        if self.adb is None:
            raise Blocked("adb is not on this workstation")
        listed = _run([self.adb, "devices"]).stdout.splitlines()[1:]
        attached = [line.split()[0] for line in listed if line.strip().endswith("device")]
        if not attached:
            raise Blocked("no Android device is attached over adb")
        self.serial = attached[0]

    def _adb(self, *args: str, timeout: float = 60) -> subprocess.CompletedProcess:
        return _run([self.adb, "-s", self.serial, *args], timeout=timeout)

    DUMP = "/sdcard/eidolon-hil-ui.xml"

    def _screen_xml(self) -> str:
        """The screen as it is now, or nothing — never as it was last time.

        `uiautomator dump` fails while a window is in transition ("could not
        get idle state"), and a system dialog appearing is exactly such a
        moment. Reading a fixed path unconditionally then returns the PREVIOUS
        dump, so this runner spent two attempts operating on a screen that was
        no longer there: it found the join dialog's button in a stale file,
        tapped an empty spot on the candidate list, reported success, and the
        platform released the network request 64 seconds later with
        `(timeout)`. That reads exactly like a device refusing to be joined.

        So the file is removed first and the dump is required to say it
        wrote one. An empty answer means "cannot see the screen", which callers
        retry — a wrong answer is the thing there is no recovering from.
        """

        for attempt in range(4):
            self._adb("shell", "rm", "-f", self.DUMP)
            reported = self._adb("shell", "uiautomator", "dump", self.DUMP)
            if "dumped to" in reported.stdout:
                xml = self._adb("shell", "cat", self.DUMP).stdout
                if "<node" in xml:
                    return xml
            time.sleep(1 + attempt)
        return ""

    def labels(self) -> list[tuple[str, int, int, bool]]:
        dump = self._screen_xml()
        found: list[tuple[str, int, int, bool]] = []
        for node in re.finditer(r"<node[^>]*>", dump):
            raw = node.group(0)
            text = re.search(r'text="([^"]*)"', raw)
            desc = re.search(r'content-desc="([^"]*)"', raw)
            box = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', raw)
            label = (text.group(1) if text else "") or (desc.group(1) if desc else "")
            if label.strip() and box:
                x1, y1, x2, y2 = (int(v) for v in box.groups())
                found.append(
                    (label, (x1 + x2) // 2, (y1 + y2) // 2, 'clickable="true"' in raw)
                )
        return found

    def wait_for(self, needle: str, timeout: float = 40) -> tuple[str, int, int, bool]:
        """The element a person would press, not the first one that contains it.

        Substring matching alone picks whatever the dump lists first, and a
        dialog's title usually contains its button's word: Android's join
        prompt is titled "连接到设备" above a button reading "连接", so asking
        for "连接" tapped the title. The request then sat unpressed until the
        platform released it with `(timeout)` a minute later — a failure that
        reads exactly like a device refusing to be joined.

        So candidates are ranked: an exact label wins, clickable beats inert,
        and only then does dump order decide.
        """

        deadline = time.time() + timeout
        seen: list[str] = []
        blind = 0
        while time.time() < deadline:
            seen = []
            matches = []
            for label, x, y, clickable in self.labels():
                seen.append(label)
                if needle in label:
                    matches.append((label == needle, clickable, label, x, y))
            if matches:
                matches.sort(key=lambda m: (m[0], m[1]), reverse=True)
                _exact, clickable, label, x, y = matches[0]
                return label, x, y, clickable
            if not seen:
                blind += 1
            time.sleep(2)
        if blind and not seen:
            raise Blocked(
                f"the screen could not be read on {blind} attempts while waiting "
                f"for {needle!r}; uiautomator never reported an idle window"
            )
        raise AssertionError(
            f"{needle!r} never appeared. On screen: {sorted(set(seen))[:12]}"
        )

    def tap(self, needle: str, timeout: float = 40) -> str:
        label, x, y, _clickable = self.wait_for(needle, timeout)
        self._adb("shell", "input", "tap", str(x), str(y))
        return label

    def type_into_the_only_field(self, value: str) -> None:
        dump = self._screen_xml()
        fields = [
            box.groups()
            for node in re.finditer(r"<node[^>]*>", dump)
            if "EditText" in node.group(0)
            for box in [re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', node.group(0))]
            if box
        ]
        if len(fields) != 1:
            raise AssertionError(f"expected one text field on screen, saw {len(fields)}")
        x1, y1, x2, y2 = (int(v) for v in fields[0])
        self._adb("shell", "input", "tap", str((x1 + x2) // 2), str((y1 + y2) // 2))
        time.sleep(2)
        self._adb("shell", "input", "text", value)
        time.sleep(1)
        # Put the keyboard away so the confirm control is on screen again.
        self._adb("shell", "input", "keyevent", "KEYCODE_BACK")
        time.sleep(2)

    def restart_app(self) -> None:
        self._adb("shell", "am", "force-stop", self.PACKAGE)
        time.sleep(2)
        self._adb("shell", "am", "start", "-n", f"{self.PACKAGE}/.MainActivity")
        time.sleep(8)

    def scroll_down(self) -> None:
        self._adb("shell", "input", "swipe", "1068", "2600", "1068", "1400", "300")
        time.sleep(2)


class Device:
    """The Body, read over its serial line.

    Opening the port resets an ESP32-S3, which is why this is explicit: a
    reboot is a step in the test, not a side effect of looking.
    """

    IDF_PYTHON = Path.home() / ".espressif/python_env/idf5.5_py3.13_env/bin/python"

    def __init__(self, port: str) -> None:
        self.port = port
        if not Path(port).exists():
            raise Blocked(f"{port} is not present; is the device plugged in?")

    def capture(self, seconds: float, out: Path, reset: bool = True) -> str:
        script = (
            "import sys,time,serial\n"
            "p=serial.Serial(sys.argv[1],115200,timeout=1)\n"
            "p.dtr=False\n"
            "reset = sys.argv[4]=='reset'\n"
            "if reset:\n"
            "    p.rts=True; time.sleep(0.1); p.rts=False\n"
            "f=open(sys.argv[3],'w',buffering=1)\n"
            "end=time.time()+float(sys.argv[2])\n"
            "while time.time()<end:\n"
            "    raw=p.readline().decode('utf-8','replace').rstrip()\n"
            "    if raw: f.write(time.strftime('%H:%M:%S ')+raw+'\\n')\n"
            "p.close(); f.close()\n"
        )
        interpreter = str(self.IDF_PYTHON) if self.IDF_PYTHON.exists() else sys.executable
        out.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [interpreter, "-", self.port, str(seconds), str(out),
             "reset" if reset else "keep"],
            input=script,
            capture_output=True,
            text=True,
            timeout=seconds + 60,
            check=False,
        )
        if result.returncode != 0:
            raise Blocked(
                "could not read the device's serial line "
                f"(is another session holding {self.port}?): "
                f"{result.stderr.strip()[:160]}"
            )
        return out.read_text(encoding="utf-8")

    @staticmethod
    def says(log: str, needle: str) -> bool:
        return needle in log


# --------------------------------------------------------------------------
# checkpoints, named by the plan's own case ids


def check_rig(run: Run, port: str) -> dict:
    """Refuse to report on a rig that is not the one under test.

    A run against a device carrying yesterday's firmware answers a question
    nobody asked, and answers it green.
    """

    head = _run(["git", "-C", str(ROOT), "rev-parse", "--short=9", "HEAD"]).stdout.strip()
    dirty = bool(_run(["git", "-C", str(ROOT), "status", "--porcelain"]).stdout.strip())
    expected = f"{head}{'+dirty' if dirty else ''}"

    device = Device(port)
    log = device.capture(20, run.evidence / "rig-boot.log")
    stamp = re.search(r"EIDOLON-BUILDSTAMP git=(\S+)", log)
    if stamp is None:
        raise Blocked("the device did not print a build stamp in 20s")
    if stamp.group(1) != expected:
        run.record(
            "rig", "firmware is this working tree", FAIL,
            f"device runs git={stamp.group(1)}, this tree is {expected}",
        )
    else:
        run.record("rig", "firmware is this working tree", PASS, f"git={expected}")

    profile = re.search(r"EIDOLON-PROFILE (\S+.*)$", log, re.M)
    run.record(
        "rig", "device profile", PASS if profile else BLOCKED,
        profile.group(1) if profile else "no EIDOLON-PROFILE line",
    )
    return {"boot_log": log, "device": device}


def check_one_image(run: Run) -> None:
    """F-016, on one board: nothing in this image is per-device.

    The two-board half of F-016 needs a second board, which this round
    deliberately does not touch. What can be held here is the property that
    made two boards possible: no per-device constant is compiled in at all.
    """

    leftovers = _run(
        ["git", "-C", str(ROOT), "grep", "-lE",
         "EIDOLON_ADMISSION_SETUP_SECRET|setup_secret", "--", "main", "scripts"]
    ).stdout.strip()
    if leftovers:
        run.record("F-016", "no per-device secret in the image", FAIL, leftovers)
    else:
        run.record(
            "F-016", "no per-device secret in the image", PASS,
            "no EIDOLON_ADMISSION_SETUP_SECRET / setup_secret in main or scripts",
        )
    run.record(
        "F-016", "same image on a second board", NEEDS_HAND,
        "needs a second board flashed from this artifact; korvo-1 is out of scope this round",
    )


def reach_the_setup_page(run: Run, phone: Phone) -> None:
    """Get to "find a device", from wherever the app was left.

    Restarting the app is not a clean slate. The setup page persists a
    checkpoint and restores it, so a run can open straight onto the previous
    attempt — including its dead end, which is exactly the state this rig is
    left in by a removal. That screen is not an obstacle to work around: it
    says what to do next ("重新设置设备"), and pressing it is the path a person
    takes. It clears the checkpoint and asks for nothing else.
    """

    def on_screen() -> str:
        return " ".join(label for label, _x, _y, _c in phone.labels())

    phone.restart_app()
    if "查找设备" not in on_screen():
        phone.tap("主机已保存")
        phone.tap("打开我的 Eidolon")
        phone.wait_for("已安全连接")
        phone.scroll_down()
        phone.tap("打开设备管理")
        phone.tap("配置新设备网络")

    # The checkpoint is restored when this page opens, not when the app
    # starts, so the previous attempt appears only now — after the
    # navigation, which is why looking for it any earlier finds a home screen
    # and learns nothing.
    if "重新设置设备" in on_screen():
        run.record(
            "rig", "cleared a restored setup attempt", PASS,
            "the page reopened on a previous attempt and offered to start over",
        )
        phone.tap("重新设置设备")


def add_the_device(run: Run, phone: Phone, wifi_ssid: str, wifi_password: str) -> None:
    """The everyday path, driven the way an Owner drives it."""

    reach_the_setup_page(run, phone)
    phone.tap("查找设备")
    phone.tap("softap", timeout=60)
    phone.tap("连接", timeout=40)

    # Two visits, and the Host is asked between them. The phone joins the
    # device's access point with the radio it reaches the Host on, so this
    # ordering is a physical constraint, not a preference.
    phone.wait_for("选择家庭 Wi-Fi", timeout=90)
    run.record(
        "F-016", "descriptor read and standing signed", PASS,
        "the Host signed this device's voucher between the two visits",
    )

    phone.tap(wifi_ssid)
    phone.type_into_the_only_field(wifi_password)
    phone.tap("确认配网并批准这次设备接入")
    phone.tap("连接", timeout=60)
    landed = phone.wait_for("尚未 ClaimActive", timeout=180)[0]
    run.record(
        "F-016", "network and trust handed over", PASS, landed.splitlines()[0]
    )


def check_hub_minted_one_identity(
    run: Run, hub: Hub, expect_active: bool
) -> str:
    """The Authority's record is the oracle, not the screen.

    One voucher, spent at the instant the identity was bound, bound to the key
    the device can prove and to no other. Those three hold whatever lifecycle
    state the newest Claim is in, so they are asserted either way.

    Whether that Claim must be *active* depends on what this run did.
    `expect_active` is set only when this run just added the device; asked on
    its own against a rig whose device was deliberately removed, the state is
    reported rather than judged. The flag is not a way to soften a failure: if
    the add stage ran, an inactive Claim is a failure and there is no path here
    that says otherwise.
    """

    claims = hub.query(
        "select device_instance_id, state, claim_generation from admission_claims_v1"
        " order by activated_at desc limit 1"
    )
    if not claims:
        run.record("F-016", "Claim is active", FAIL, "the Hub holds no Claim at all")
        raise Blocked("nothing to assert about")
    instance, state, generation = claims[0]
    key = "sha256:" + instance.removeprefix("device-instance-")

    identities = hub.query(
        "select device_base_id, operational_key_id, provenance, bound_at"
        " from admission_base_identities_v1 where operational_key_id = ?",
        (key,),
    )
    vouchers = hub.query(
        "select jti, device_base_id, consumed_at from"
        " admission_commissioning_vouchers_v1 where operational_key_id = ?",
        (key,),
    )
    proposals = hub.query(
        "select enrollment_id, state from admission_proposals_v1"
        " where operational_key_id = ?",
        (key,),
    )

    if expect_active:
        run.record(
            "F-016", "Claim is active", PASS if state == "active" else FAIL,
            f"{instance} state={state} gen={generation}",
        )
    else:
        run.record(
            "F-016", "newest Claim (state reported, not judged)", PASS,
            f"{instance} state={state} gen={generation}"
            " — run the add stage to require active",
        )

    if len(identities) == 1:
        base_id, _key, provenance, bound_at = identities[0]
        run.record(
            "F-016", "one base identity, minted, bound to this key", PASS,
            f"{base_id} provenance={provenance}",
        )
    else:
        run.record(
            "F-016", "one base identity, minted, bound to this key", FAIL,
            f"{len(identities)} identities bound to {key}",
        )
        bound_at = None

    spent = [v for v in vouchers if v[2]]
    if len(spent) == 1 and bound_at and spent[0][2] == bound_at:
        run.record(
            "F-018", "the voucher was spent binding this identity", PASS,
            f"{spent[0][0]} consumed_at == bound_at == {bound_at}",
        )
    elif len(spent) == 1:
        run.record(
            "F-018", "the voucher was spent binding this identity", FAIL,
            f"consumed_at={spent[0][2]} but bound_at={bound_at}",
        )
    else:
        run.record(
            "F-018", "the voucher was spent binding this identity", FAIL,
            f"{len(spent)} consumed vouchers for this key",
        )

    run.record(
        "F-020", "one Proposal for this key", PASS if len(proposals) == 1 else FAIL,
        ", ".join(f"{p[0]}={p[1]}" for p in proposals) or "none",
    )
    return instance


def check_removal_is_terminal(
    run: Run, phone: Phone, hub: Hub, device: Device, instance: str
) -> None:
    """F-020: a removed Body says so, and does not re-queue itself."""

    phone.restart_app()
    phone.tap("主机已保存")
    phone.tap("打开我的 Eidolon")
    phone.wait_for("已安全连接")
    phone.scroll_down()
    phone.tap("打开设备管理")
    phone.tap(instance[:32], timeout=40)
    phone.tap("移除设备")
    phone.tap("移除")
    phone.wait_for("已从平台移除", timeout=60)

    before = hub.query(
        "select count(*) from admission_proposals_v1 where operational_key_id = ?",
        ("sha256:" + instance.removeprefix("device-instance-"),),
    )[0][0]

    log = device.capture(150, run.evidence / "after-removal.log")
    if Device.says(log, "Removed from this Owner. Open setup to claim it again"):
        run.record(
            "F-020", "the device says it was removed", PASS,
            "screen reached REMOVED, not 'waiting for approval'",
        )
    else:
        reason = re.search(r"Owner instruction refused: (\S+)", log)
        run.record(
            "F-020", "the device says it was removed", FAIL,
            "never reached the removal verdict"
            + (f"; last refusal was {reason.group(1)}" if reason else ""),
        )

    after = hub.query(
        "select count(*) from admission_proposals_v1 where operational_key_id = ?",
        ("sha256:" + instance.removeprefix("device-instance-"),),
    )[0][0]
    run.record(
        "F-020", "it did not re-queue itself", PASS if after == before else FAIL,
        f"{before} Proposal(s) before the reboots, {after} after",
    )

    erased = hub.query(
        "select state, attempt_count, acknowledged_at from hub_device_erase_operations"
        " where device_id = ?",
        (instance,),
    )
    if erased and erased[0][2]:
        run.record(
            "F-020", "the Owner's erase instruction was carried out", PASS,
            f"acknowledged_at={erased[0][2]}",
        )
    elif erased:
        run.record(
            "F-020", "the Owner's erase instruction was carried out", FAIL,
            f"state={erased[0][0]} attempts={erased[0][1]} never acknowledged",
        )
    else:
        run.record(
            "F-020", "the Owner's erase instruction was carried out", FAIL,
            "the Hub recorded no erase operation for this device",
        )

    run.record(
        "F-020", "and it comes back on the same base identity", NEEDS_HAND,
        "opening setup again needs a long press on the device's own button",
    )
    run.record(
        "F-017", "an erased device is a new device", NEEDS_HAND,
        "needs a full flash erase between two runs; the Hub-side rule is held by"
        " test_erased_body_arrives_as_a_new_base_identity_and_inherits_nothing",
    )


# --------------------------------------------------------------------------
# entry point


STAGES = ("rig", "image", "add", "authority", "removal")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the device admission chain against the real rig."
    )
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--wifi-ssid", default="Manson")
    parser.add_argument(
        "--wifi-password",
        help="the Owner network's password, needed only by the 'add' stage",
    )
    parser.add_argument(
        "--stages",
        default="rig,image,authority",
        help="comma-separated subset of " + ",".join(STAGES)
        + ". The default is the read-only set: it reports on the rig as it stands"
        " without driving the phone or re-provisioning anything.",
    )
    parser.add_argument(
        "--evidence",
        default=None,
        help="where to write the bundle (default: tests/hil/evidence/<timestamp>)",
    )
    args = parser.parse_args()

    wanted = [stage.strip() for stage in args.stages.split(",") if stage.strip()]
    unknown = [stage for stage in wanted if stage not in STAGES]
    if unknown:
        parser.error(f"unknown stage(s): {', '.join(unknown)}")
    if "add" in wanted and not args.wifi_password:
        parser.error("--wifi-password is required by the 'add' stage")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    evidence = Path(args.evidence) if args.evidence else Path(__file__).parent / "evidence" / stamp
    run = Run(evidence=evidence)
    print(f"device admission HIL — stages: {', '.join(wanted)}")
    print(f"evidence: {evidence}")

    device: Device | None = None
    instance: str | None = None

    for stage in wanted:
        print(f"\n{stage}:")
        try:
            if stage == "rig":
                device = check_rig(run, args.port)["device"]
            elif stage == "image":
                check_one_image(run)
            elif stage == "add":
                add_the_device(run, Phone(), args.wifi_ssid, args.wifi_password)
            elif stage == "authority":
                instance = check_hub_minted_one_identity(
                    run, Hub(), expect_active="add" in wanted
                )
            elif stage == "removal":
                if device is None:
                    device = Device(args.port)
                if instance is None:
                    instance = check_hub_minted_one_identity(
                        run, Hub(), expect_active=False
                    )
                check_removal_is_terminal(run, Phone(), Hub(), device, instance)
        except Blocked as blocked:
            run.record(stage, "could not run", BLOCKED, str(blocked))
        except AssertionError as broken:
            run.record(stage, "the rig did not do what this stage needs", FAIL, str(broken))

    run.write()
    counts = {
        result: sum(1 for p in run.checkpoints if p.result == result)
        for result in (PASS, FAIL, BLOCKED, NEEDS_HAND)
    }
    print(
        f"\n{counts[PASS]} pass, {counts[FAIL]} fail, {counts[BLOCKED]} blocked, "
        f"{counts[NEEDS_HAND]} need a hand"
    )
    if counts[NEEDS_HAND]:
        print("A checkpoint needing a hand is not a pass. It is listed above.")
    return run.worst()


if __name__ == "__main__":
    sys.exit(main())
