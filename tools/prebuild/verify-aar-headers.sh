#!/usr/bin/env bash
# Fail unless the prefab header path set of a Loom AAR matches RN's baseline AAR, compared
# CASE-SENSITIVELY (a case-insensitive build filesystem merges React/ and react/ include dirs).
# Missing baseline paths always fail. Extra paths must be Loom's own headers: LOOM_EXTRA_RE (default
# matches a "Loom" path/file name) or any path listed in $LOOM_EXTRA_ALLOW (a file, one path per line).
# Usage: verify-aar-headers.sh <ours.aar> <baseline.aar>
set -euo pipefail
export LC_ALL=C
ours="$1"; base="$2"
RE="${LOOM_EXTRA_RE:-[Ll]oom}"
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
list() { unzip -Z1 "$1" | grep -E '^prefab/modules/[^/]+/include/.+[^/]$' | sort; }
list "$ours" > "$t/ours"; list "$base" > "$t/base"
comm -13 "$t/ours" "$t/base" > "$t/missing"
comm -23 "$t/ours" "$t/base" > "$t/extra"
bad=0
if [ -s "$t/missing" ]; then echo "FAIL: $(wc -l < "$t/missing") baseline header paths missing (case-sensitive):"; head -20 "$t/missing"; bad=1; fi
# Loom's own headers: a name matching $RE, or a file that exists in Loom's overlay (packages/react-native-loom/src).
LOOMSRC="${LOOM_SRC:-$(cd "$(dirname "$0")/../.." && pwd)/packages/react-native-loom/src}"
find "$LOOMSRC" -name '*.h' -o -name '*.hpp' | sed -E 's#.*/(react|React)/#\1/#' | sort -u > "$t/overlay"
sed -E 's#^prefab/modules/[^/]+/include/##' "$t/extra" | paste -d'\t' - "$t/extra" > "$t/extra2"
awk -F'\t' 'NR==FNR{o[$0]=1;next} !($1 in o){print $2}' "$t/overlay" "$t/extra2" | grep -vE "$RE" > "$t/extra-unknown" || true
[ -n "${LOOM_EXTRA_ALLOW:-}" ] && [ -f "$LOOM_EXTRA_ALLOW" ] && { grep -vxFf "$LOOM_EXTRA_ALLOW" "$t/extra-unknown" > "$t/e2" || true; mv "$t/e2" "$t/extra-unknown"; }
if [ -s "$t/extra-unknown" ]; then echo "FAIL: $(wc -l < "$t/extra-unknown") extra header paths that are not Loom's:"; head -20 "$t/extra-unknown"; bad=1; fi
echo "baseline=$(wc -l < "$t/base") ours=$(wc -l < "$t/ours") loom-extra=$(( $(wc -l < "$t/extra") - $(wc -l < "$t/extra-unknown") ))"
[ $bad = 0 ] && echo "OK: header path sets match" || exit 1
