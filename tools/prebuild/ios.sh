#!/usr/bin/env bash
# Build the rncore xcframeworks the way RN's .github/workflows/prebuild-ios-core.yml does (slices ios,
# ios-simulator, mac-catalyst) for the published Loom configuration: loom.renderer=true, worker on,
# JavaScript thread chosen at app startup (not a build axis). Tarballs land in $OUT/ in RN's naming:
#   ReactCore{Debug,Release}.xcframework.tar.gz, ReactCore{..}.framework.dSYM.tar.gz,
#   ReactNativeHeaders{..}.xcframework.tar.gz
# ReactNativeDependencies and Hermes are RN's own (the prebuild setup step downloads them from
# com.facebook.react:react-native-artifacts and com.facebook.hermes:hermes-ios): Loom does not change them.
# Flavors: Debug, Release (lean), ReleaseProfiling (Release with DevTools/tracing). Every flavor compiles Fabric's
# renderer out and keeps all of RN's exports of the same flavor (two passes, export stubs).
# Usage: ios.sh [Debug] [Release] [ReleaseProfiling]   (default: all three)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/prebuild/casefs.sh"
WORK="${LOOM_PREBUILD_DIR:-$ROOT/.local/cs}"; loom_require_case_sensitive "$WORK"
. "$ROOT/tools/prebuild/ccache.sh"; loom_ccache_setup "$WORK"
trap 'command -v ccache >/dev/null && [ "${LOOM_CCACHE:-1}" = 1 ] && ccache --show-stats || true' EXIT
SRC="$WORK/rn-src/packages/react-native"
OUT="${LOOM_IOS_OUT:-$WORK/ios-out}"
export HERMES_VERSION="$(sed -n 's/^HERMES_VERSION_NAME=//p' "$SRC/sdks/hermes-engine/version.properties")"
# Read by the patched Package.swift (tools/prebuild/package-swift-loom.patch).
export RCT_LOOM_RENDERER_IOS=1
mkdir -p "$OUT"
cd "$SRC"
if [ $# -gt 0 ]; then FLAVORS=("$@"); else FLAVORS=(Debug Release ReleaseProfiling); fi
BASE_DIR="${LOOM_BASELINE_DIR:-$ROOT/.local/rn-baseline/ios}"
VERSION="$(node -p "require('$ROOT/packages/react-native-loom/overrides.json').upstream.version")"
STUBS="$SRC/loom-stubs"

# build_flavor <xcode configuration> <stubs dir of this flavor>: the three slices and the xcframework
build_flavor() {
  local C="$1" G="$2" S SLICE
  rm -rf ".build/output/spm/$C" ".build/output/xcframeworks/$C"   # macros changed between variants
  node scripts/ios-prebuild.js -s -f "$C"
  for S in ios ios-simulator mac-catalyst; do
    case "$S" in
      ios) SLICE=ios-arm64 ;; ios-simulator) SLICE=ios-arm64_x86_64-simulator ;; *) SLICE=ios-arm64_x86_64-maccatalyst ;;
    esac
    rm -rf "$STUBS"; cp -R "$G/$SLICE" "$STUBS"
    node scripts/ios-prebuild -b -f "$C" -p "$S"
  done
  node scripts/ios-prebuild -c -f "$C" --require-hermes
}
package_flavor() {
  local C="$1" N="$2" X=".build/output/xcframeworks/$1"
  node scripts/ios-prebuild/headers-verify.js --flavor "$C"
  (cd "$X" && tar -cz -f "$OUT/ReactCore$N.xcframework.tar.gz" React.xcframework ReactNativeHeaders.xcframework)
  (cd "$X/Symbols" && tar -cz -f "$OUT/ReactCore$N.framework.dSYM.tar.gz" .)
  cp "$X/ReactNativeHeaders.xcframework.tar.gz" "$OUT/ReactNativeHeaders$N.xcframework.tar.gz"
}
# two_pass <configuration> <name> <baseline classifier>: pass 1 (no stubs, unresolved names tolerated) learns which
# of RN's exports have no real code; the final build links a stub for each (gen-export-stubs-ios.py).
two_pass() {
  local C="$1" N="$2" B="$3"
  local G="$WORK/loom-ios-stubs-$N" P="$WORK/loom-ios-pass1-$N"
  export RCT_LOOM_STUBS_IOS=1
  if [ "${LOOM_SKIP_PASS1:-0}" != 1 ]; then
    rm -rf "$G" "$P" "$STUBS"; mkdir -p "$P" "$G"
    for SLICE in ios-arm64 ios-arm64_x86_64-simulator ios-arm64_x86_64-maccatalyst; do
      python3 "$ROOT/tools/prebuild/gen-export-stubs-ios.py" --pass1 "$G/$SLICE"
    done
    export RCT_LOOM_STUBS_PASS1_IOS=1
    build_flavor "$C" "$G"
    cp -R ".build/output/xcframeworks/$C/React.xcframework" "$P/"
    unset RCT_LOOM_STUBS_PASS1_IOS
  fi
  python3 "$ROOT/tools/prebuild/gen-export-stubs-ios.py" "$BASE_DIR/react-native-artifacts-$VERSION-reactnative-core-$B.tar.gz" "$P/React.xcframework" "$G"
  build_flavor "$C" "$G"; package_flavor "$C" "$N"
  "$ROOT/tools/prebuild/verify-ios-exports.sh" "$OUT/ReactCore$N.xcframework.tar.gz" "$BASE_DIR/react-native-artifacts-$VERSION-reactnative-core-$B.tar.gz"
  rm -rf "$SRC/.build/output/spm/$C" "$SRC/.build/output/xcframeworks/$C/Symbols"   # disk
}

for F in "${FLAVORS[@]}"; do
  unset RCT_LOOM_STUBS_IOS RCT_LOOM_LEAN_RELEASE_IOS RCT_LOOM_STUBS_PASS1_IOS RCT_LOOM_PROFILING_IOS RCT_LOOM_STRIP_LOCALS_IOS
  case "$F" in
    Debug) two_pass Debug Debug debug ;;
    ReleaseProfiling)
      # Release with DevTools and tracing kept (React Native's profiling build mode): not lean.
      export RCT_LOOM_PROFILING_IOS=1 RCT_LOOM_STRIP_LOCALS_IOS=1
      two_pass Release ReleaseProfiling release ;;
    Release)
      # Lean: also no inspector/DevTools/tracing.
      export RCT_LOOM_LEAN_RELEASE_IOS=1 RCT_LOOM_STRIP_LOCALS_IOS=1
      two_pass Release Release release ;;
    *) echo "unknown flavor $F" >&2; exit 2 ;;
  esac
done
ls -l "$OUT"
