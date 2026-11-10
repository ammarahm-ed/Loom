#!/bin/bash
# Builds the release APKs bench/run.js compares, arm64 only, into bench/out/:
#   stock  stock React Native from Maven
#   tuned  stock with the text and recycling flags the direct renderer forces on itself
#   off    Resha with every switch off
#   on     Resha with every switch on (JavaScript on the main thread)
#   js     Resha renderer + worker with JavaScript on React Native's own JS thread
# and keeps each build's unstripped native libraries in bench/out/sym-<variant>/ so simpleperf can
# symbolize profiles. Usage: bench/build.sh [variant...]   (default: stock tuned off on)
set -e
APP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$APP/bench/out"
mkdir -p "$OUT"
cd "$APP/android"
COMMON="-PreactNativeArchitectures=arm64-v8a --console=plain"
for variant in ${@:-stock tuned off on}; do
  {
    start=$(date +%s)
    case $variant in
      stock) env RESHA_BENCH_STOCK=1 ./gradlew :app:assembleRelease -PbenchVariant=stock $COMMON ;;
      tuned) env RESHA_BENCH_STOCK=1 ./gradlew :app:assembleRelease -PbenchVariant=tuned -PbenchTunedFlags=true $COMMON ;;
      off) ./gradlew :app:assembleRelease -PbenchVariant=off -PRN_UI_THREAD=false -PUI_THREAD_RENDERER=false -PUI_THREAD_RENDERER_WORKER=false $COMMON ;;
      on) ./gradlew :app:assembleRelease -PbenchVariant=on -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true $COMMON ;;
      js) ./gradlew :app:assembleRelease -PbenchVariant=js -PRN_UI_THREAD=false -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true $COMMON ;;
      *) echo "unknown variant $variant" >&2; exit 1 ;;
    esac
    cp app/build/outputs/apk/release/app-release.apk "$OUT/app-$variant.apk"
    rm -rf "$OUT/sym-$variant"
    mkdir -p "$OUT/sym-$variant"
    # merged_native_libs holds the libraries before stripping; the exact subpath varies across AGP
    # versions, so look it up.
    libs=$(find app/build/intermediates/merged_native_libs/release -type d -path '*arm64-v8a' | head -1)
    cp "$libs"/*.so "$OUT/sym-$variant/"
    echo "BUILT $variant in $(( $(date +%s) - start ))s"
  }
done
