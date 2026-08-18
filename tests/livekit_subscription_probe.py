"""Prove a device connection can carry more than one conversation.

Runs on the Host (where the LiveKit server and its credentials live), not on the
device. It joins the device's room as a stand-in agent, publishes audio, leaves,
and repeats. That is the exact shape of the bug this exercises: the LiveKit ESP
client records the one remote audio track a connection is subscribed to, and for
a long time released that record only when the whole connection was torn down —
so the second agent to speak was refused with ENGINE_ERR_MAX_SUB and the device
sat in a conversation it could not hear.

Doing it this way needs nobody standing at the device, and it isolates the
transport from the product flow: if a round fails here, the fault is in the SDK,
not in wake-word, session routing, or the agent.

Usage, on the Host:

    sudo -n env ROOM=<device room> bash -c \\
      'set -a; . /etc/eidolon/livekit.env; set +a; \\
       /opt/eidolon/current/eidolon_channel/.venv/bin/python livekit_subscription_probe.py'

The room name is in the device's own log (`room=eidolon-device-...`).

Then read the device's serial log. Per round it must show a fresh
`Subscribing to audio track: sid=TR_...` and, when this participant leaves,
`Releasing audio track subscription: ... (publisher disconnected)`. Rounds 2 and
3 succeeding is the point. Also check, across the whole window:

  - `livekit_peer.sub: State changed:` reaches 2
  - no `SCTP: Send INIT chunk`   (a data channel on a connection that
    negotiated none retries INIT forever)
  - no `MAX_SUB`, and no session renewal between rounds
  - no `DTLS: Detected DTLS connection close` / `Publisher peer connection failed`
"""

import asyncio
import os

import numpy as np
from livekit import api, rtc

ROOM = os.environ["ROOM"]
URL = os.environ.get("LIVEKIT_URL", "ws://127.0.0.1:7880")
ROUNDS = int(os.environ.get("ROUNDS", "3"))
HOLD_S = int(os.environ.get("HOLD_S", "12"))
GAP_S = int(os.environ.get("GAP_S", "8"))

SAMPLE_RATE = 48000
FRAME = 480  # 10ms


def token(identity):
    return (
        api.AccessToken(os.environ["LIVEKIT_API_KEY"], os.environ["LIVEKIT_API_SECRET"])
        .with_identity(identity)
        .with_name(identity)
        .with_grants(
            api.VideoGrants(
                room_join=True, room=ROOM, can_publish=True, can_subscribe=True
            )
        )
        .to_jwt()
    )


async def one_round(n):
    identity = "probe-agent-%d" % n
    room = rtc.Room()
    await room.connect(URL, token(identity))
    source = rtc.AudioSource(SAMPLE_RATE, 1)
    track = rtc.LocalAudioTrack.create_audio_track("probe-voice", source)
    pub = await room.local_participant.publish_track(
        track, rtc.TrackPublishOptions(source=rtc.TrackSource.SOURCE_MICROPHONE)
    )
    print("[round %d] joined as %s, track sid=%s" % (n, identity, pub.sid), flush=True)

    # A 440Hz tone, so the track carries real media rather than only an SDP entry.
    t = 0
    for _ in range(int(HOLD_S * SAMPLE_RATE / FRAME)):
        samples = np.arange(t, t + FRAME)
        tone = (np.sin(2 * np.pi * 440 * samples / SAMPLE_RATE) * 8000).astype(np.int16)
        t += FRAME
        await source.capture_frame(
            rtc.AudioFrame(tone.tobytes(), SAMPLE_RATE, 1, FRAME)
        )

    await room.disconnect()
    print("[round %d] left" % n, flush=True)


async def main():
    for n in range(1, ROUNDS + 1):
        await one_round(n)
        if n < ROUNDS:
            print("--- gap %ds ---" % GAP_S, flush=True)
            await asyncio.sleep(GAP_S)
    print("probe done", flush=True)


asyncio.run(main())
