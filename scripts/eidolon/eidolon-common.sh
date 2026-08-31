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
eidolon__die()  { echo ">> [common] ERROR: $*" >&2; exit 1; }

# ---- ESP-IDF toolchain selection -------------------------------------------
#
# The IDF version is a BOARD ATTRIBUTE. Every board script declares
# BOARD_IDF_VERSION next to BOARD_TARGET, and this resolver honours it exactly.
# It is deliberately NOT an ambient property of whichever shell you happen to
# be in.
#
# This replaces five copy-pasted per-board resolvers that globbed
# ~/.espressif/v*/esp-idf/export.sh and sorted DESCENDING, i.e. "use the newest
# IDF installed". Installing a second IDF therefore silently re-pointed every
# existing board at it from any shell that had not already sourced export.sh —
# and a board built against the wrong toolchain fails in ways that look like
# firmware bugs. So:
#
#   * resolution is keyed on the requested version and never falls back to a
#     different one;
#   * an already-exported IDF is reused only when it IS that version, otherwise
#     the environment is scrubbed and the right one is loaded;
#   * after loading, `idf.py --version` is VERIFIED against the request and the
#     build is refused on a mismatch;
#   * the resolved version is baked into the build stamp, so a wrong toolchain
#     shows up in the first screen of serial output instead of being inferred
#     from behaviour days later.
#
# Escape hatches, in the order they are consulted:
#   EIDOLON_IDF_VERSION=6.1        override the REQUIREMENT (recorded, verified)
#   EIDOLON_IDF_EXPORT=/x/export.sh  where an IDF lives (still verified)
#   EIDOLON_IDF_PATH=/x/esp-idf      where an IDF lives (still verified)
# There is deliberately no global idf.path file any more: one path cannot be
# correct for boards that require different IDF versions.

# Version of the idf.py currently on PATH, normalized to "5.5.4" ("" if none).
eidolon_idf_current_version() {
  command -v idf.py >/dev/null 2>&1 || { echo ""; return 0; }
  idf.py --version 2>/dev/null | sed -n 's/.*[vV]\([0-9][0-9.]*\).*/\1/p' | head -1
}

eidolon__idf_export_candidates() {
  local want="$1" home="${HOME:-}"
  [[ -n "${EIDOLON_IDF_EXPORT:-}" ]] && printf '%s\n' "${EIDOLON_IDF_EXPORT}"
  [[ -n "${EIDOLON_IDF_PATH:-}" ]] && printf '%s/export.sh\n' "${EIDOLON_IDF_PATH%/}"
  if [[ -n "${home}" ]]; then
    printf '%s\n' \
      "${home}/.espressif/v${want}/esp-idf/export.sh" \
      "${home}/esp/v${want}/esp-idf/export.sh" \
      "${home}/esp/esp-idf-v${want}/export.sh" \
      "${home}/esp-idf-v${want}/export.sh"
  fi
}

# Resolve + export the IDF this board requires. Returns non-zero instead of
# exiting, so callers that only probe (status displays) stay non-fatal.
# On success EIDOLON_RESOLVED_IDF holds the verified version.
eidolon_idf_ensure() {
  local want="${EIDOLON_IDF_VERSION:-${1:-}}"
  want="${want#v}"
  if [[ -z "${want}" ]]; then
    eidolon__warn "no IDF version requested; board script must set BOARD_IDF_VERSION"
    return 1
  fi

  local current
  current="$(eidolon_idf_current_version)"
  if [[ "${current}" == "${want}" ]] && command -v ninja >/dev/null 2>&1; then
    EIDOLON_RESOLVED_IDF="${current}"
    export EIDOLON_RESOLVED_IDF
    return 0
  fi
  if [[ -n "${current}" && "${current}" != "${want}" ]]; then
    eidolon__info "ESP-IDF v${current} is exported but this board requires v${want}; switching"
    unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION
  fi

  local export_sh found=""
  while IFS= read -r export_sh; do
    [[ -f "${export_sh}" ]] || continue
    found="${export_sh}"
    eidolon__info "Loading ESP-IDF v${want}: ${export_sh}"
    # export.sh is not written against `set -euo pipefail`; relax it for the
    # source and restore exactly what the caller had, rather than assuming.
    local __opts="$-"
    set +eu
    # shellcheck source=/dev/null
    source "${export_sh}" >/dev/null
    [[ "${__opts}" == *e* ]] && set -e
    [[ "${__opts}" == *u* ]] && set -u
    break
  done < <(eidolon__idf_export_candidates "${want}" | awk '!seen[$0]++')

  if [[ -z "${found}" ]]; then
    eidolon__warn "ESP-IDF v${want} not found. Install it (Espressif IDE Manager puts it in ~/.espressif/v${want}/esp-idf) or set EIDOLON_IDF_PATH."
    return 1
  fi

  current="$(eidolon_idf_current_version)"
  if [[ "${current}" != "${want}" ]]; then
    eidolon__warn "${found} exported ESP-IDF v${current:-<unknown>}, but this board requires v${want}."
    return 1
  fi
  if ! command -v ninja >/dev/null 2>&1; then
    eidolon__warn "ESP-IDF v${current} exported but ninja is missing; run its install.sh."
    return 1
  fi

  EIDOLON_RESOLVED_IDF="${current}"
  export EIDOLON_RESOLVED_IDF
  eidolon__info "ESP-IDF v${current} ready (${IDF_PATH:-?})"
  return 0
}

# Hard requirement: refuse to build rather than silently use another toolchain.
eidolon_require_idf() {
  local want="${EIDOLON_IDF_VERSION:-${1:-}}"
  eidolon_idf_ensure "${1:-}" ||
    eidolon__die "this board requires ESP-IDF v${want#v}; refusing to build against anything else. Install it, or set EIDOLON_IDF_VERSION to make the change explicit."
}

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

# Version actually resolved by the component manager (what gets compiled in).
#
# Git dependencies carry the pinned commit in dependencies.lock while their
# own idf_component.yml still contains the library's semantic release version.
# Comparing that semantic version to a commit pin makes every build look stale
# and causes flash to destroy the build it is supposed to install.
eidolon_installed_sdk() {
  local lock comp resolved
  lock="$(eidolon_lock_file "$1")"
  if [[ -f "${lock}" ]]; then
    resolved="$(awk '
      /^  livekit\/livekit:[[:space:]]*$/ { in_lk=1; next }
      in_lk && /^    version:[[:space:]]*/ {
        v=$0; sub(/.*version:[[:space:]]*/, "", v); gsub(/["'"'"' ]/, "", v);
        print v; exit
      }
      in_lk && /^  [^[:space:]]/ { in_lk=0 }
    ' "${lock}")"
    if [[ -n "${resolved}" ]]; then
      printf '%s\n' "${resolved}"
      return 0
    fi
  fi
  comp="$(eidolon_lk_component "$1")/idf_component.yml"
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
  local root="$1" gen git_desc branch sdk dirty idf
  gen="$(eidolon_stamp_gen "${root}")"
  git_desc="$(git -C "${root}" rev-parse --short=9 HEAD 2>/dev/null || echo nogit)"
  branch="$(git -C "${root}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo nogit)"
  if [[ -n "$(git -C "${root}" status --porcelain 2>/dev/null)" ]]; then dirty="+dirty"; else dirty=""; fi
  sdk="$(eidolon_pinned_sdk "${root}")"
  # The toolchain this image was actually compiled with. Verified by
  # eidolon_idf_ensure, not guessed from the environment.
  idf="${EIDOLON_RESOLVED_IDF:-$(eidolon_idf_current_version)}"

  mkdir -p "$(dirname "${gen}")"
  cat >"${gen}" <<EOF
// Generated by scripts/eidolon/eidolon-common.sh — do not edit, do not commit.
#pragma once
#define EIDOLON_BUILD_GIT    "${git_desc}${dirty}"
#define EIDOLON_BUILD_BRANCH "${branch}"
#define EIDOLON_BUILD_SDK    "${sdk:-unknown}"
#define EIDOLON_BUILD_IDF    "${idf:-unknown}"
EOF
  # Expected fingerprint for post-flash verification.
  printf 'git=%s%s branch=%s sdk=%s idf=%s\n' "${git_desc}" "${dirty}" "${branch}" \
    "${sdk:-unknown}" "${idf:-unknown}" >"${root}/.eidolon_expected_stamp"
  eidolon__info "Build stamp: git=${git_desc}${dirty} branch=${branch} sdk=${sdk:-unknown} idf=${idf:-unknown}"
}

# Prepare a build: call right before idf.py build/flash.
#
# The toolchain is resolved FIRST, on purpose. A build cannot be prepared
# without knowing which IDF it targets, and the build stamp records that
# version — resolving later would stamp "unknown" and defeat the check.
eidolon_prepare_build() {
  eidolon_require_idf "${2:-${BOARD_IDF_VERSION:-}}"
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
  local exp_git exp_sdk exp_idf act_git act_sdk act_idf
  exp_git="$(sed -n 's/.*git=\([^ ]*\).*/\1/p' <<<"${expected}")"
  exp_sdk="$(sed -n 's/.*sdk=\([^ ]*\).*/\1/p' <<<"${expected}")"
  exp_idf="$(sed -n 's/.*idf=\([^ ]*\).*/\1/p' <<<"${expected}")"
  act_git="$(sed -n 's/.*git=\([^ ]*\).*/\1/p' <<<"${actual}")"
  act_sdk="$(sed -n 's/.*sdk=\([^ ]*\).*/\1/p' <<<"${actual}")"
  act_idf="$(sed -n 's/.*idf=\([^ ]*\).*/\1/p' <<<"${actual}")"
  if [[ "${exp_git}" == "${act_git}" && "${exp_sdk}" == "${act_sdk}" &&
        "${exp_idf}" == "${act_idf}" ]]; then
    eidolon__info "VERIFIED: device runs the just-built firmware (git=${act_git} sdk=${act_sdk} idf=${act_idf})."
  else
    eidolon__warn "MISMATCH: built git=${exp_git} sdk=${exp_sdk} idf=${exp_idf} but device git=${act_git} sdk=${act_sdk} idf=${act_idf}."
    eidolon__warn "The device is NOT running what you just built (stale flash / wrong path / cached SDK / wrong toolchain)."
  fi
}

# ---------------------------------------------------------------------------
# Private setup-secret overlay.
#
# Development Admission needs CONFIG_EIDOLON_ADMISSION_SETUP_SECRET_HEX, and the
# secret must never live in the repository. The caller points
# EIDOLON_PRIVATE_SDKCONFIG_OVERLAY at a 0600 file outside the tree holding
# exactly that one setting; it is validated, sealed into a 0700 build directory
# and handed to IDF from there, so a rename after validation cannot swap the
# secret that reaches Kconfig.
#
# Without it a board builds and flashes perfectly and then fails only once it
# reaches a Hub, with "Development Admission setup secret is not provisioned"
# and ESP_ERR_NOT_SUPPORTED. This lived in the esp-box-3 script alone, which is
# why every other board hit that wall.
#
# On success it redirects BUILD_DIR, SDKCONFIG_FILE and SDKCONFIG_OVERLAY into
# the private directory and sets PRIVATE_SDKCONFIG_OVERLAY; callers add that to
# SDKCONFIG_DEFAULTS last. With the variable unset it returns immediately and
# the public build directory is used unchanged.
# ---------------------------------------------------------------------------
eidolon_stat_uid() {
  if stat -f '%u' "$1" >/dev/null 2>&1; then
    stat -f '%u' "$1"
  else
    stat -c '%u' "$1"
  fi
}

eidolon_stat_mode() {
  if stat -f '%Lp' "$1" >/dev/null 2>&1; then
    stat -f '%Lp' "$1"
  else
    stat -c '%a' "$1"
  fi
}

eidolon_stat_identity() {
  if stat -f '%d:%i' "$1" >/dev/null 2>&1; then
    stat -f '%d:%i' "$1"
  else
    stat -c '%d:%i' "$1"
  fi
}

eidolon_canonical_existing_path() {
  local input="$1"
  local directory base
  directory="$(cd "$(dirname "${input}")" && pwd -P)"
  base="$(basename "${input}")"
  printf '%s/%s\n' "${directory}" "${base}"
}

eidolon_configure_private_sdkconfig_overlay() {
  local board="$1"
  local requested="${EIDOLON_PRIVATE_SDKCONFIG_OVERLAY:-}"
  [[ -n "${requested}" ]] || return 0

  [[ "${requested}" == /* ]] ||
    die "EIDOLON_PRIVATE_SDKCONFIG_OVERLAY must be an absolute path"
  local requested_parent resolved_parent
  requested_parent="$(dirname "${requested}")"
  [[ -d "${requested_parent}" && ! -L "${requested_parent}" ]] ||
    die "private sdkconfig overlay parent must be a non-symlink directory"
  resolved_parent="$(cd "${requested_parent}" && pwd -P)"
  [[ "${requested_parent}" == "${resolved_parent}" ]] ||
    die "private sdkconfig overlay parent must not traverse symlinks"
  [[ "$(eidolon_stat_uid "${resolved_parent}")" == "$(id -u)" ]] ||
    die "private sdkconfig overlay parent must be owned by the calling user"
  [[ "$(eidolon_stat_mode "${resolved_parent}")" == "700" ]] ||
    die "private sdkconfig overlay parent permissions must be 0700"

  [[ -f "${requested}" && ! -L "${requested}" ]] ||
    die "private sdkconfig overlay must be a regular, non-symlink file"

  local resolved file_uid file_mode file_size setting identity_before identity_after
  resolved="$(eidolon_canonical_existing_path "${requested}")"
  case "${resolved}" in
    "${PROJECT_ROOT}"|"${PROJECT_ROOT}"/*)
      die "private sdkconfig overlay must be outside the repository"
      ;;
  esac

  file_uid="$(eidolon_stat_uid "${resolved}")"
  [[ "${file_uid}" == "$(id -u)" ]] ||
    die "private sdkconfig overlay must be owned by the calling user"
  file_mode="$(eidolon_stat_mode "${resolved}")"
  [[ "${file_mode}" == "600" ]] ||
    die "private sdkconfig overlay permissions must be 0600"
  identity_before="$(eidolon_stat_identity "${resolved}")"
  file_size="$(wc -c <"${resolved}" | tr -d '[:space:]')"
  [[ "${file_size}" =~ ^[0-9]+$ ]] &&
    ((file_size > 0 && file_size <= 256)) ||
    die "private sdkconfig overlay has an invalid size"

  setting="$(cat "${resolved}")"
  identity_after="$(eidolon_stat_identity "${resolved}")"
  [[ "${identity_before}" == "${identity_after}" ]] ||
    die "private sdkconfig overlay changed during validation"
  [[ "${setting}" =~ ^CONFIG_EIDOLON_ADMISSION_SETUP_SECRET_HEX=\"[0-9a-f]{64}\"$ ]] ||
    die "private sdkconfig overlay must contain exactly the supported setup-secret setting"
  local setting_size="${#setting}"
  ((file_size == setting_size || file_size == setting_size + 1)) ||
    die "private sdkconfig overlay must contain exactly one setting line"

  local private_root="${resolved}.build"
  if [[ -e "${private_root}" ]]; then
    [[ -d "${private_root}" && ! -L "${private_root}" ]] ||
      die "private build root must be a non-symlink directory"
    [[ "$(eidolon_stat_uid "${private_root}")" == "$(id -u)" ]] ||
      die "private build root must be owned by the calling user"
  fi
  local private_build="${private_root}/${board}"
  if [[ -e "${private_build}" ]]; then
    [[ -d "${private_build}" && ! -L "${private_build}" ]] ||
      die "private build directory must be a non-symlink directory"
    [[ "$(eidolon_stat_uid "${private_build}")" == "$(id -u)" ]] ||
      die "private build directory must be owned by the calling user"
  fi
  (umask 077; mkdir -p "${private_build}")
  chmod 0700 "${private_root}" "${private_build}"
  [[ "$(eidolon_stat_uid "${resolved_parent}")" == "$(id -u)" &&
     "$(eidolon_stat_mode "${resolved_parent}")" == "700" &&
     ! -L "${resolved_parent}" ]] ||
    die "private sdkconfig overlay parent changed during validation"
  [[ "$(eidolon_stat_uid "${private_root}")" == "$(id -u)" &&
     "$(eidolon_stat_mode "${private_root}")" == "700" &&
     ! -L "${private_root}" ]] ||
    die "private build root failed secure revalidation"
  [[ "$(eidolon_stat_uid "${private_build}")" == "$(id -u)" &&
     "$(eidolon_stat_mode "${private_build}")" == "700" &&
     ! -L "${private_build}" ]] ||
    die "private build directory failed secure revalidation"

  # IDF reads this sealed snapshot, not the caller path. A rename after
  # validation therefore cannot change the secret that reaches Kconfig.
  local sealed_overlay="${private_root}/sdkconfig.private.${board}"
  local sealed_temporary="${sealed_overlay}.tmp.$$"
  (umask 077; printf '%s\n' "${setting}" >"${sealed_temporary}")
  chmod 0600 "${sealed_temporary}"
  mv "${sealed_temporary}" "${sealed_overlay}"
  [[ -f "${sealed_overlay}" && ! -L "${sealed_overlay}" &&
     "$(eidolon_stat_uid "${sealed_overlay}")" == "$(id -u)" &&
     "$(eidolon_stat_mode "${sealed_overlay}")" == "600" ]] ||
    die "sealed private sdkconfig overlay failed secure revalidation"

  PRIVATE_SDKCONFIG_OVERLAY="${sealed_overlay}"
  BUILD_DIR="${private_build}"
  SDKCONFIG_FILE="${BUILD_DIR}/sdkconfig.${board}"
  SDKCONFIG_OVERLAY="${BUILD_DIR}/sdkconfig.overlay.${board}"
}
