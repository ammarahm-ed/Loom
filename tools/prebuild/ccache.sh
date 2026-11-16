#!/usr/bin/env bash
# Source from android.sh / ios.sh: compiles through ccache when it is installed (LOOM_CCACHE=0 turns it off).
#
# Every prebuild compiles each flavor twice (pass 1, then the final build with export stubs), and the two
# passes differ in a handful of files, so even a cold cache saves most of the second pass; CI also restores
# the cache across runs.
#
#   Android: the NDK's CMake toolchain uses $NDK_CCACHE as the compiler launcher.
#   iOS:     React Native's prebuild drives xcodebuild itself, so CC/CXX come from an xcconfig that
#            xcodebuild applies to every build (XCODE_XCCONFIG_FILE), pointing at ccache wrappers - the same
#            setup React Native's own CocoaPods ccache support uses.
loom_ccache_setup() {
  [ "${LOOM_CCACHE:-1}" = 1 ] || return 0
  local ccache; ccache="$(command -v ccache || true)"
  [ -n "$ccache" ] || { echo "ccache not installed; compiling without it"; return 0; }
  # Paths differ between machines and runs: hash relative to the work tree, by compiler content.
  export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$1}"
  export CCACHE_NOHASHDIR=1
  export CCACHE_COMPILERCHECK=content
  # React Native's ccache.conf (scripts/xcode/ccache.conf): clang modules, PCHs and index stores.
  export CCACHE_SLOPPINESS=clang_index_store,file_stat_matches,include_file_ctime,include_file_mtime,ivfsoverlay,pch_defines,modules,system_headers,time_macros
  export CCACHE_DEPEND=1
  export CCACHE_FILECLONE=1
  if [ "$(uname)" = Darwin ]; then
    local dir="${CCACHE_WRAPPER_DIR:-$1/.ccache-wrappers}"
    mkdir -p "$dir"
    printf '#!/bin/sh\nexec "%s" "$(xcrun -f clang)" "$@"\n' "$ccache" > "$dir/clang"
    printf '#!/bin/sh\nexec "%s" "$(xcrun -f clang++)" "$@"\n' "$ccache" > "$dir/clang++"
    printf '#!/bin/sh\nexec "$(xcrun -f clang)" "$@"\n' > "$dir/ld"
    printf '#!/bin/sh\nexec "$(xcrun -f clang++)" "$@"\n' > "$dir/ldplusplus"
    chmod +x "$dir"/*
    printf 'CC = %s\nCXX = %s\nLD = %s\nLDPLUSPLUS = %s\n' \
      "$dir/clang" "$dir/clang++" "$dir/ld" "$dir/ldplusplus" > "$dir/ccache.xcconfig"
    export XCODE_XCCONFIG_FILE="$dir/ccache.xcconfig"
  else
    export NDK_CCACHE="$ccache"
  fi
  ccache --zero-stats >/dev/null
  echo "ccache: $ccache (dir ${CCACHE_DIR:-default}, max $(ccache --get-config max_size))"
}
