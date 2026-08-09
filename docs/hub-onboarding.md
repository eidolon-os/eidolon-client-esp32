# Hub onboarding contract

The ESP32 client consumes only the current Hub flow:

1. discover `txtvers=1`, `descriptor_uri`, and `enrollment_uri` over mDNS;
2. fetch and cross-check the HTTPS descriptor;
3. create or resume one crash-safe Enrollment intent;
4. poll the Enrollment handoff until Owner approval and a compatible Provider
   assignment are both available.

There is no fallback to `api=v1`, `register_url`, or `RegisterDevice`.

Wi-Fi is transport state. It is complete before onboarding starts and is never
used as evidence of Owner admission. Hub lifecycle is stored separately as
`pending-approval`, internal `waiting-binding`, `approved`, or `revoked`.

## Persistent identity and retry state

The existing P-256 private key remains in the `eidolon_id/p256_priv` NVS key.
Before the first Enrollment POST, the client atomically persists a separate
onboarding object containing the Hub/device identity, request ID, 256-bit
retrieval token, 256-bit local pairing secret and its SHA-256 commitment. A lost
HTTP response or reboot therefore retries the identical Enrollment request; it
does not create parallel secrets. The receipt's enrollment ID, claim URI and
deadline are saved into the same object before handoff.

The ESP32 never logs the retrieval token, pairing secret, identity signature or
opaque Provider binding. A firmware-side Mobile/Local integration may load the
state internally and use `BuildLocalPairingPayload` to render the exact
physical/near-field payload defined by Hub's `device-pairing-proof-v1` contract.
The Emote display implementation additionally renders this compact QR transport
profile while the enrollment is `pending-approval`:

```text
EIDOLON:PAIR:1:<enrollment_id>:<pairing_secret>
```

Both fields use only base64url-safe ASCII and the whole payload is at most 106
bytes (QR version 5, ECC Low). It is an admission proof, not a provisioning
descriptor: the scanner must already have the verified Hub descriptor origin
and constructs the v1 pairing-claim URI from that origin plus the scanned
enrollment ID. The QR is hidden outside `pending-approval`; the plaintext secret
is removed from onboarding NVS after Hub reports approval, waiting for Provider
binding, or revocation. QR payload content is suppressed from both display-layer
log tags. There is no unauthenticated LAN endpoint for this secret.

Pending handoff is a successful onboarding state (`HTTP 202`), not a Wi-Fi or Hub
failure. The controller polls every five seconds. An expired pending enrollment
can be replaced with a new persisted intent; an approved enrollment with an
expired retrieval window cannot be silently re-enrolled because Hub Owner
admission is authoritative.

## Provisioning transport boundary

The minimum ready path in this firmware assumes Wi-Fi was configured earlier,
then performs Hub discovery, enrollment, physical QR Owner proof, handoff and
voice binding. The existing `Xiaozhi-*` captive portal remains a legacy Wi-Fi
transport: it has no authenticated session or verifiable identity and must not
be adapted as the Mobile `DeviceProvisioningTransport` contract.

The future authenticated provisioning transport consumed by Mobile is a
separate firmware feature. Its session descriptor is exactly:

```json
{
  "contract_version": "1",
  "device_id": "<stable device ID>",
  "device_kind": "<CMake BOARD_TYPE>",
  "display_name": "<CMake BOARD_NAME>",
  "identity_fingerprint": "p256:<SHA-256 of DER SPKI>",
  "session_id": "<fresh 128-bit-or-greater random ID>",
  "expires_at": "<UTC RFC3339 timestamp>",
  "trust": "development-tofu"
}
```

Its authenticated, encrypted, replay-protected session has three operations and
keeps their outcomes distinct:

1. `scan-networks` returns SSID/RSSI/security only;
2. `configure-network` accepts Wi-Fi credentials plus `{hub_id,
   descriptor_uri}` and returns `network-configured` only;
3. `await-enrollment` returns `{device_id, enrollment_id, lifecycle_state}` only
   after the device has independently signed and submitted Hub enrollment.

Wi-Fi credentials, Owner/Controller credentials and the QR pairing secret are
never persisted in a Mobile checkpoint or sent together in one request. This
authenticated provisioning transport is **not implemented by this revision**;
Mobile must treat the current legacy hotspot as Wi-Fi-only and may use the QR
path only for an already-networked pending enrollment.

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

The build uses the CMake `BOARD_NAME`/`BOARD_TYPE` values in Enrollment. The
currently connected ESP-BOX-3 build therefore reports `esp-box-3`; the Waveshare
build reports its own board values. Neither kind is hard-coded in onboarding.
