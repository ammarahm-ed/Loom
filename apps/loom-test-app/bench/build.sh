#!/bin/bash
# Builds the release APKs bench/run.js compares, arm64 only, into .local/bench/ (repo root):
#   stock  stock React Native from Maven
#   tuned  stock with the text and recycling flags the Loom renderer forces on itself
#   off    Loom with every switch off
#   on     Loom with every switch on (JavaScript on the main thread)
#   js     Loom renderer + worker with JavaScript on React Native's own JS thread
# and keeps each build's unstripped native libraries in .local/bench/ (repo root)sym-<variant>/ so simpleperf can
# symbolize profiles. Usage: bench/build.sh [variant...]   (default: stock tuned off on)
set -e
APP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(cd "$APP/../.." && pwd)/.local/bench"
mkdir -p "$OUT"
cd "$APP/android"
ARCH=${ARCH:-arm64-v8a}
COMMON="-PreactNativeArchitectures=$ARCH --console=plain $GRADLE_EXTRA"
for variant in ${@:-stock tuned off on}; do
  {
    start=$(date +%s)
    # React Native's bundle task does not track files under node_modules, which is where Loom's
    # JavaScript overrides are materialized; drop its outputs so every build re-bundles.
    rm -rf app/build/generated/assets/react app/build/generated/res/react \
      app/build/generated/sourcemaps/react app/build/intermediates/sourcemaps/react
    case $variant in
      stock) env LOOM_BENCH_STOCK=1 ./gradlew :app:assembleRelease -PbenchVariant=stock $COMMON ;;
      tuned) env LOOM_BENCH_STOCK=1 ./gradlew :app:assembleRelease -PbenchVariant=tuned -PbenchTunedFlags=true $COMMON ;;
      off) ./gradlew :app:assembleRelease -PbenchVariant=off -Ploom.js=background -Ploom.renderer=false -Ploom.worker=false -Ploom.compileFromSource=true $COMMON ;;
      on) ./gradlew :app:assembleRelease -PbenchVariant=on -Ploom.js=main -Ploom.renderer=true -Ploom.worker=true -Ploom.compileFromSource=true $COMMON ;;
      js) ./gradlew :app:assembleRelease -PbenchVariant=js -Ploom.js=background -Ploom.renderer=true -Ploom.worker=true -Ploom.compileFromSource=true $COMMON ;;
      *) echo "unknown variant $variant" >&2; exit 1 ;;
    esac
    cp app/build/outputs/apk/release/app-release.apk "$OUT/app-$variant.apk"
    rm -rf "$OUT/sym-$variant"
    mkdir -p "$OUT/sym-$variant"
    # merged_native_libs holds the libraries before stripping; the exact subpath varies across AGP
    # versions, so look it up.
    libs=$(find app/build/intermediates/merged_native_libs/release -type d -path "*$ARCH" | head -1)
    cp "$libs"/*.so "$OUT/sym-$variant/"
    echo "BUILT $variant in $(( $(date +%s) - start ))s"
  }
done
