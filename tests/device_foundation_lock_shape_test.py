"""The lock has to be in a shape that can name bytes, or the sync means nothing.

The regression this pins is not hypothetical: `sdk_commit` was a whole 40-character
id for three entries, then one entry shortened it to seven characters and the next
copied that neighbour. Git resolves an abbreviation against whatever the repository
holds at the time, so it is a name that can start meaning something else, or stop
resolving at all, as history grows. `472ce572` corrected the value; without this
test the same abbreviation goes back in with nothing turning red.
"""

import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import sync_device_foundation_v1 as sync  # noqa: E402


WHOLE = "c66b990fe490f53a38dd529a264e2fe8188de988"


def loads(document: object) -> dict:
    """Load `document` as if it were the repository's lock file."""

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "lock.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        original = sync.LOCK_PATH
        sync.LOCK_PATH = path
        try:
            return sync.load_lock()
        finally:
            sync.LOCK_PATH = original


def refuses(document: object) -> str:
    try:
        loads(document)
    except sync.LockCannotNameBytes as refusal:
        return str(refusal)
    raise AssertionError(f"the lock was accepted and should not have been: {document!r}")


def main() -> None:
    # The control case, and it has to match something: a lock whose file list
    # were empty would let every assertion below hold while the sync checked
    # nothing at all. That is the failure mode the golden contract test guards
    # against by asserting it matched a vector, and it applies here too.
    shipped = json.loads(
        (ROOT / "device_foundation_sdk.lock.json").read_text(encoding="utf-8")
    )
    assert len(shipped["files"]) >= 13, "the shipped lock stopped naming its files"
    assert loads(shipped) == shipped

    good = {"sdk_commit": WHOLE, "files": shipped["files"]}
    assert loads(good)["sdk_commit"] == WHOLE

    # The abbreviation itself, at the length it actually regressed to.
    message = refuses({**good, "sdk_commit": WHOLE[:7]})
    assert "sdk_commit" in message, message
    assert "40" in message, message

    for wrong in (
        WHOLE[:39],
        WHOLE + "0",
        WHOLE.upper(),
        WHOLE[:39] + "g",
        f" {WHOLE}",
        "",
        None,
        40,
        ["c"] * 40,
    ):
        refuses({**good, "sdk_commit": wrong})

    refuses({"files": shipped["files"]})

    # A lock that names no files passes every byte comparison it makes, because
    # it makes none.
    for empty in ({**good, "files": []}, {"sdk_commit": WHOLE}):
        assert "verify nothing" in refuses(empty)

    # The refusal is reported, not raised: a traceback out of a sync tool reads
    # like a broken tool rather than a lock waiting to be repointed.
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "lock.json"
        path.write_text(json.dumps({**good, "sdk_commit": WHOLE[:7]}), encoding="utf-8")
        original_lock, original_argv = sync.LOCK_PATH, sys.argv
        sync.LOCK_PATH = path
        sys.argv = ["sync_device_foundation_v1.py", "--check"]
        stderr = io.StringIO()
        try:
            with contextlib.redirect_stderr(stderr):
                status = sync.main()
        finally:
            sync.LOCK_PATH, sys.argv = original_lock, original_argv
    assert status == 1, status
    assert "sdk_commit" in stderr.getvalue(), stderr.getvalue()

    print("device-foundation lock shape: PASS")


if __name__ == "__main__":
    main()
