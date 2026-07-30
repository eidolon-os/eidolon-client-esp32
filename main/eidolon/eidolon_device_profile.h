#pragma once

#include "eidolon_topics.h"

#include <esp_log.h>
#include <sdkconfig.h>

// Single compile-time source of truth for this build's device profile.
//
// There are TWO independent axes. Keeping them separate is the whole point of
// this header, because they cross in the real board matrix:
//
//   CAPABILITY — CONFIG_USE_DEVICE_AEC: does this board have a *validated*
//     device-side AEC reference (speaker loopback into the capture stream)?
//     This decides the CAPTURE TOPOLOGY. With a reference, mic PCM must go
//     through the ESP-SR AFE for echo cancellation. Without one the AFE has
//     nothing left to do: aec_init is false, no NS model is flashed on the
//     eidolon partition tables (esp_srmodel_init finds none -> ns_init false),
//     and the AFE's VAD result is consumed only by the xiaozhi audio_service
//     path, never by the LiveKit path — so it degenerates into a pass-through
//     that still costs internal SRAM, a task and CPU. Then we read the mic
//     directly instead.
//
//   MODE — ptt / half_duplex / full_duplex: WHEN is the mic open. This decides
//     turn-taking and mic gating (the gated capture source in livekit_board.cc
//     plus the controller's capture gate) and is declared to the Hub at runtime
//     via X-Device-Interaction-Mode. It must NOT decide the topology.
//
// Why the separation matters: m5stack-stackchan is half_duplex WITHOUT an AEC
// reference, esp-box-3 is full_duplex WITH one, waveshare-2.06 is ptt without —
// so "no AEC" is not a synonym for "ptt". Gating the topology on the mode would
// give a future ptt board that ships a validated reference the wrong pipeline,
// and today it would exclude StackChan for the wrong reason.
//
// The one direction that IS causal: full_duplex means capturing while playing,
// which without an AEC reference records the device's own speaker. That is a
// build error below, not a runtime surprise.

namespace eidolon {

#if CONFIG_EIDOLON_INTERACTION_MODE_PTT
inline constexpr bool kModePtt = true;
#else
inline constexpr bool kModePtt = false;
#endif

#if CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX
inline constexpr bool kModeHalfDuplex = true;
#else
inline constexpr bool kModeHalfDuplex = false;
#endif

inline constexpr bool kModeFullDuplex = !kModePtt && !kModeHalfDuplex;

#ifdef CONFIG_USE_DEVICE_AEC
inline constexpr bool kDeviceAec = true;
#else
inline constexpr bool kDeviceAec = false;
#endif

// Capture topology. Mirrors the capability, never the mode. The AFE path is
// compiled in only when it can actually cancel something (see the note above);
// the preprocessor guard in eidolon_mic_capture.cc keeps ESP-SR out of the
// image entirely on boards without a reference.
inline constexpr bool kCaptureViaAfe = kDeviceAec;

static_assert(!(kModePtt && kModeHalfDuplex),
              "ptt and half_duplex are mutually exclusive; Kconfig makes "
              "HALF_DUPLEX depend on !PTT, so both being set means a build "
              "script forced the symbols by hand.");

static_assert(!kModeFullDuplex || kDeviceAec,
              "full_duplex needs a validated device-side AEC reference "
              "(USE_DEVICE_AEC=y): barge-in captures while the speaker plays, "
              "so without cancellation the device transcribes its own voice. "
              "Declare EIDOLON_INTERACTION_MODE_HALF_DUPLEX=y for this board, "
              "or add it to the USE_DEVICE_AEC allowlist once its AEC "
              "reference is qualified.");

inline const char* InteractionModeName()
{
    return kModePtt ? kInteractionModePtt
                    : (kModeHalfDuplex ? kInteractionModeHalfDuplex : kInteractionModeFullDuplex);
}

// One line per boot, next to EIDOLON-BUILDSTAMP. Both axes plus what they
// resolved to, so a board whose Kconfig silently disagrees with its intent is
// visible in the first screen of serial output instead of being inferred from
// behaviour later. (This project has twice shipped a board that silently landed
// on the wrong mode because a build script forced a stale symbol.)
inline void LogDeviceProfile(const char* tag)
{
    ESP_LOGW(tag, "EIDOLON-PROFILE mode=%s device_aec=%d capture=%s",
             InteractionModeName(), kDeviceAec ? 1 : 0, kCaptureViaAfe ? "afe" : "raw");
}

}  // namespace eidolon
