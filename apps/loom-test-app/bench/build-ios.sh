#!/bin/bash
# Builds one of the three Release iOS apps bench/run-ios.js compares. Each gets its own bundle id,
# so all of them stay installed side by side.
#
#   bench/build-ios.sh stock   com.loomtestapp.bench.stock  stock React Native, prebuilt core
#   bench/build-ios.sh on      com.loomtestapp.bench.on     Loom renderer + worker, JS on main
#   bench/build-ios.sh js      com.loomtestapp.bench.js     Loom renderer + worker, JS on its own thread
#
# Environment:
#   DEVELOPMENT_TEAM  signing team (required)
#   DD_ROOT           DerivedData root (default <repo>/.local/bench/ios-dd); one folder per config
#   DEST              xcodebuild destination (default generic/platform=iOS)
#   SKIP_POD=1        reuse the Pods project of the previous run (same config only)
#   XCODE_ARGS        extra xcodebuild arguments (default CODE_SIGN_INJECT_BASE_ENTITLEMENTS=YES so
#                     Instruments can attach to the Release build)
#   INSTALL_DEVICE    devicectl device id: install the built app there afterwards
#
# `pod install` runs on every invocation: the Loom switches are read by the podspecs, so the Pods
# project has to match the config being built. The log goes to $DD_ROOT/build-<config>.log.
set -o pipefail
CONFIG=${1:?usage: build-ios.sh stock|on|js}
: "${DEVELOPMENT_TEAM:?set DEVELOPMENT_TEAM}"
APP=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
DD_ROOT=${DD_ROOT:-$REPO/.local/bench/ios-dd}
mkdir -p "$DD_ROOT"

case $CONFIG in
  stock) export LOOM_BENCH_STOCK=1; LOOM_ENV=(env)
         DEFS="LOOM_BENCH_RENDERER=0 LOOM_BENCH_WORKER=0 LOOM_BENCH_JS_ON_MAIN=1 LOOM_BENCH_STOCK=1" ;;
  on)    unset LOOM_BENCH_STOCK; LOOM_ENV=(env loom.renderer=true loom.js=main loom.compileFromSource=true)
         DEFS="LOOM_BENCH_RENDERER=1 LOOM_BENCH_WORKER=1 LOOM_BENCH_JS_ON_MAIN=1 LOOM_BENCH_STOCK=0" ;;
  js)    unset LOOM_BENCH_STOCK; LOOM_ENV=(env loom.renderer=true loom.js=background loom.compileFromSource=true)
         DEFS="LOOM_BENCH_RENDERER=1 LOOM_BENCH_WORKER=1 LOOM_BENCH_JS_ON_MAIN=0 LOOM_BENCH_STOCK=0" ;;
  *) echo "unknown config $CONFIG" >&2; exit 2 ;;
esac

LOG=$DD_ROOT/build-$CONFIG.log
cd "$APP" || exit 1
if [ "$CONFIG" != stock ]; then
  node ../../packages/react-native-loom/bin/loom.js materialize || exit 1
fi
if [ -n "$SKIP_POD" ]; then : > "$LOG"; else (cd ios && "${LOOM_ENV[@]}" pod install) > "$LOG" 2>&1; fi || { echo "pod install failed, see $LOG"; exit 1; }
cd ios || exit 1
xcodebuild -workspace LoomTestApp.xcworkspace -scheme LoomTestApp -configuration Release \
  -destination "${DEST:-generic/platform=iOS}" -derivedDataPath "$DD_ROOT/dd-$CONFIG" \
  DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" CODE_SIGN_STYLE=Automatic -allowProvisioningUpdates \
  PRODUCT_BUNDLE_IDENTIFIER=com.loomtestapp.bench.$([ "$CONFIG" = stock ] && echo stock || echo "$CONFIG") \
  "GCC_PREPROCESSOR_DEFINITIONS=\$(inherited) $DEFS" \
  ${XCODE_ARGS-CODE_SIGN_INJECT_BASE_ENTITLEMENTS=YES} build >> "$LOG" 2>&1
STATUS=$?
echo "exit $STATUS" >> "$LOG"
[ $STATUS -eq 0 ] || { echo "build failed, see $LOG"; exit $STATUS; }
APP_PATH=$DD_ROOT/dd-$CONFIG/Build/Products/Release-iphoneos/LoomTestApp.app
# Laid out as run-ios.js --products expects: <dir>/<config>/LoomTestApp.app.
mkdir -p "$DD_ROOT/products/$CONFIG"
rm -rf "$DD_ROOT/products/$CONFIG/LoomTestApp.app"
cp -R "$APP_PATH" "$DD_ROOT/products/$CONFIG/LoomTestApp.app"
echo "built $APP_PATH (copied to $DD_ROOT/products/$CONFIG)"
if [ -n "$INSTALL_DEVICE" ]; then
  xcrun devicectl device install app --device "$INSTALL_DEVICE" "$APP_PATH"
fi
