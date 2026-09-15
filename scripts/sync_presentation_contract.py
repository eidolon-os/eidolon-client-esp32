#!/usr/bin/env python3
"""Copy generated public presentation contracts from the sibling SDK checkout."""
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk-root", type=Path, default=root.parent / "eidolon_sdk")
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
source = args.sdk_root / "contracts/presentation/v1"
files = {
    "golden/companion-manifest.json": "tests/fixtures/presentation/companion-manifest.json",
    "embedded/output_catalog.h": "main/eidolon/expression/generated/output_catalog.h",
    "embedded/presentation_catalog.h": "main/eidolon/expression/generated/presentation_catalog.h",
    "golden/expression-plan.json": "tests/fixtures/presentation/expression-plan.json",
    "golden/silent-session.json": "tests/fixtures/presentation/silent-session.json",
}
for name, target in files.items():
    src, dst = source / name, root / target
    if args.check:
        if not dst.is_file() or src.read_bytes() != dst.read_bytes():
            raise SystemExit(f"Stale contract: {dst}")
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(src.read_bytes())
