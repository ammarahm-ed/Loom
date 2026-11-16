#!/usr/bin/env bash
# Fail unless, for every ABI, the dynamic symbols exported by our libreactnative.so (and libjsi.so,
# libhermestooling.so) are a SUPERSET of those exported by RN's own AAR of the same flavor.
# Nothing RN exports may disappear: libraries link against these names. Cut code becomes a stub, never a removal.
# Usage: verify-aar-exports.sh <ours.aar> <baseline.aar>
set -euo pipefail
export LC_ALL=C
# ANDROID_NDK is set by android.sh from React Native's pinned ndkVersion.
: "${ANDROID_NDK:?set ANDROID_NDK (android.sh does)}"
NM="$(ls "$ANDROID_NDK"/toolchains/llvm/prebuilt/*/bin/llvm-nm | head -1)"
ours="$1"; base="$2"; t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
# Compiler instantiations are not API: whether such a symbol is emitted at all depends on inlining, so
# they may differ between two builds of the same source. ONLY these two families are ignored:
#   typeinfo for libc++ std::__ndk1::__shared_ptr_emplace<...>   (_ZTINSt6__ndk120__shared_ptr_emplace...)
#   fbjni jmethod_traits<...>::kDescriptor static members          (_ZN8facebook3jni14jmethod_traits...kDescriptorE)
ARTIFACTS='^(_ZTINSt6__ndk120__shared_ptr_emplace|_ZN8facebook3jni14jmethod_traits.*11kDescriptorE$)'

mkdir "$t/o" "$t/b"; bad=0
unzip -q "$ours" 'jni/*' -d "$t/o"; unzip -q "$base" 'jni/*' -d "$t/b"
for abi in $(ls "$t/b/jni"); do
  for lib in libreactnative.so libjsi.so libhermestooling.so; do
    [ -f "$t/b/jni/$abi/$lib" ] || continue
    [ -f "$t/o/jni/$abi/$lib" ] || { echo "FAIL: $abi/$lib missing"; bad=1; continue; }
    "$NM" -D --defined-only "$t/b/jni/$abi/$lib" | awk '{print $NF}' | sort -u > "$t/b.sym"
    "$NM" -D --defined-only "$t/o/jni/$abi/$lib" | awk '{print $NF}' | sort -u > "$t/o.sym"
    comm -13 "$t/o.sym" "$t/b.sym" | grep -vE "$ARTIFACTS" > "$t/miss" || true
    missing=$(wc -l < "$t/miss" | tr -d ' ')
    if [ "$missing" != 0 ]; then echo "FAIL: $abi/$lib lacks $missing of $(wc -l < "$t/b.sym") RN exports:"; head -10 "$t/miss"; bad=1
    else echo "ok   $abi/$lib exports $(wc -l < "$t/o.sym") >= RN's $(wc -l < "$t/b.sym")"; fi
  done
done
exit $bad
