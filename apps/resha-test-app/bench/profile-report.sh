#!/bin/bash
# Summarizes a bench/profile.sh recording: CPU time per thread, then the heaviest libraries and
# functions (self time) on the threads that matter, then inclusive time for named subsystems.
#
#   bench/profile-report.sh <scenario> <variant> [top N]
set -e
SCENARIO=$1
V=$2
TOP=${3:-25}
APP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$APP/bench/out"
NDK=$(ls -d ~/Library/Android/sdk/ndk/* | sort -V | tail -1)
SP=$(ls "$NDK"/simpleperf/bin/darwin/*/simpleperf | head -1)
DATA="$OUT/prof-$SCENARIO-$V.data"
SYMFS="$OUT/binary_cache-$V"

echo "== $SCENARIO / $V: samples per thread"
"$SP" report -i "$DATA" --symfs "$SYMFS" --sort comm 2>/dev/null | sed -n '/^Overhead/,$p' | head -15

echo
echo "== self time by library (all app threads)"
"$SP" report -i "$DATA" --symfs "$SYMFS" --sort dso 2>/dev/null | sed -n '/^Overhead/,$p' | head -15

echo
echo "== top $TOP functions by self time (all app threads)"
"$SP" report -i "$DATA" --symfs "$SYMFS" --sort dso,symbol 2>/dev/null | sed -n '/^Overhead/,$p' \
  | head -$((TOP + 1)) | cut -c1-200
