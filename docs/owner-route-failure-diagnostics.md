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
