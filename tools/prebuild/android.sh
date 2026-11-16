#!/usr/bin/env bash
# Build the Android variants (debug, debugOptimized, release; 4 ABIs) the way RN's release does
# (.github/actions/build-android: publishAllToMavenTempLocal with stable Hermes) and publish
# io.github.ammarahm-ed:react-android (LOOM_JS=background, default) or react-android-jsmain (LOOM_JS=main)
# to the LOCAL Maven repo. LOOM_PROFILING=1 builds and publishes the release-profiling variant instead
# (react-android[-jsmain]-profiling; release only). Published config: loom.renderer=true,.
# Nothing is uploaded anywhere.
#
# Every flavor compiles Fabric's renderer out and keeps every export of React Native's library of the same
# flavor: two passes per flavor (gen-export-stubs.py). Release (not profiling) is also lean: no inspector,
# DevTools or tracing code. Disk: one variant at a time, objects dropped after each.
# LOOM_SKIP_PASS1=1 reuses the pass-1 libraries of an earlier run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/prebuild/casefs.sh"
WORK="${LOOM_PREBUILD_DIR:-$ROOT/.local/cs}"; loom_require_case_sensitive "$WORK"
SRC="$WORK/rn-src"
REPO="${LOOM_MAVEN_LOCAL:-$ROOT/.local/maven-local}"
export LOOM_MAVEN_REPO="$REPO"
export LOOM_JS="${LOOM_JS:-background}"
# The NDK React Native pins (libs.versions.toml ndkVersion), never a copy of the number: a React Native
# upgrade that moves it must not silently build with the old one.
NDK_VERSION="$(sed -n 's/^ndkVersion = "\(.*\)"/\1/p' "$SRC/packages/react-native/gradle/libs.versions.toml")"
[ -n "$NDK_VERSION" ] || { echo "no ndkVersion in $SRC/packages/react-native/gradle/libs.versions.toml" >&2; exit 1; }
export ANDROID_NDK="$ANDROID_HOME/ndk/$NDK_VERSION"
[ -d "$ANDROID_NDK" ] || { echo "NDK $NDK_VERSION is not installed ($ANDROID_NDK)" >&2; exit 1; }
BASE="${LOOM_BASELINE_DIR:-$ROOT/.local/rn-baseline/android}"
VER="$(node -p "require('$ROOT/packages/react-native-loom/overrides.json').upstream.version")"
cd "$SRC"
GRADLE=(./gradlew --init-script "$ROOT/tools/prebuild/loom-publish.init.gradle" -Preact.internal.useHermesStable=true -Ploom.renderer=true -Ploom.js="$LOOM_JS")
SUFFIX=""
if [ "${LOOM_PROFILING:-0}" = 1 ]; then
  GRADLE+=(-Ploom.releaseProfiling=true -Ploom.publishVariants=release)
  STUBS="$WORK/loom-stubs-profiling"; P1="$WORK/loom-pass1-profiling"
  VARIANTS=(release); SUFFIX="-profiling"
else
  GRADLE+=(-Ploom.leanRelease=true)
  STUBS="$WORK/loom-stubs"; P1="$WORK/loom-pass1"
  VARIANTS=(release debugOptimized debug)
fi
GRADLE+=("-Ploom.exportStubsDir=$STUBS")
CXX="$SRC/packages/react-native/ReactAndroid/.cxx"
cap() { echo "$(echo "${1:0:1}" | tr a-z A-Z)${1:1}"; }

for v in "${VARIANTS[@]}"; do
  if [ "${LOOM_SKIP_PASS1:-0}" != 1 ]; then
    # Pass 1: no stubs, unresolved names tolerated: which of RN's exports have no real code?
    python3 "$ROOT/tools/prebuild/gen-export-stubs.py" --pass1 "$STUBS/$v"
    "${GRADLE[@]}" ":packages:react-native:ReactAndroid:assemble$(cap $v)" "$@"
    rm -rf "$P1/$v"; mkdir -p "$P1/$v"
    while IFS= read -r so; do
      abi="$(basename "$(dirname "$so")")"; mkdir -p "$P1/$v/$abi"; cp "$so" "$P1/$v/$abi/"
    done < <(find "packages/react-native/ReactAndroid/build/intermediates/merged_native_libs/$v" -path '*/lib/*/libreactnative.so')
  fi
  python3 "$ROOT/tools/prebuild/gen-export-stubs.py" "$BASE/react-android-$VER-$v.aar" "$P1/$v" "$STUBS/$v"
  "${GRADLE[@]}" ":packages:react-native:ReactAndroid:assemble$(cap $v)" "$@"
  find "$CXX" \( -name '*.o' -o -name '*.a' \) -delete
done
LOOM_SKIP_NATIVE=1 "${GRADLE[@]}" :packages:react-native:ReactAndroid:publishAllPublicationsToLoomLocalRepository "$@"
# Verify the published AARs: prefab header paths must equal RN's baseline (case-sensitively), plus Loom's own.
AID=react-android; [ "$LOOM_JS" = main ] && AID=react-android-jsmain; AID="$AID$SUFFIX"
V="${LOOM_PUBLISH_VERSION:-$VER}"   # published version; the baseline is React Native's own ($VER)
# A publication with a single variant (release profiling) has no classifier on its AAR.
aar_of() { local f="$REPO/io/github/ammarahm-ed/$AID/$V/$AID-$V-$1.aar"; [ -f "$f" ] || f="$REPO/io/github/ammarahm-ed/$AID/$V/$AID-$V.aar"; echo "$f"; }
for variant in "${VARIANTS[@]}"; do
  "$ROOT/tools/prebuild/verify-aar-headers.sh" "$(aar_of $variant)" "$BASE/react-android-$VER-$variant.aar"
done
for variant in "${VARIANTS[@]}"; do
  "$ROOT/tools/prebuild/verify-aar-exports.sh" "$(aar_of $variant)" "$BASE/react-android-$VER-$variant.aar"
done
