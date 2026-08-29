#pragma once

// Build fingerprint. scripts/eidolon/eidolon-common.sh generates
// eidolon_build_stamp.gen.h (gitignored) with the real git commit / branch /
// LiveKit SDK version / verified ESP-IDF version at build time; this header
// includes it when present and otherwise falls back to placeholders so a plain
// `idf.py build` (no toolkit) still compiles. The app prints these once at boot
// as one "EIDOLON-BUILDSTAMP ..." line so you can confirm exactly which
// firmware + SDK + toolchain the device is actually running (PROJECT_VER alone
// is a static "1.0.0").

#if defined(__has_include)
#  if __has_include("eidolon_build_stamp.gen.h")
#    include "eidolon_build_stamp.gen.h"
#  endif
#endif

#ifndef EIDOLON_BUILD_GIT
#define EIDOLON_BUILD_GIT "unstamped"
#endif
#ifndef EIDOLON_BUILD_BRANCH
#define EIDOLON_BUILD_BRANCH "unknown"
#endif
#ifndef EIDOLON_BUILD_SDK
#define EIDOLON_BUILD_SDK "unknown"
#endif
#ifndef EIDOLON_BUILD_IDF
#define EIDOLON_BUILD_IDF "unknown"
#endif
