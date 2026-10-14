# AV render dependency maintenance

Vendored from IDF registry `tempotian/av_render` 1.0.0 (component hash
`7ad5b2e1cb01582dcc1360a2c7a9af02f14f33cfef3b2e57643eb98689236cd0`).
The upstream implementation is retained, including its license and public API.

Local correction: `render_flush` waits only when worker acknowledgements exist,
and combines audio/video worker bits. An allocated but never-used renderer has
no workers; waiting on zero bits asserts in FreeRTOS. This is independent of
board, scene and output policy. No media pipeline or transport is added.

Remove this override when an upstream release includes both corrections; run
empty-renderer, audio, video and combined flush regression checks plus device
start/end and PTT interruption checks when updating.

The audio prebuffer also has a monotonic deadline equal to its target PCM
duration. Only startup or a genuinely empty queue arms it; low water drains
normally. This prevents short tails and sparse/DTX packets from waiting for
future speech to fill the byte threshold. Flush bypasses prebuffer waiting and
resets it, as does stream replacement. This corrects the existing renderer;
there is no second queue, pacing task or media transport. Check
`tests/audio_prebuffer_test.c` and the flush regression when updating upstream.
