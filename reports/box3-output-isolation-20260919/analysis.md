# BOX-3 mixed output isolation

## Evidence and cause

After recommissioning, ASR reached Agent and the selected speech provider warmed up. Channel then rejected expression delivery before public answer text reached TTS. The original pipeline awaited expression completion twice: Agent held text behind PresentationFeedback, and Channel awaited PresentationTransport inside the LLM stream. A refusal raised ValueError and became a retryable model error. Four model attempts did not repair the device output.

Firmware HUB_MODE did not establish wall time, but expression commands required Unix time >= 1700000000000 to have a bounded deadline. The same command parses as unbounded with a boot/epoch clock and bounded with a current clock. The original Channel erased the device's exact rejection code, so this is a proven code defect and strong explanation of the现场 refusal, not a captured device subcode.

## Final boundaries

- Agent validates one structured response. Selected speech/text requires public answer content (intent=none remains explicit suppression); a face profile does not mean silent mode. Strict silent mode cannot persist a shadow language answer.
- Agent emits public text and generation DONE before awaiting bounded presentation evidence. Public answer history is no longer overwritten by an expression summary. Language delivery remains explicitly unconfirmed; generation is not playback evidence.
- Channel owns a bounded expression task independently of the LLM/TTS stream. New responses, accepted interruption and session teardown cancel it. Expression receipts, refusals and transport timeouts remain output results, including the original reason. They never restart the model or tools.
- Once a logical Agent turn is submitted, stream errors do not automatically create another turn. Connection establishment retains its existing retry behavior.
- TtsStage delegates synthesis to the existing LiveKit provider while routing its error events to the existing output observer. A failed synthesis remains an exception/result for the speech branch, not a counter toward closing the whole multimodal session. Metrics and provider events remain available; no new synthesis engine or retry loop was added.
- Normal text/audio synchronization is preserved. RoomOptions, the transcription node and LiveKit's synchronizer have not been changed. This change does not claim that all generated text was displayed or spoken after TTS failed, and does not add an automatic unsynchronized caption fallback.
- Output observation records speech and expression results separately. An expression refusal cannot overwrite audio-ready state; a speech error cannot erase a completed expression.

## Firmware time

Reuse the existing authenticated Hub HTTPS Date response. A small, boot-local HubClock advances that reading using monotonic elapsed time. It is attached to the validated configuration, never persisted, and is replaced with that configuration. No global settimeofday, new NTP service, NVS migration or new wire protocol is introduced.

HTTP Date rounding and response round-trip delay are included conservatively, so a delayed response cannot extend a command's lifetime. Unknown time yields COMMAND_CLOCK_UNAVAILABLE; malformed bounds yield INVALID_COMMAND_DEADLINE; expired commands remain COMMAND_EXPIRED. Expression runtime still uses local monotonic durations. The same clock value is used for provider-binding expiry, control ACK timestamps and the existing remote erase deadline adapter.

## Validation

- Real local gRPC regression: generation/text complete while the device receipt is still pending; a later refusal does not create a second Agent turn.
- Device refusal reasons, missing receipt timeout, cancellation delivery, late receipt discard and session teardown are covered.
- Mixed-output observation and five consecutive TTS errors verify independent failure handling without a global session close.
- Agent tests cover public answer delivery before DONE/feedback, typed late failure evidence, strict silent selection and intentional suppression.
- Native firmware tests cover cold boot without wall time, authenticated time acquisition, delayed commands/responses, malformed deadlines, monotonic reset and fresh configuration invalidation.
- BOX-3 builds with the existing ESP-IDF 5.5.4 profile and partition layout. Hardware audibility and long-duration stability require现场 verification; local tests alone do not prove them.
