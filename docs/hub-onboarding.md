# Hub onboarding contract

The ESP32 client consumes only the current Hub flow:

0. be commissioned, at setup, for exactly one Host — its Hub id and the
   certificate that Host presents;
1. discover `txtvers=1`, `descriptor_uri`, and `enrollment_uri` over mDNS;
2. fetch and cross-check the HTTPS descriptor, verified against the commissioned
   certificate, and refuse any Hub id other than the commissioned one;
3. create or resume one crash-safe Enrollment intent;
4. poll the Enrollment handoff until administrator approval and a compatible Provider
   assignment are both available.

There is no fallback to `api=v1`, `register_url`, or `RegisterDevice`, and no
fallback to the public certificate bundle, which cannot contain a Host.

Wi-Fi is transport state. It is complete before onboarding starts and is never
used as evidence of Owner admission. Hub lifecycle is stored separately as
`pending-approval`, internal `waiting-binding`, `approved`, or `revoked`.

## Commissioning: how the device learns whose it is

A Host signs its own Hub certificate, so no public CA vouches for it, and the
device has no screen to read a code from and no Owner session to ask. Setup is
where a person is already standing at the device telling it which network to
join; that same act tells it which Host it belongs to.

The firmware serves two endpoints on port **8266** of its own configuration
access point (the vendor captive portal owns port 80). They exist only while
commissioning is open: while the device is in Wi-Fi configuration mode, and — for
a device that is on a network but belongs to no Host yet — while it is connected
and uncommissioned. The moment a Host is stored, the endpoints stop.

`GET /identity` — who this device is, so the commissioner can recognize the
enrollment it is about to create:

```json
{"schema_version": 1, "device_id": "aa:bb:...", "board": "esp-box-3", "device_kind": "esp-box-3"}
```

`POST /commission` — the Host to trust, and optionally the network to reach it
on:

```json
{
  "schema_version": 1,
  "hub_id": "eidolon-hub-...",
  "hub_certificate": "-----BEGIN CERTIFICATE-----\n...",
  "wifi": {"ssid": "home", "password": "secret"}
}
```

`wifi` is optional. A device already on the right network still has to be told
which Host it belongs to — after that Host is rebuilt, or when the device was set
up before there was one — and such a payload must not disturb the network it is
already on.

The certificate is bounded and must actually be a PEM certificate; the Hub id is
bounded; an unsupported `schema_version` is refused rather than guessed at.
Nothing is stored unless the whole payload is accepted.

### One device, one Host

The trust store holds one Hub id and one certificate. It is written only by
commissioning, and onboarding fails closed when it is empty: a device with no
commissioned Host talks to nobody. A Hub that answers discovery under a different
id is not a fallback, and the persisted onboarding state refuses to be switched
to another Hub or device id.

Retargeting a device to a different Host is therefore a physical act — put it
back into setup and commission it again — not something anything on the network
can ask for.

### The device answers before it changes networks

`POST /commission` arrives over the device's own access point, and joining the
commissioned network takes that access point down. So one act happens in a fixed
order, which `Commission()` in `device_commissioning_protocol.cc` performs and
`tests/device_commissioning_sequence_test.cc` pins:

1. **store the Host** — joining a network is the act of a device that already
   knows whose it is;
2. **answer the commissioner** — while the connection carrying the answer still
   exists;
3. **join the network**, on a task of its own, after the handler has returned and
   the connection has been closed.

`HTTP 200` therefore means *accepted and stored*, not *on the network*. The
answer says which of the two it is:

```json
{"schema_version": 1, "device_id": "aa:bb:...", "hub_id": "eidolon-hub-...", "joining_network": true}
```

When `joining_network` is true the verdict on the credentials is not in this
response and cannot be: the response leaves first. The commissioner learns the
outcome from the Host — the device enrolls within seconds of joining — and the
device reports a failure the only way still open to it, by putting the way back
in. After roughly 25 seconds without a connection, a device commissioned over its
setup access point reopens that access point with `/commission` still listening,
says so on its own screen, and lets the next attempt overwrite the credentials;
a device commissioned over a network it was already on keeps its station
retrying instead, because the credentials for that network are still saved and
that retry is the recovery. Either way the stored certificate survives, because a
wrong Wi-Fi password does not make the Host wrong.

A rejected payload or a certificate that would not store answers `400` and
changes nothing at all — no trust written, no network touched.

## Persistent enrollment and retry state

Before the first Enrollment POST, the client atomically persists an onboarding
object containing the Hub/device identity, request ID and 256-bit retrieval
token. A lost HTTP response or reboot therefore retries the identical Enrollment
request instead of creating parallel pending devices. The receipt's enrollment
ID and last observed lifecycle are saved into the same object before handoff;
the Hub remains authoritative for the retrieval deadline through HTTP 410.

The device never creates or exposes an Owner pairing secret. It does not require
a screen, QR renderer, camera, button, BLE transport or Mobile callback. Admission
is an act of the Host's administrator against a pending device: reviewed in Hub
management, or — because a commissioner already knows the `device_id` it read from
`/identity` and just commissioned — claimed automatically by the app that
performed the setup. Either way the decision is the Host's; the device only
enrolls and waits. A screen-capable product may display generic progress such as
“Waiting for approval”, but display output is not protocol evidence.

The ESP32 never logs the retrieval token or opaque Provider binding. Its existing
P-256 identity remains available to other signed device APIs, but it is not an
extra enrollment admission mechanism in this approved flow.

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
The ESP32 stores the assignment expiry with its short-lived Provider config and
rejects an expired cached binding once wall-clock time is available. It may ask
the existing handoff for a refreshed binding only while the bounded retrieval
window remains open. Long-term Provider credential renewal requires a separate
formal contract; the firmware does not reinterpret Enrollment as permanent
device authentication or silently resurrect a revoked lifecycle.

Enrollment uses the active build's CMake `BOARD_NAME`/`BOARD_TYPE` values. The
onboarding client does not hard-code Box3, Waveshare, or any other board kind.
