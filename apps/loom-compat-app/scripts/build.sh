#!/bin/bash
# Build and install the compat app for one platform and one variant.
#   Usage: scripts/build.sh <ios|android> <main|bg|stock> [run]
#   main   Loom renderer, JavaScript on the main thread (Android: loom.js=main; iOS: loom.js=main)
#   bg     Loom renderer, JavaScript on its own thread (loom.js=background; iOS: loom.js=background)
#   stock  stock React Native, no Loom (LOOM_BENCH_STOCK=1)
# Without "run" the script only builds. Pod install (iOS) is rerun whenever the variant changes,
# recorded in ios/.variant. Android passes the variant as -P properties, which win over gradle.properties.
set -euo pipefail
PLATFORM=${1:?platform: ios|android}
VARIANT=${2:?variant: main|bg|stock}
RUN=${3:-}
APP="$(cd "$(dirname "$0")/.." && pwd)"
cd "$APP"

case "$VARIANT" in
  main)  export LOOM_BENCH_STOCK=0; RCT_JS=main;       LOOM_PROPS="-Ploom.renderer=true -Ploom.js=main -Ploom.compileFromSource=true" ;;
  bg)    export LOOM_BENCH_STOCK=0; RCT_JS=background; LOOM_PROPS="-Ploom.renderer=true -Ploom.js=background -Ploom.compileFromSource=true" ;;
  stock) export LOOM_BENCH_STOCK=1; RCT_JS=none;       LOOM_PROPS="" ;;
  *) echo "unknown variant $VARIANT" >&2; exit 1 ;;
esac

if [ "$PLATFORM" = ios ]; then
  cd ios
  WANT="$VARIANT"
  if [ "$(cat .variant 2>/dev/null || true)" != "$WANT" ] || [ ! -d Pods ]; then
    if [ "$VARIANT" = stock ]; then
      bundle exec pod install
    else
      env loom.renderer=true loom.js=$RCT_JS loom.compileFromSource=true bundle exec pod install
    fi
    echo "$WANT" > .variant
  fi
  if [ -n "${IOS_DEVICE:-}" ]; then
    # A real iPhone: signed with team NRMH46S5JU (automatic signing), bundle id under that team.
    xcodebuild -workspace LoomCompatApp.xcworkspace -scheme LoomCompatApp -configuration Debug \
      -destination "id=$IOS_DEVICE" -derivedDataPath build/device -quiet -allowProvisioningUpdates \
      DEVELOPMENT_TEAM=NRMH46S5JU CODE_SIGN_STYLE=Automatic PRODUCT_BUNDLE_IDENTIFIER=com.loomtestapp.compat build
    APP_PATH=build/device/Build/Products/Debug-iphoneos/LoomCompatApp.app
    echo "BUILT ios $VARIANT -> $APP_PATH on device $IOS_DEVICE"
    if [ "$RUN" = run ]; then
      xcrun devicectl device install app --device "$IOS_DEVICE" "$APP_PATH"
    fi
  else
    DEST=$(xcrun simctl list devices booted | grep -m1 -oE '[0-9A-F-]{36}' || true)
    [ -n "$DEST" ] || { echo "no booted iOS simulator" >&2; exit 1; }
    xcodebuild -workspace LoomCompatApp.xcworkspace -scheme LoomCompatApp -configuration Debug \
      -destination "id=$DEST" -derivedDataPath build/derived -quiet build
    APP_PATH=$(find build/derived/Build/Products/Debug-iphonesimulator -maxdepth 1 -name 'LoomCompatApp.app' | head -1)
    BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP_PATH/Info.plist")
    echo "BUILT ios $VARIANT -> $APP_PATH ($BUNDLE_ID) on $DEST"
    if [ "$RUN" = run ]; then
      xcrun simctl install "$DEST" "$APP_PATH"
    fi
  fi
else
  cd android
  DEV=${ANDROID_SERIAL:-emulator-5554}
  # shellcheck disable=SC2086
  ./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a --console=plain $LOOM_PROPS
  if [ "$RUN" = run ]; then
    adb -s "$DEV" install -r app/build/outputs/apk/debug/app-debug.apk
    # Environment: this emulator image enforces ACCESS_LOCAL_NETWORK; without the grant the app
    # cannot reach Metro at 10.0.2.2:8081. Reinstalls reset it, so grant after every install.
    adb -s "$DEV" shell pm grant com.loomcompatapp android.permission.ACCESS_LOCAL_NETWORK 2>/dev/null || true
  fi
  echo "BUILT android $VARIANT"
fi
