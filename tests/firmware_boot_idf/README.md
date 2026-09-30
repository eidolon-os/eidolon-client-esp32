# Firmware confirmation across real OTA boots

```sh
source /path/to/esp-idf/export.sh
# If not on PATH:
export QEMU_XTENSA=/path/to/espressif/qemu-system-xtensa
bash tests/run_firmware_boot_idf_tests.sh
```

This compiles the production `firmware_boot.h` with ESP-IDF 5.5.4 and runs the
actual ESP32 bootloader, OTA APIs and NVS in Espressif QEMU. The runner creates
a disposable 4 MiB flash image in `.cache/firmware-boot-build`; no serial port
or physical device is opened. Each run starts from a fresh image.

The same test executable occupies both OTA slots. Across seven boots it checks:

- A pending candidate confirms offline and remains selected after restart.
- A candidate with an injected storage-init failure remains selected even when
  a valid rollback image exists; the fault flag and stored identity survive.
- A candidate that restarts **before** confirmation is still rejected by the
  real bootloader, which boots the previous valid image.

Identity contents are checked after every restart. An assertion, panic, failed
IDF check, missing success marker or 50-second timeout fails the run. Full logs
are saved in the build directory. GitHub CI also runs this and the real-kernel
controller inbox contract.

This verifies boot selection, not radio/audio/display hardware. The storage
error is injected into the boot state; it is not a simulation of flash wear.
`run_commissioning_nvs_replay_tests.sh` separately tests real NVS allocation,
write/erase interruptions and valid/invalid legacy PEM reads without replacing
identity. `run_firmware_boot_tests.sh` injects OTA metadata read/write failures
and deliberately links no Owner/clock/network adapters, keeping persistent
configuration validation out of the executable-confirmation decision.
