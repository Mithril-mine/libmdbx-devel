#!/usr/bin/env bash
# Boot the Android emulator for the CI emulator cells (B5 pilot) with a
# bounded retry, replacing the boot half of
# reactivecircus/android-emulator-runner so a transient boot failure
# (#561, "Timeout waiting for emulator to boot") does not fail the whole
# build+test cell: the emulator is killed and the boot is retried once
# within the same step, and the build+tests still run exactly once after a
# successful boot. A genuinely broken emulator still fails the cell: the
# attempts and the per-attempt wait are bounded, and the last 50 lines of
# the emulator's own log are dumped on each failed attempt.
#
# Launch parameters mirror what android-emulator-runner did for the
# android-r27d-x86-64-emu cell: the hosted runners have no KVM, so the
# emulator runs TCG software emulation (-accel off) with the
# ANDROID_I_WANT_MY_TCG opt-in (the emulator 37.3.x refuses TCG without it),
# animations disabled, no window/audio/snapshots.
#
# Knobs (env): EMU_API_LEVEL (default 30), EMU_ARCH (default x86_64),
# EMU_BOOT_TIMEOUT_S per-attempt boot wait (default 900; the action's
# emulator-boot-timeout default was 600), EMU_BOOT_ATTEMPTS (default 2).
set -euo pipefail

API="${EMU_API_LEVEL:-30}"
ARCH="${EMU_ARCH:-x86_64}"
BOOT_TIMEOUT="${EMU_BOOT_TIMEOUT_S:-900}"
ATTEMPTS="${EMU_BOOT_ATTEMPTS:-2}"
AVD_NAME="mdbx-ci-emu-${API}-${ARCH}"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/usr/local/lib/android/sdk}}"
EMU_LOG="$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/android-emulator-boot-XXXXXX.log")"

resolve_tool() { # resolve_tool <name> <sdk-relative-fallback>
  if command -v "$1" >/dev/null 2>&1; then
    command -v "$1"
  else
    echo "$SDK/$2"
  fi
}
ADB="$(resolve_tool adb platform-tools/adb)"
SDKMANAGER="$(resolve_tool sdkmanager cmdline-tools/latest/bin/sdkmanager)"
AVDMANAGER="$(resolve_tool avdmanager cmdline-tools/latest/bin/avdmanager)"
for tool in "$ADB" "$SDKMANAGER" "$AVDMANAGER"; do
  if [ ! -x "$tool" ]; then
    echo "error: required Android SDK tool not found: $tool" >&2
    exit 2
  fi
done

echo "==> android emulator boot: api=${API} arch=${ARCH} sdk=${SDK}"

# Install BEFORE the emulator-binary check: the hosted images may ship
# without the emulator package (the 2026-11-23 nightly: the ubuntu-24.04
# image dropped it), and the sdkmanager below is exactly what restores it -
# demanding the binary up front was a chicken-and-egg failure. The licenses
# are accepted non-interactively first: a fresh $SDK without the acceptance
# markers would make the install fail silently.
yes | "$SDKMANAGER" --licenses >/dev/null 2>&1 || true
if ! "$SDKMANAGER" --install "platform-tools" "emulator" \
     "system-images;android-${API};default;${ARCH}" >/dev/null 2>&1; then
  echo "error: sdkmanager failed to install the emulator packages" >&2
  exit 1
fi
if [ ! -x "$SDK/emulator/emulator" ]; then
  echo "error: the emulator binary is still missing after the sdkmanager install: $SDK/emulator/emulator" >&2
  exit 2
fi

EMU_OPTS=(-no-window -gpu swiftshader_indirect -no-snapshot -noaudio -no-boot-anim)
if [ ! -e /dev/kvm ]; then
  # No KVM on the hosted runners: TCG software emulation. Needs both the
  # explicit -accel off and the ANDROID_I_WANT_MY_TCG env opt-in (set by
  # the caller), see the emulator 37.3.x refusal note in ci-run.yml.
  EMU_OPTS+=(-accel off)
fi

kill_emulator() { # kill_emulator <pid>
  "$ADB" -s "emulator-5554" emu kill >/dev/null 2>&1 || true
  if [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; then
    kill "$1" 2>/dev/null || true
    for _ in $(seq 1 15); do
      kill -0 "$1" 2>/dev/null || break
      sleep 1
    done
    kill -9 "$1" 2>/dev/null || true
  fi
  "$ADB" kill-server >/dev/null 2>&1 || true
  sleep 3 # let the console/adb ports settle before the next attempt
}

attempt=1
while :; do
  echo "==> emulator boot attempt ${attempt}/${ATTEMPTS}"
  echo no | "$AVDMANAGER" create avd --force --name "$AVD_NAME" \
    --package "system-images;android-${API};default;${ARCH}" >/dev/null
  "$ADB" start-server >/dev/null 2>&1 || true
  # ANDROID_EMULATOR_USE_SYSTEM_LIBS mirrors android-emulator-runner on Linux.
  ANDROID_EMULATOR_USE_SYSTEM_LIBS=1 \
    "$SDK/emulator/emulator" -avd "$AVD_NAME" -port 5554 "${EMU_OPTS[@]}" \
    >"$EMU_LOG" 2>&1 &
  emu_pid=$!
  booted=""
  waited=0
  while [ "$waited" -lt "$BOOT_TIMEOUT" ]; do
    if [ "$("$ADB" -s emulator-5554 shell getprop sys.boot_completed \
          2>/dev/null | tr -d '\r')" = "1" ]; then
      booted="yes"
      break
    fi
    if ! kill -0 "$emu_pid" 2>/dev/null; then
      echo "==> emulator process exited after ${waited}s of the ${BOOT_TIMEOUT}s wait"
      break
    fi
    sleep 5
    waited=$((waited + 5))
  done
  if [ -n "$booted" ]; then
    echo "==> emulator booted in ${waited}s (attempt ${attempt})"
    # disable-animations parity with android-emulator-runner (cosmetic).
    "$ADB" -s emulator-5554 shell settings put global window_animation_scale 0 \
      >/dev/null 2>&1 || true
    "$ADB" -s emulator-5554 shell settings put global transition_animation_scale 0 \
      >/dev/null 2>&1 || true
    "$ADB" -s emulator-5554 shell settings put global animator_duration_scale 0 \
      >/dev/null 2>&1 || true
    break
  fi
  echo "==> emulator boot attempt ${attempt} failed; last 50 lines of the emulator log:"
  tail -n 50 "$EMU_LOG" 2>/dev/null || true
  kill_emulator "$emu_pid"
  if [ "$attempt" -ge "$ATTEMPTS" ]; then
    echo "error: emulator did not boot within ${BOOT_TIMEOUT}s x ${ATTEMPTS} attempts" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
done
