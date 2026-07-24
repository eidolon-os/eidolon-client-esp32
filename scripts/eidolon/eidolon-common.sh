#!/usr/bin/env bash
# Eidolon dev toolkit - shared helper sourced by every per-board script.
#
# Centralizes the build details that repeatedly bit us during box-3 / stackchan
# debugging:
#
#   1. LiveKit SDK version pinning + FORCED dependency re-resolution.
#      idf.py will NOT downgrade a component just because idf_component.yml
#      changed — the cached dependencies.lock keeps the old (higher) version. So
#      switching the SDK silently had no effect. eidolon_sdk_sync() makes the
#      pin authoritative: it can rewrite the pin (EIDOLON_LIVEKIT_SDK=x.y.z) and,
#      whenever the pin != the installed managed_component, wipes the lock and the
#      livekit managed_component so the next build re-resolves cleanly.
#
#   2. Build fingerprint + post-flash verification.
#      PROJECT_VER is hardcoded "1.0.0", so the device can't tell you which build
#      / which SDK it is running. eidolon_write_build_stamp() bakes the git commit,
#      branch and resolved SDK version into the firmware; the app prints one
#      "EIDOLON-BUILDSTAMP ..." line at boot. eidolon_verify_flashed() reads it back
#      over serial after flashing and diffs it against what was just built, so a
#      stale binary / wrong flash path is caught instead of silently trusted.
#
# Every eidolon_* function takes PROJECT_ROOT explicitly and is safe under
# `set -euo pipefail`.

# ---- paths -----------------------------------------------------------------
eidolon_idf_manifest() { printf '%s/main/idf_component.yml' "$1"; }
eidolon_lock_file()     { printf '%s/dependencies.lock' "$1"; }
eidolon_lk_component()  { printf '%s/managed_components/livekit__livekit' "$1"; }
eidolon_stamp_gen()     { printf '%s/main/eidolon/eidolon_build_stamp.gen.h' "$1"; }

eidolon__info() { echo ">> [common] $*"; }
eidolon__warn() { echo ">> [common] WARNING: $*" >&2; }

# Version currently pinned in main/idf_component.yml for livekit/livekit.
eidolon_pinned_sdk() {
  local manifest; manifest="$(eidolon_idf_manifest "$1")"
  [[ -f "${manifest}" ]] || { echo ""; return 0; }
  awk '
    /^[[:space:]]*livekit\/livekit:[[:space:]]*$/ { in_lk=1; next }
    in_lk && /version:/ {
      v=$0; sub(/.*version:[[:space:]]*/, "", v); gsub(/["'"'"']/, "", v);
      sub(/[[:space:]].*$/, "", v); print v; exit
    }
    in_lk && /^[^[:space:]]/ { in_lk=0 }
  ' "${manifest}"
}

# Version actually installed under managed_components (what got compiled in).
eidolon_installed_sdk() {
  local comp; comp="$(eidolon_lk_component "$1")/idf_component.yml"
  [[ -f "${comp}" ]] || { echo ""; return 0; }
  awk -F': *' '/^version:/ { gsub(/["'"'"' ]/, "", $2); print $2; exit }' "${comp}"
}

# Rewrite the livekit/livekit version pin in main/idf_component.yml in place.
eidolon__rewrite_pin() {
  local root="$1" want="$2" manifest tmp
  manifest="$(eidolon_idf_manifest "${root}")"
  tmp="${manifest}.tmp"
  awk -v want="${want}" '
    /^[[:space:]]*livekit\/livekit:[[:space:]]*$/ { in_lk=1; print; next }
    in_lk && /version:/ {
      ws=$0; sub(/version:.*/, "", ws);   # leading whitespace before "version:"
      printf "%sversion: \"%s\"\n", ws, want;
      in_lk=0; next
    }
    in_lk && /^[^[:space:]]/ { in_lk=0 }
    { print }
  ' "${manifest}" >"${tmp}" && mv "${tmp}" "${manifest}"
}

# Make the SDK pin authoritative and force a clean re-resolve when it drifts.
#   EIDOLON_LIVEKIT_SDK=0.3.7  -> rewrite the pin to that, then re-resolve.
#   (unset)                    -> leave the pin as-is, only re-resolve on drift.
eidolon_sdk_sync() {
  local root="$1"
  local pinned installed

  if [[ -n "${EIDOLON_LIVEKIT_SDK:-}" ]]; then
    pinned="$(eidolon_pinned_sdk "${root}")"
    if [[ "${pinned}" != "${EIDOLON_LIVEKIT_SDK}" ]]; then
      eidolon__info "Pinning livekit SDK ${pinned:-<none>} -> ${EIDOLON_LIVEKIT_SDK} (idf_component.yml)"
      eidolon__rewrite_pin "${root}" "${EIDOLON_LIVEKIT_SDK}"
    fi
  fi

  pinned="$(eidolon_pinned_sdk "${root}")"
  installed="$(eidolon_installed_sdk "${root}")"

  if [[ -z "${pinned}" ]]; then
    eidolon__warn "Could not read livekit pin from idf_component.yml; skipping SDK sync."
    return 0
  fi

  if [[ "${installed}" != "${pinned}" ]]; then
    eidolon__info "SDK drift: pinned=${pinned} installed=${installed:-<none>} -> forcing re-resolve"
    rm -f "$(eidolon_lock_file "${root}")"
    rm -rf "$(eidolon_lk_component "${root}")"
    # A LiveKit SDK version change alters component ABIs (esp_websocket_client,
    # av_render, esp_capture...). A stale incremental build dir then mixes old and
    # new objects and crashes at runtime (e.g. LoadProhibited inside
    # esp_websocket_client_init on control-room connect). Wipe the per-board build
    # outputs so the next build is a clean full rebuild.
    if [[ -d "${root}/build/eidolon" ]]; then
      eidolon__info "SDK changed -> wiping ${root}/build/eidolon for a clean rebuild"
      rm -rf "${root}/build/eidolon"
    fi
  else
    eidolon__info "SDK resolved: livekit ${installed} (matches pin)"
  fi
}

# Bake git commit / branch / pinned SDK into a gitignored header the app includes.
eidolon_write_build_stamp() {
  local root="$1" gen git_desc branch sdk dirty
  gen="$(eidolon_stamp_gen "${root}")"
  git_desc="$(git -C "${root}" rev-parse --short=9 HEAD 2>/dev/null || echo nogit)"
  branch="$(git -C "${root}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo nogit)"
  if [[ -n "$(git -C "${root}" status --porcelain 2>/dev/null)" ]]; then dirty="+dirty"; else dirty=""; fi
  sdk="$(eidolon_pinned_sdk "${root}")"

  mkdir -p "$(dirname "${gen}")"
  cat >"${gen}" <<EOF
// Generated by scripts/eidolon/eidolon-common.sh — do not edit, do not commit.
#pragma once
#define EIDOLON_BUILD_GIT    "${git_desc}${dirty}"
#define EIDOLON_BUILD_BRANCH "${branch}"
#define EIDOLON_BUILD_SDK    "${sdk:-unknown}"
EOF
  # Expected fingerprint for post-flash verification.
  printf 'git=%s%s branch=%s sdk=%s\n' "${git_desc}" "${dirty}" "${branch}" "${sdk:-unknown}" \
    >"${root}/.eidolon_expected_stamp"
  eidolon__info "Build stamp: git=${git_desc}${dirty} branch=${branch} sdk=${sdk:-unknown}"
}

# Prepare a build: call right before idf.py build/flash.
eidolon_prepare_build() {
  eidolon_sdk_sync "$1"
  eidolon_write_build_stamp "$1"
}

# Read the EIDOLON-BUILDSTAMP boot line back over serial and diff vs expected.
# Best-effort: resets the board, captures for a bounded time, degrades to a manual
# hint if pyserial / the port is unavailable.
eidolon_verify_flashed() {
  local root="$1" port="$2" timeout="${3:-12}"
  local expected="" actual=""
  if [[ -f "${root}/.eidolon_expected_stamp" ]]; then
    expected="$(cat "${root}/.eidolon_expected_stamp")"
  fi

  eidolon__info "Verifying flashed firmware on ${port} (expected: ${expected:-<unknown>})"
  actual="$(python3 - "${port}" "${timeout}" <<'PY' 2>/dev/null || true
import sys, time
try:
    import serial
except Exception:
    sys.exit(3)
port, tmo = sys.argv[1], float(sys.argv[2])
try:
    p = serial.Serial(port, 115200, timeout=1)
except Exception:
    sys.exit(4)
# Reset into the app: RTS=EN low pulse, DTR=IO0 high (normal boot).
p.dtr = False
p.rts = True; time.sleep(0.1); p.rts = False
deadline = time.time() + tmo
while time.time() < deadline:
    try:
        line = p.readline().decode("utf-8", "replace")
    except Exception:
        break
    i = line.find("EIDOLON-BUILDSTAMP")
    if i != -1:
        print(line[i:].strip()); break
PY
)"

  if [[ -z "${actual}" ]]; then
    eidolon__warn "Could not auto-read the build stamp (pyserial/port busy?)."
    eidolon__warn "Open a monitor and look for a line containing: EIDOLON-BUILDSTAMP"
    eidolon__warn "It must match: ${expected:-<unknown>}"
    return 0
  fi

  eidolon__info "Device reports: ${actual}"
  # Compare the git= and sdk= tokens against expected.
  local exp_git exp_sdk act_git act_sdk
  exp_git="$(sed -n 's/.*git=\([^ ]*\).*/\1/p' <<<"${expected}")"
  exp_sdk="$(sed -n 's/.*sdk=\([^ ]*\).*/\1/p' <<<"${expected}")"
  act_git="$(sed -n 's/.*git=\([^ ]*\).*/\1/p' <<<"${actual}")"
  act_sdk="$(sed -n 's/.*sdk=\([^ ]*\).*/\1/p' <<<"${actual}")"
  if [[ "${exp_git}" == "${act_git}" && "${exp_sdk}" == "${act_sdk}" ]]; then
    eidolon__info "VERIFIED: device runs the just-built firmware (git=${act_git} sdk=${act_sdk})."
  else
    eidolon__warn "MISMATCH: built git=${exp_git} sdk=${exp_sdk} but device git=${act_git} sdk=${act_sdk}."
    eidolon__warn "The device is NOT running what you just built (stale flash / wrong path / cached SDK)."
  fi
}
