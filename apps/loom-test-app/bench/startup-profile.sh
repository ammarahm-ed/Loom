#!/bin/bash
# Cold-start comparison of APKs under the same ART compilation state as a fresh install from Play:
# install, reset compilation, install the APK's baseline profile when it ships one (through
# androidx.profileinstaller), compile speed-profile, then launch the startup scenario N times.
#   bench/startup-profile.sh <serial> <launches> <name>=<apk> [<name>=<apk> ...]
# Prints TTID (am start -W) and first React content (BenchProbe `ready sinceStart`) per launch.
set -e
SERIAL=$1; N=$2; shift 2
adb() { command adb -s "$SERIAL" "$@"; }
for spec in "$@"; do
  name=${spec%%=*}; apk=${spec#*=}
  pkg=$(~/Library/Android/sdk/build-tools/37.0.0/aapt2 dump packagename "$apk" 2>/dev/null | tail -1)
  # A fresh install also drops any profile earlier runs collected (`compile --reset` needs root).
  adb uninstall "$pkg" >/dev/null 2>&1 || true
  adb install "$apk" >/dev/null
  if unzip -l "$apk" | grep -q "assets/dexopt/baseline.prof"; then
    adb shell am broadcast -a androidx.profileinstaller.action.INSTALL_PROFILE \
      -n "$pkg/androidx.profileinstaller.ProfileInstallReceiver" | grep -o "result=[0-9]*" || true
    sleep 2
  fi
  adb shell am force-stop "$pkg"
  adb shell cmd package compile -f -m speed-profile "$pkg" >/dev/null
  ttids=(); readys=()
  for i in $(seq "$N"); do
    adb shell am force-stop "$pkg"; sleep 1; adb logcat -c
    ttid=$(adb shell am start -S -W -n "$pkg/com.loomtestapp.MainActivity" --es scenario startup | sed -n 's/TotalTime: //p' | tr -d '\r')
    pid=$(adb shell pidof "$pkg" | tr -d '\r')
    ready=""
    for _ in $(seq 40); do
      ready=$(adb logcat -d -v raw --pid="$pid" -s LoomBench:I | sed -n 's/.*sinceStart=//p' | head -1)
      [ -n "$ready" ] && break; sleep 0.25
    done
    ttids+=("$ttid"); readys+=("$ready")
  done
  med() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
  echo "$name ttid=[${ttids[*]}] median=$(med "${ttids[@]}")  firstContent=[${readys[*]}] median=$(med "${readys[@]}")"
done
