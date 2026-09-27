# The LiveKit SDK is our fork, pinned to a commit

`main/idf_component.yml` does not take `livekit/livekit` from the component
registry. It takes it from our fork, at one commit:

```yaml
  livekit/livekit:
    git: https://github.com/eidolon-os/client-sdk-esp32.git
    path: components/livekit
    version: 502ce391bf01c47dea74e1dd651c5b8945ca8906
    rules:
    - if: target in [esp32s3, esp32p4]
```

- **`git` + `path`**, because the SDK is one component inside a repository that
  also holds examples and a test app.
- **https, not ssh**, so a build needs no SSH access to the fork. Pushing to it
  does; see below.
- **a commit, not a branch**, so a build is reproducible. `managed_components/`
  is gitignored, so the fork is the only place a change to this SDK survives —
  editing `managed_components/livekit__livekit/` changes nothing that lasts.

The pinned commit is compiled in as `LIVEKIT_SDK_VERSION` and sent to the server
on every join, so the LiveKit server's own logs name the exact SDK a device is
running:

```
"clientInfo":{"sdk":"ESP32","version":"502ce391bf01c47dea74e1dd651c5b8945ca8906", ...}
```

## Why the fork exists

Upstream `v0.3.10` — the newest release — has two defects this firmware cannot
live with. The fork is that release plus exactly two fixes and a version marker.

**The subscription slot was never released.** `engine.c` recorded the one remote
audio track a connection was subscribed to and refused every later track while
that record was set, and nothing cleared it except tearing down the whole
connection. `handle_participant_update()` never read `ParticipantInfo.state`, so
a participant leaving was processed as though it were still arriving. A
connection that had carried one conversation believed forever that it was
subscribed to a track that no longer existed: the next agent's audio was refused
with `ENGINE_ERR_MAX_SUB` and the device sat in a conversation it could not hear.

**A data channel was started on a connection that negotiated none.** `peer.c`
set `enable_data_channel` unconditionally. On the subscriber connection that
left SCTP retrying INIT for the whole session against a peer with no SCTP
endpoint (measured at ~13 chunks/second), and — because the only route to
`CONNECTION_STATE_CONNECTED` was both data channels opening — meant the
subscriber could never report itself connected at all.

The fix is not "decide by role". Data channels live on the publisher connection,
and additionally on the subscriber connection when the session is subscriber
primary — which is what the join response's `subscriber_primary` announces,
before either peer is created. This client currently joins with `protocol=1`
deliberately (see the TODO in `core/url.c`), which the server answers with
publisher-primary; deciding by role would break the day that TODO is resolved.

### These two facts are coupled — do not separate them

The firmware used to work around the first bug by rebuilding its whole transport
after every conversation (`LiveKitSession::RenewSession`). That is gone, because
the reason for it is gone. Which means:

| SDK | `RenewSession` | Result |
|---|---|---|
| fork | absent | correct — what we ship |
| fork | present | merely redundant |
| stock 0.3.10 | present | works, badly — rebuilds the transport every conversation |
| **stock 0.3.10** | **absent** | **the device goes deaf on the second conversation** |

Reverting the pin without restoring the workaround lands in the last row. They
were removed in one commit for this reason; keep them together.

## Changing the SDK

1. Edit `components/livekit/` in a clone of the fork, on `eidolon_dev`.
2. Commit and push (see *Pushing* below).
3. Put the new commit SHA in `main/idf_component.yml`.
4. Rebuild. The component manager re-resolves the git source on its own;
   `dependencies.lock` records `type: git` and the SHA, and is gitignored.
5. Verify on a device — see *Verifying*.

## Taking an upstream update

Upstream releases are infrequent and `v0.3.10` is still the newest. When one
lands:

```bash
git fetch upstream --tags
git rebase --onto vX.Y.Z <old-base> eidolon_dev
```

**Rebase, not merge.** Our three commits are meant to become an upstream pull
request one day, so they need to stay a clean series on top of upstream rather
than a tangle of merge commits. Expect the version-marker commit to conflict on
`idf_component.yml` — resolve it to the new upstream version plus our `~N`
suffix.

Then check whether upstream has fixed either defect itself, and drop our commit
if so. As of `v0.3.10` neither is fixed: upstream `main` is the tag plus four
dependabot commits, and the branch
`jacobgelman/bot-220-create-data-channels-at-correct-point-in-lifecycle` is a
different change (already merged as `bb7cec3`) that moves *when* channels are
created, not *whether* the connection has any.

Push, bump the pin in `main/idf_component.yml`, rebuild, re-verify.

### Pushing

The fork lives under the `eidolon-os` organization. A personal SSH key will not
have write access to it, so pushes go over https with the `gh` credential:

```bash
git remote set-url --push origin https://github.com/eidolon-os/client-sdk-esp32.git
git config credential.helper '!gh auth git-credential'
```

`gh auth status` must show the `eidolon-os` account with `repo` scope.

## Verifying

Both defects are in the SDK's engine/peer layer, so they are board-independent —
verify on whatever board is to hand, and note which one. `Board: ... SKU=<board>`
in the device's boot log is the reliable identifier; MAC addresses and IPs are
not (two boards on one desk look alike in the server logs, and re-provisioning
changes identity).

`tests/livekit_subscription_probe.py` drives the whole thing from the Host
without anyone standing at the device: it joins the device's room as a stand-in
agent, publishes audio, leaves, and repeats three times. Its docstring has the
invocation and the four things to check in the device's serial log.

A clean run looks like this — three different track SIDs, taken and given back
on one connection, with no renewal in between:

```
Subscribing to audio track: sid=TR_AMwwxRdUJeVULD
Releasing audio track subscription: sid=TR_AMwwxRdUJeVULD (publisher disconnected)
Subscribing to audio track: sid=TR_AMQe9aJiHKwxmG
Releasing audio track subscription: sid=TR_AMQe9aJiHKwxmG (publisher disconnected)
Subscribing to audio track: sid=TR_AMNeS67UnZ9jVH
Releasing audio track subscription: sid=TR_AMNeS67UnZ9jVH (publisher disconnected)
livekit_peer.sub: State changed: 0 -> 1
livekit_peer.sub: State changed: 1 -> 2
```

### Capturing the serial log without rebooting the board

`stty -f /dev/cu.usbmodem101 ...` carries `hupcl`, so opening and closing the
port pulls DTR/RTS — which is wired to reset. A capture script that re-runs
`stty` on every reconnect will hold the board in a reset loop that looks exactly
like a firmware crash. Read with pyserial holding `dtr=False, rts=False` on a
single long-lived connection, and check the `rst:` reason in the boot banner
before concluding anything: `USB_UART_CHIP_RESET` is the host pulling reset,
`RTC_SW_CPU_RST` is the chip restarting itself.
