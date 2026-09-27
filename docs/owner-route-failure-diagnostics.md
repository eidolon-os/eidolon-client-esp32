# Owner route failure diagnostics

`OWNER_ROUTE_UNAVAILABLE` identifies the commissioning route check, not a
specific DNS, TCP, TLS or HTTP failure. The observed BOX-3 rollback on
2026-09-22 remains unexplained: its device-side error was not captured, and a
later successful maintenance submission followed a device restart.

`HubHttp` now records the failing phase, elapsed milliseconds, original API
result, HTTP status, socket errno, ESP-TLS error, mbedTLS error and certificate
verification flags before closing or freeing the HTTP client. Phases cover
initialization, open, body write, header read, body read, incomplete body and
response size. `open` includes name resolution, TCP and TLS; interpret the SDK
error evidence instead of inferring a substage from the phase alone. Zero TLS
fields do not prove that a handshake completed. The commissioning caller already
logs a non-200 route response as a rejected route.

Failure reporting no longer performs another DNS lookup. Such a lookup could
change resolver state and report an address different from the attempted one.
No attempted address is claimed without connection-level evidence. The DNS
cache-clear message now correctly describes a whole-cache operation.

This change does not alter return values, response handling, retry policy,
timeouts, certificate verification, cache invalidation policy or commissioning
commit/rollback decisions. Incomplete-body reporting is diagnostic only; changing
that existing response behavior requires a separate scoped change. Logs contain
no request/response bodies, Wi-Fi passwords or certificate material.

Validation: the modified translation unit compiles against the production
ESP-IDF 5.5.4 configuration; existing host-resolution and HTTP-date tests pass.
No firmware was flashed and no real DNS/TLS failure was induced for this change.

## Activation and reconnect diagnostics (2026-09-27)

`HubOnboardingClient::Run` and `Resume` now produce one attempt-local diagnostic
result and a failure summary. Stable stage codes distinguish `trust-load`,
`trust-bundle`, `directory-restore`, `directory-fetch`, `directory-validate`,
`generation-check`, `claim-restore`, `claim-cleanup`, `claim-resume`,
`claim-propose`, `claim-proposal-store`, `claim-collect`, `claim-ack`,
`claim-terminal`, `claim-revocation-store`, `configuration-proof`,
`configuration-fetch`, and `configuration-validate`.

The summary includes symbolic/native error, hexadecimal code, HTTP status,
elapsed time, attempt mode (activate/resume), and the commissioning generation
at attempt start. HTTP zero means no response recorded for that stage, never
HTTP success. Each new stage/candidate clears the previous HTTP status; response
validation retains its actual HTTP status (including 200). A new attempt resets
all previous diagnostic evidence. Stage codes locate the operation, while the
existing lower-level HubHttp phase/errno/TLS logs identify transport details;
a `directory-fetch` failure alone must not be labelled DNS or TLS failure.

Boot retry text shows stage, error name/code, HTTP evidence, failed attempt number
and retry delay. The delay is labelled as a delay, not a live countdown. This
replaces the misleading generic “Looking for the Hub” message for all retryable
activation errors. Reconnect uses the same diagnostic summary in logs; it keeps
its existing lifecycle UI. Terminal Claim UI and retry/authorization decisions
are unchanged. Identity-load and trust-load failures log their underlying error
before existing return-code mapping; discovery logs both its own exhausted
result and the original route error it returns. HubHttp also records HTTP
rejections without logging bodies or credentials.

Diagnostics are in RAM/logs only; no persistent ring buffer, management export,
NVS write, hardcoded device mapping, or retry policy change is introduced. This
improves evidence for future incidents; it does not establish the lost error code
of the 2026-09-27 BOX-3 incident or claim to fix that incident's unknown trigger.

Validation: diagnostic reset/HTTP distinction/rendering tests under ASan/UBSan;
existing activation retry, Owner discovery, onboarding protocol, host resolution
and UI presenter tests; compilation of all four changed translation units against
the BOX-3 production ESP-IDF configuration. No device was flashed for this change.
