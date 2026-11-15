#!/usr/bin/env bash
# iOS equivalent of verify-aar-exports.sh: for every slice of React.xcframework, the global symbols our
# React binary defines must be a SUPERSET of those in React Native's own core tarball of the same flavor.
# Compiler instantiations are not API: React Native's WEAK definitions (template/inline instantiations; every
# image that uses one has its own copy) are not required, only its strong exports.
# Usage: verify-ios-exports.sh <ours.tar.gz> <baseline.tar.gz>
set -euo pipefail
export LC_ALL=C
ours="$1"; base="$2"; t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
mkdir "$t/o" "$t/b"; tar xzf "$ours" -C "$t/o" React.xcframework; tar xzf "$base" -C "$t/b" React.xcframework
# Build-system and compiler artifacts, not API (ONLY these families are ignored):
#   libc++ template instantiations (std::__1::...)                      _ZNSt3__1... _ZNKSt3__1... _ZTINSt3__1... _ZTSNSt3__1... _ZTVNSt3__1...
#   SwiftPM's generated resource-bundle finder                          *SWIFTPM_MODULE_BUNDLE*
ARTIFACTS='^_?_Z(N|NK|TIN|TSN|TVN)St3__1|SWIFTPM_MODULE_BUNDLE'
bad=0
for slice in "$t"/b/React.xcframework/*/; do
  s="$(basename "$slice")"
  [ -d "$slice/React.framework" ] || continue
  bin_b="$slice/React.framework/React"; bin_o="$t/o/React.xcframework/$s/React.framework/React"
  [ -f "$bin_o" ] || { echo "FAIL: slice $s missing"; bad=1; continue; }
  for arch in $(lipo -archs "$bin_b"); do
    nm -gUm -arch "$arch" "$bin_b" | grep -v ' weak ' | awk '{print $NF}' | sort -u > "$t/b.sym"; nm -gjU -arch "$arch" "$bin_o" | sort -u > "$t/o.sym"
    comm -13 "$t/o.sym" "$t/b.sym" | grep -vE "$ARTIFACTS" > "$t/miss" || true
    if [ -s "$t/miss" ]; then echo "FAIL: $s/$arch lacks $(wc -l < "$t/miss" | tr -d ' ') of $(wc -l < "$t/b.sym" | tr -d ' ') RN exports:"; head -10 "$t/miss"; bad=1
    else echo "ok   $s/$arch exports $(wc -l < "$t/o.sym" | tr -d ' ') >= RN's $(wc -l < "$t/b.sym" | tr -d ' ')"; fi
  done
done
exit $bad
