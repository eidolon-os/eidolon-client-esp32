#!/usr/bin/env python3
"""Sync the exact canonical Device Foundation inputs from the pinned SDK commit."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = REPO_ROOT / "device_foundation_sdk.lock.json"
WHOLE_COMMIT = re.compile(r"[0-9a-f]{40}")


class LockCannotNameBytes(RuntimeError):
    """The lock is not in a shape that can say which bytes it means."""


def load_lock() -> dict:
    """The lock document, refused at the entry unless it can name bytes.

    `sdk_commit` is resolved by git against whatever the SDK repository holds
    at the time, so an abbreviation is a name that can start meaning something
    else, or stop resolving at all, as history grows — for the one field whose
    whole job is to say which bytes. It was a whole id for three entries, then
    one shortened it to seven and the next copied that neighbour, and nothing
    was observably wrong at 157 commits. That is why the rule is here and not
    in review: it is cheap to violate for a long time before it is expensive.
    eidolon_ops holds the same rule for release revisions
    (`config.py:_require_revision`) but cannot be imported from a firmware
    repository, so it is restated rather than depended on.

    An empty file list is refused for the reason the golden contract test
    asserts it matched something: a check with nothing to check passes.
    """

    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    commit = lock.get("sdk_commit")
    if not isinstance(commit, str) or WHOLE_COMMIT.fullmatch(commit) is None:
        raise LockCannotNameBytes(
            f"sdk_commit must be a whole 40-character lowercase commit id, not {commit!r}"
        )
    if not lock.get("files"):
        raise LockCannotNameBytes("the lock names no files, so a check would verify nothing")
    return lock


class MissingCanonicalSource(RuntimeError):
    """The pinned SDK commit does not contain a file this lock claims.

    That is the normal state while a contract change is still uncommitted in the
    SDK: the canonical file exists in the working tree and nowhere in history
    yet. Saying so is worth a sentence, because the alternative was a git
    traceback that read like a broken tool rather than a lock waiting to be
    repointed at the commit that carries the change.
    """


def sdk_blob(sdk_repo: Path, commit: str, source: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(sdk_repo), "show", f"{commit}:{source}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if result.returncode != 0:
        raise MissingCanonicalSource(
            f"{source} is not in SDK commit {commit}: repoint sdk_commit at the "
            f"commit that carries it"
        )
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if any target drifts")
    parser.add_argument(
        "--sdk-repo",
        type=Path,
        default=Path(
            os.environ.get(
                "EIDOLON_SDK_REPO", str(REPO_ROOT.parent / "eidolon_sdk")
            )
        ),
        help="local eidolon_sdk repository containing the pinned commit",
    )
    args = parser.parse_args()
    try:
        lock = load_lock()
    except LockCannotNameBytes as invalid:
        # Reported rather than raised, for the same reason MissingCanonicalSource
        # is: a traceback out of a sync tool reads like a broken tool.
        print(str(invalid), file=sys.stderr)
        return 1
    failures: list[str] = []
    for item in lock["files"]:
        try:
            data = sdk_blob(args.sdk_repo, lock["sdk_commit"], item["source"])
        except MissingCanonicalSource as missing:
            failures.append(str(missing))
            continue
        digest = hashlib.sha256(data).hexdigest()
        if digest != item["sha256"]:
            failures.append(
                f"canonical digest mismatch for {item['source']}: {digest}"
            )
            continue
        target = REPO_ROOT / item["target"]
        if args.check:
            if not target.is_file() or target.read_bytes() != data:
                failures.append(f"generated drift: {item['target']}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"device-foundation SDK sync {'check' if args.check else 'update'}: PASS "
        f"({lock['sdk_commit']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
