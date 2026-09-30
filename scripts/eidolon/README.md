# Eidolon board build and flash contract

Use the board's script under `scripts/eidolon/`. All six board scripts call
`eidolon_flash` in `eidolon-common.sh`: build → preflight → IDF write → partition
readback → official OTA activation → runtime build-stamp verification. There is
no project `idf_ext.py` hook. Direct `idf.py`, raw esptool, Ninja flash targets and
external flashers are outside this verified entry point.

## New board

1. Declare the chip, flash capacity and pinned ESP-IDF version in the board
   configuration. Keep interaction mode and output capabilities independent.
2. Select a compatible partition CSV. Prefer `partitions/v2/16m_eidolon.csv` for
   compatible 16 MB boards; BOX-3 uses its asset/application size variant.
   Do not copy offsets into shell scripts or board C++ code.
3. Build using the board script. Hub builds require `owner_trust` (data/nvs,
   at least the capacity in `owner_trust_storage_policy.h`), `nvs`, `otadata`,
   equal-sized OTA slots and `assets`. IDF verifies names, overlap and alignment.
   The generated binary table must match the configured CSV, and flash images
   must fit their destinations. Missing requirements fail the build.
4. Connect hardware and flash through the same script. The shared gate checks
   actual chip, flash capacity and existing binary partition table before
   invoking IDF's writer, then reads the partition table back. The board
   independent gate then activates the application partition named by IDF's
   build artifacts using the official `otatool`, for `flash` and `app-flash`.
   This entry supports complete or application-only maintenance flashing.
   Activation happens only after a successful write and readback; activation
   failures fail the command. No private OTA sequence/CRC implementation or
   board-specific slot offsets are used. The board
   script additionally checks the running build fingerprint. Startup reports
   `EIDOLON-STORAGE owner_trust=ready bytes=...` after successful initialization.
5. Verify factory-empty provisioning and normal interaction on hardware.
   Compile-time storage checks do not replace those acceptance tests.

Namespace and key names are internal to the shared storage implementation.
Flash scripts never create namespaces. All read paths treat absent storage
content as unclaimed and unreadable/corrupt content as unavailable.

## Existing devices

A matching layout permits an update. An erased partition table permits only a
complete flash. A different or unreadable table stops the operation before
writing. No script automatically erases or relocates ownership and user data.
Plan a data-preserving migration, or explicitly authorize and perform a full
erase before the complete flash. Application-only flashing cannot install a new
partition layout. Read-only compatibility in the firmware does not bypass this
release/flash requirement.

Application-only writes previously left OTA selection unchanged. Any dual-slot
board booting the other slot could therefore keep running old firmware despite
successful write/hash verification; this was observed on BOX-3, not specific to
BOX-3. The shared gate now completes write + boot selection; running build-stamp
verification remains mandatory in board scripts. This is a local maintenance
flash flow, not a power-loss-atomic OTA update or a guaranteed rollback scheme.
It updates `otadata`; it does not erase Owner trust/NVS/assets or rewrite the
other application slot. Direct `idf.py` does not provide these script-level guarantees.

BOX-3 supports the same application-only operation without sourcing its shell
internals: `EIDOLON_PORT=/dev/cu.usbmodemXXXX bash scripts/eidolon/eidolon-esp-box-3.sh flash --app-only`.

Encrypted flash and extra raw esptool write arguments are refused by this gate
until an explicit verification flow exists for them. The old
`--only-flash-partition` option is not supported by the pinned IDF; use the
supported application-only or full-image operation. Explicit partition erase
uses IDF parttool to query the connected device's own table.

The historical `16m_atk_guard.csv` has no Owner partition and intentionally fails
Hub contract checks. Its face data layout needs an explicit migration design;
it is not silently resized or moved by this change.

## Regression tests

With the pinned ESP-IDF environment exported:

```sh
python tests/partition_contract_test.py
python tests/flash_lifecycle_test.py
python tests/board_flash_entry_test.py
bash tests/run_fresh_owner_trust_tests.sh
bash tests/run_owner_trust_storage_policy_tests.sh
bash tests/run_owner_trust_commissioner_tests.sh
bash tests/run_eidolon_common_tests.sh
python tests/serial_buildstamp_reconnect_test.py
```

The Python test imports IDF's real partition parser. No duplicate CSV parser,
flash writer or private provisioning protocol is introduced.

## OTA capacity

Hub builds reserve at least 256 KiB in each application slot, checked against
actual binaries by the shared contract (`CONFIG_EIDOLON_OTA_MIN_FREE_BYTES`).
Every flashed partition reports used/free bytes. This budget does not resize
partitions. Reduce linked features before proposing a device-layout migration.
