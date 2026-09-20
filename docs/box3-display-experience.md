# BOX-3 display behavior

The companion surface is shared by BOX-3, StackChan and CoreS3. The BOX-3
production profile uses its physical session button; touch controls are a
separate supported configuration.

## Startup and state ownership

Hub admission with an active configuration projects **Connecting**, never
**Ready**. Only the voice transport's operational observation makes the service
ready and enables a conversation. Finding the Hub, checking device access,
waiting for service configuration and connecting have distinct descriptions.
Unknown enrollment never claims that the device has already been claimed, and
service failures remain visible even before enrollment is known.

Application owns Presenter mutations. Calls from background tasks are copied
into the Application queue. Activation UI updates carry the activation's setup
generation and are discarded after setup changes or while offline. The worker's
completion travels through the same FIFO, so it cannot overtake admission UI
updates. Queued voice readiness updates are also fenced by setup generation.

## Captions and notifications

Dialogue uses a fixed, two-line region between the face and the action row.
The region remains reserved during conversation pauses when dialogue output is
allowed; short captions do not resize the face. With dialogue output disabled,
no dialogue is retained in the view.

The first caption appears immediately. Subsequent bursts coalesce to at most
ten visible text changes per second. While speaking, captions follow the newest
line; completed non-streamed captions advance in whole-line pages
every 3.5 seconds, without horizontal marquee motion. Repeated model updates do
not reset the reading position. After the final page and the end of speaking,
the text clears after six seconds and duplicate models cannot resurrect it.
A new user-speaking/thinking phase clears the previous assistant response;
leaving the conversation clears its text. Muting does not reset the transcript.

This is presentation pacing, not word-level audio synchronization. The existing
transport delivers completed transcription streams and supplies no word/audio
playback timestamps. These changes do not invent audio alignment or alter the
voice transport.

Notifications use their own temporary banner, not the caption label. On recovery
pages the banner moves to the footer to avoid covering the instructions.

## Indicators

Populated icons pack from the right in 28-pixel cells, centered and separated
from the status title. An unavailable battery reading has no visible icon.
Microphone mute projects immediately with the voice model instead of waiting
for a status-bar tick. Setup uses a glyph present in the actual icon font.
Network indicators can refresh while connecting and speaking as well as idle.

## Verification

- `bash tests/run_eidolon_ui_labels_tests.sh`
- `bash tests/run_voice_runtime_projector_tests.sh`
- `bash tests/run_ui_input_profile_tests.sh`
- `bash tests/run_operational_readiness_tests.sh`
- `bash tests/run_hub_activation_retry_core_tests.sh`
- `bash tests/run_ui_presenter_tests.sh` (AddressSanitizer and UBSan)
- `bash tests/run_companion_ui_tests.sh` (production LVGL renderer and board fonts)
- ESP-IDF 5.5.4 build using `build/eidolon/esp-box-3`

Renderer snapshots cover touch controls, the physical-button profile, captions,
notifications, mute/setup/network/battery icons, recovery, and PTT release across
model changes. They verify desktop renderer behavior, not panel timings or real
speech latency. Physical cold boot, Wi-Fi loss/recovery and spoken conversation
still require a device acceptance run.

## Chinese resource font regression

A device acceptance report found that `我叫小何。` rendered only its supported
characters (`我` and `。`). The built-in Noto basic subset lacks `叫`, `小`, and
`何`. The complete `font_noto_qwen_20_4.bin` was already present in the flashed
assets image (2,998,916 bytes, verified identical to the source resource), but
`Apply(false)` did not refresh the companion labels. The legacy Eidolon theme
also held its original subset fallback.

The companion now explicitly binds the loaded resource font for captions and
detail text, independently of theme refresh. Header/control typography uses the
board-sized built-in font. Resource descriptors remain owned while referenced
by labels; labels revert to the built-in font before asset unmapping. Caption
height follows the actual resource font's metrics,
with two complete lines and whole-line paging. No full font is added to app
Flash, and the mapped glyph bitmaps are not copied into RAM.

The host render test now loads the production 32-bit cbin through a host-only
pointer-layout adapter. It verifies individual glyph coverage and renders the
reported sentence, font rebinds, multiline pagination, and output suppression.
Snapshots `box3-chinese-before`, `box3-chinese-after`, and `box3-chinese-page`
use the actual packed font's glyphs, bitmaps and metrics.

## Streaming captions and BOX-3 typography

Assistant `lk.transcription` chunks now publish valid UTF-8 snapshots before
the trailer. Incomplete characters wait for the next chunk. The bounded 1KB
window retains recent text, so long turns continue without unbounded memory.
User transcripts keep the existing finality gate. User-speaking/thinking states
invalidate the active assistant stream to suppress its late chunks.

BOX-3 selects the complete 16px Noto asset and matching built-in boot font
instead of 20px. Icons retain their existing size. Caption width is 304px
on the 320px panel, with symmetric 8px margins and explicit left alignment.
English wraps at word boundaries, so the rightmost ink need not touch the margin.
Live captions follow the latest whole line at up to 10 updates per second.

Host checks cover byte-split Chinese, rolling long responses, interim assistant
events, actual 16px resource glyphs, 304px width and streaming scroll position.
These checks do not measure device speaker-to-screen timing; that still depends
on server pacing and device audio buffering. Stream-open/first-chunk/close logs
contain timing and byte counts without logging dialogue content.
