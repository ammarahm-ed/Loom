#!/bin/bash
# CPU-profiles one benchmark scenario on each build with simpleperf (call graphs, dwarf unwinding).
#
#   bench/profile.sh <adb serial> <scenario> [variant...]     e.g. bench/profile.sh QV7120NC26 burst-raf
#
# Needs the APKs and unstripped libraries from bench/build.sh, and a device on API 29+ (the app is
# <profileable android:shell="true">). Writes .local/bench/ (repo root)prof-<scenario>-<variant>.data and a
# symbol cache in .local/bench/ (repo root)binary_cache-<variant>; summarize them with bench/profile-report.sh.
#
# Scenarios:
#   startup     records the first 3 s of a cold start, from process creation
#   burst-raf   records 6 s of the commit burst, which starts 1.5 s after the first render
#   anything else is recorded for 6 s, starting 2 s after launch
set -e
SERIAL=$1
SCENARIO=$2
shift 2
VARIANTS=${@:-stock off on}
APP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(cd "$APP/../.." && pwd)/.local/bench"
NDK=$(ls -d ~/Library/Android/sdk/ndk/* | sort -V | tail -1)
SP_DEVICE=/data/local/tmp/simpleperf
adb() { command adb -s "$SERIAL" "$@"; }

# Push the simpleperf build matching the device: 32-bit-only phones (armeabi-v7a) need the arm one.
case "$(adb shell getprop ro.product.cpu.abi | tr -d '\r')" in
  arm64-v8a) SP_ARCH=arm64 ;; armeabi-v7a|armeabi) SP_ARCH=arm ;; x86_64) SP_ARCH=x86_64 ;; x86) SP_ARCH=x86 ;; *) SP_ARCH=arm64 ;;
esac
adb push "$NDK/simpleperf/bin/android/$SP_ARCH/simpleperf" $SP_DEVICE >/dev/null
adb shell chmod +x $SP_DEVICE

for v in $VARIANTS; do
  PKG=com.loomtestapp.bench.$v
  DATA=/data/local/tmp/prof-$SCENARIO-$v.data
  # Same compilation state as bench/run.js: clean install, baseline profile, speed-profile AOT.
  # SKIP_INSTALL=1 profiles the build already installed (same APK as the last bench/run.js run).
  if [ -z "$SKIP_INSTALL" ]; then
  adb uninstall "$PKG" >/dev/null 2>&1 || true
  adb install "$OUT/app-$v.apk" >/dev/null
  adb shell am broadcast -a androidx.profileinstaller.action.INSTALL_PROFILE \
    -n "$PKG/androidx.profileinstaller.ProfileInstallReceiver" >/dev/null || true
  sleep 1.5
  adb shell am force-stop "$PKG"
  adb shell cmd package compile -f -m speed-profile "$PKG" >/dev/null
  fi
  adb shell am force-stop $PKG
  sleep 1
  if [ "$SCENARIO" = startup ]; then
    # --app waits for the process to appear, so the launch is captured from the start.
    adb shell $SP_DEVICE record --app $PKG -g -f 4000 --duration 3 -o $DATA &
    REC=$!
    sleep 1
    adb shell am start -n $PKG/com.loomtestapp.MainActivity --es scenario startup >/dev/null
    wait $REC
  else
    adb shell am start -n $PKG/com.loomtestapp.MainActivity --es scenario "$SCENARIO" >/dev/null
    sleep 2
    # PROFILE_INPUT: an `adb shell input ...` command repeated every PROFILE_INPUT_EVERY seconds
    # while recording, for scenarios that need touch (e.g. "swipe 360 1500 360 400 120", "tap 360 290").
    if [ -n "$PROFILE_INPUT" ]; then
      ( for _ in $(seq 12); do adb shell input $PROFILE_INPUT; sleep "${PROFILE_INPUT_EVERY:-0.5}"; done ) &
      INPUT_PID=$!
    fi
    adb shell $SP_DEVICE record --app $PKG -g -f 4000 --duration 6 -o $DATA
    [ -n "$PROFILE_INPUT" ] && wait $INPUT_PID
  fi
  adb shell am force-stop $PKG
  adb pull $DATA "$OUT/prof-$SCENARIO-$v.data" >/dev/null
  # Symbolize against this build's unstripped libraries (system libraries are pulled from the device).
  (cd "$OUT" && python3 "$NDK/simpleperf/binary_cache_builder.py" -i "prof-$SCENARIO-$v.data" \
      -lib "sym-$v" --disable_adb_root >/dev/null 2>&1 && rm -rf "binary_cache-$v" && mv binary_cache "binary_cache-$v")
  echo "profiled $SCENARIO on $v"
done
