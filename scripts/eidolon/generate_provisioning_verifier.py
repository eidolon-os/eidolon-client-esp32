#!/usr/bin/env python3
"""Generate the SRP6a salt and verifier a device carries for setup.

One script for both kinds of build, because the provisioning act is one act:
a development build passes the shared development passphrase, a production
build passes that device's own. Only the values differ.

    ./scripts/eidolon/generate_provisioning_verifier.py --password <passphrase>

Prints the two Kconfig lines to set. The passphrase itself is never written to
the device: only the verifier derived from it is, so a device that is read out
does not yield the code needed to set up the next one.
"""

import argparse
import os
import sys


def _load_srp6a():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("error: IDF_PATH is not set; source the ESP-IDF export script first")
    tools = os.path.join(idf_path, "tools", "esp_prov")
    source = os.path.join(tools, "security", "srp6a.py")
    if not os.path.isfile(source):
        sys.exit(f"error: {source} not found; is this a complete ESP-IDF checkout?")
    # Loaded by path rather than imported as `security.srp6a`: that package's
    # __init__ pulls in the protobuf-based security1 module, which needs
    # dependencies this script does not otherwise require.
    sys.path.insert(0, tools)
    import importlib.util

    spec = importlib.util.spec_from_file_location("eidolon_srp6a", source)
    if spec is None or spec.loader is None:
        sys.exit(f"error: could not load {source}")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as exc:  # pragma: no cover - depends on the local IDF
        sys.exit(f"error: could not load ESP-IDF's SRP6a helper: {exc}")
    return module.generate_salt_and_verifier


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--username",
        default="eidolon-setup",
        help="SRP6a identity; must match what the controller uses (default: eidolon-setup)",
    )
    parser.add_argument("--password", required=True, help="setup passphrase")
    parser.add_argument("--salt-len", type=int, default=16, help="salt length in bytes")
    args = parser.parse_args()

    generate_salt_and_verifier = _load_srp6a()
    salt, verifier = generate_salt_and_verifier(
        args.username, args.password, len_s=args.salt_len
    )

    print(f'CONFIG_EIDOLON_PROVISIONING_SALT_HEX="{bytes(salt).hex()}"')
    print(f'CONFIG_EIDOLON_PROVISIONING_VERIFIER_HEX="{bytes(verifier).hex()}"')


if __name__ == "__main__":
    main()
