#!/usr/bin/env bash
# Publish the iOS prebuilt cores built by ios.sh to the LOCAL Maven repo (never remote), in React Native's
# react-native-artifacts layout under Loom's group:
#   <repo>/io/github/ammarahm-ed/react-native-artifacts/<version>/react-native-artifacts-<version>-<classifier>.tar.gz
# classifiers (scripts/cocoapods/rncore.rb reads them):
#   reactnative-core-{debug,release,release-profiling}
#   reactnative-core-dSYM-{debug,release,release-profiling}
#   reactnative-headers-{debug,release,release-profiling}
# Usage: ios-publish.sh   (one build serves both JavaScript-thread modes)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${LOOM_PREBUILD_DIR:-$ROOT/.local/cs}"
IN="${LOOM_IOS_OUT:-$WORK/ios-out}"
REPO="${LOOM_MAVEN_LOCAL:-$ROOT/.local/maven-local}"
VER="${LOOM_PUBLISH_VERSION:-$(node -p "require('$ROOT/packages/react-native-loom/overrides.json').upstream.version")}"
DIR="$REPO/io/github/ammarahm-ed/react-native-artifacts/$VER"
mkdir -p "$DIR"
for pair in Debug:debug Release:release ReleaseProfiling:release-profiling; do
  n="${pair%%:*}"; c="${pair##*:}"
  [ -f "$IN/ReactCore$n.xcframework.tar.gz" ] || continue
  cp "$IN/ReactCore$n.xcframework.tar.gz" "$DIR/react-native-artifacts-$VER-reactnative-core-$c.tar.gz"
  cp "$IN/ReactCore$n.framework.dSYM.tar.gz" "$DIR/react-native-artifacts-$VER-reactnative-core-dSYM-$c.tar.gz"
  cp "$IN/ReactNativeHeaders$n.xcframework.tar.gz" "$DIR/react-native-artifacts-$VER-reactnative-headers-$c.tar.gz"
done
cat > "$DIR/react-native-artifacts-$VER.pom" <<POM
<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion>
  <groupId>io.github.ammarahm-ed</groupId><artifactId>react-native-artifacts</artifactId><version>$VER</version>
  <packaging>pom</packaging>
  <name>Loom</name>
  <description>Loom: a faster renderer for React Native. Prebuilt React Native core for iOS with the Loom renderer.</description>
  <url>https://github.com/ammarahm-ed/Loom</url>
  <licenses><license><name>MIT License</name><url>https://github.com/ammarahm-ed/Loom/blob/main/LICENSE</url></license></licenses>
  <developers><developer><id>ammarahm-ed</id><name>Ammar Ahmed</name></developer></developers>
  <scm>
    <url>https://github.com/ammarahm-ed/Loom</url>
    <connection>scm:git:https://github.com/ammarahm-ed/Loom.git</connection>
    <developerConnection>scm:git:git@github.com:ammarahm-ed/Loom.git</developerConnection>
  </scm>
</project>
POM
ls -l "$DIR"
