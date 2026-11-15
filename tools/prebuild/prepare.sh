#!/usr/bin/env bash
# Prepare the RN build tree: clean facebook/react-native at v<upstream> + Loom overrides + the build-only
# Package.swift patch. RN's release scripts (Gradle publish, iOS prebuild) only exist in the monorepo,
# not in the npm package.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/prebuild/casefs.sh"
WORK="${LOOM_PREBUILD_DIR:-$ROOT/.local/cs}"; loom_require_case_sensitive "$WORK"
SRC="$WORK/rn-src"
VERSION="$(node -p "require('$ROOT/packages/react-native-loom/overrides.json').upstream.version")"
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$WORK"
  git clone --depth 1 --branch "v$VERSION" https://github.com/facebook/react-native.git "$SRC"
fi
git -C "$SRC" checkout -q --force "v$VERSION"
git -C "$SRC" clean -fdq -e node_modules -e .build -e third-party -e .cxx -e build
git -C "$SRC" reset -q --hard "v$VERSION"
(cd "$SRC" && yarn install --frozen-lockfile)
node "$ROOT/packages/react-native-loom/bin/loom.js" overlay --monorepo "$SRC" --no-rn-tester
# SwiftPM prebuild: define Loom's macros from RCT_LOOM_*_IOS (Package.swift is not in the npm package).
git -C "$SRC" apply "$ROOT/tools/prebuild/package-swift-loom.patch"
