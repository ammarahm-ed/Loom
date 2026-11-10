#!/bin/bash
# Swaps a freshly compiled Hermes bundle into existing release APKs, for JS-only changes (Bench.tsx,
# probes) where a full native build is not needed. Both variants run the same JS.
#   [ENTRY=index.probe.js] bench/repack-js.sh <in.apk>=<out.apk> [...]
set -e
APP="$(cd "$(dirname "$0")/.." && pwd)"
cd "$APP"
TMP=$(mktemp -d); mkdir -p "$TMP/assets"
npx react-native bundle --platform android --dev false --entry-file "${ENTRY:-index.js}" \
  --bundle-output "$TMP/index.js.bundle" --minify false > "$TMP/bundle.log"
node_modules/hermes-compiler/hermesc/osx-bin/hermesc -emit-binary -O -w \
  -out "$TMP/assets/index.android.bundle" "$TMP/index.js.bundle"
BT=$(ls -d ~/Library/Android/sdk/build-tools/* | sort -V | tail -1)
for spec in "$@"; do
  in=${spec%%=*}; out=${spec#*=}
  cp "$in" "$TMP/u.apk"
  (cd "$TMP" && zip -q -0 u.apk assets/index.android.bundle)
  "$BT/zipalign" -f -p 4 "$TMP/u.apk" "$TMP/a.apk"
  "$BT/apksigner" sign --ks android/app/debug.keystore --ks-pass pass:android \
    --ks-key-alias androiddebugkey --key-pass pass:android --out "$out" "$TMP/a.apk"
  echo "repacked $out"
done
rm -rf "$TMP"
