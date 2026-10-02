# Controller inbox contract

Runs the **production** `controller_event_inbox.h` on ESP-IDF 5.5.4's actual
FreeRTOS kernel in Espressif's ESP32 QEMU machine. No host queue/notification
reimplementation is used. This is not an ESP32-S3 radio, display or audio test.

```sh
source /path/to/esp-idf/export.sh
export QEMU_XTENSA=/path/to/espressif/qemu-system-xtensa
bash tests/run_controller_inbox_tests.sh
```

The bounded runner fails on assertion/panic, missing success marker, or 40-second
timeout. Build and logs stay in `.cache`. It never opens a serial device.

Cases: original 24-entry FIFO counterexample; 10,000 combined tick/activity
signals without FIFO occupancy; real 80 ms esp_timer with a 2.4 s consumer stall; all 24 command slots, 25th command rejection,
ordered drain and capacity recovery; signals posted during handling; two
concurrent producers with a controlled 100 ms consumer stall, exact accepted vs
processed accounting and per-producer FIFO ordering; maintenance progress under
command pressure; 1,000 posts across empty/wait transitions; finite deadline wakeup without
notifications, FIFO backlog bypassing that deadline, and pending configuration
notification delivery during timed waits.

The caller owns rejected event payloads and handles protocol acknowledgements.
This transport test does not claim to cover controller parsing, network command
acknowledgements, lifecycle semantics, or the original hardware stall. Index 0
of the controller task notification array is reserved for this inbox; handlers
must not consume it for another purpose. Controller destruction still requires
producers to have stopped (the production controller has application lifetime).
