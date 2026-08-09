# Hub onboarding contract

The ESP32 client consumes only the current Hub flow:

1. discover `txtvers=1`, `descriptor_uri`, and `enrollment_uri` over mDNS;
2. fetch and cross-check the HTTPS descriptor;
3. create or resume one crash-safe Enrollment intent;
4. poll the Enrollment handoff until administrator approval and a compatible Provider
   assignment are both available.

There is no fallback to `api=v1`, `register_url`, or `RegisterDevice`.

Wi-Fi is transport state. It is complete before onboarding starts and is never
used as evidence of Owner admission. Hub lifecycle is stored separately as
`pending-approval`, internal `waiting-binding`, `approved`, or `revoked`.

## Persistent enrollment and retry state

Before the first Enrollment POST, the client atomically persists an onboarding
object containing the Hub/device identity, request ID and 256-bit retrieval
token. A lost HTTP response or reboot therefore retries the identical Enrollment
request instead of creating parallel pending devices. The receipt's enrollment
ID, lifecycle and deadline are saved into the same object before handoff.

The device never creates or exposes an Owner pairing secret. It does not require
a screen, QR renderer, camera, button, BLE transport or Mobile callback. A
trusted administrator reviews the pending device in Hub management, selects the
Owner and approves it. A screen-capable product may display generic progress
such as “Waiting for approval”, but display output is not protocol evidence.

The ESP32 never logs the retrieval token or opaque Provider binding. Its existing
P-256 identity remains available to other signed device APIs, but it is not an
extra enrollment admission mechanism in this manually approved flow.

Pending handoff is a successful onboarding state (`HTTP 202`), not a Wi-Fi or Hub
failure. The controller polls every five seconds on both screen and headless
devices. An expired pending enrollment can be replaced with a new persisted
intent; an approved enrollment with an expired retrieval window cannot be
silently re-enrolled because Hub Owner admission is authoritative.

## Provider binding consumed by this firmware

Hub treats channel credentials as opaque. The ESP32 therefore selects only a
Channel Assignment whose `binding_format` is:

```text
application/vnd.eidolon.livekit-device+json;v=1
```

After base64 decoding `opaque_binding`, the Provider-owned JSON must be:

```json
{
  "schema_version": 1,
  "active": {
    "server_url": "wss://livekit.example",
    "token": "<voice credential>",
    "identity": "device-...",
    "room_name": "device-voice-..."
  },
  "control": {
    "server_url": "wss://livekit.example",
    "token": "<control credential>",
    "identity": "device-...",
    "room_name": "device-control-..."
  },
  "audio": {"sample_rate": 16000, "channels": 1}
}
```

This binding is parsed only on the device. Hub must remain Provider-neutral.
Provider credential renewal after the bounded Hub handoff window belongs to the
Provider binding contract; the ESP32 may use its last atomically cached config
for bounded recovery but must not resurrect a revoked or unknown lifecycle.

The build uses the CMake `BOARD_NAME`/`BOARD_TYPE` values in Enrollment. For the
target hardware both are `esp32-s3-touch-amoled-2.06`; no Box-3 device kind is
hard-coded.
