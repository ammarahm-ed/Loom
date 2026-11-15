# Sourced: refuse to build on a case-insensitive filesystem. RN builds on Linux; on macOS APFS/HFS+ the
# prefab include dirs React/ and react/ merge into one and the published AAR breaks on case-sensitive hosts.
# Create the volume once:
#   hdiutil create -size 70g -type SPARSE -fs "Case-sensitive APFS" -volname loomcs .local/cs-img/loomcs.sparseimage
#   hdiutil attach .local/cs-img/loomcs.sparseimage -mountpoint "$PWD/.local/cs" -nobrowse
loom_require_case_sensitive() {
  local d="$1" t; mkdir -p "$d"; t="$(mktemp -d "$d/.casetest.XXXXXX")"
  touch "$t/a" "$t/A"
  local n; n="$(ls "$t" | wc -l | tr -d ' ')"; rm -rf "$t"
  [ "$n" = 2 ] || { echo "error: $d is on a case-insensitive filesystem; mount the case-sensitive image (see tools/prebuild/casefs.sh)" >&2; exit 1; }
}
