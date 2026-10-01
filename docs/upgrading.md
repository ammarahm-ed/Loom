# Upgrading react-native-resha

Resha carries React Native overrides, so "take an upstream fix" means rebasing
those overrides onto a new `react-native` version — plus the Android tests and
the rn-tester benchmark patch, which npm does not ship.

The goal is that this is a version bump, a bounded set of conflict hunks, and a
verification pass — not a fork rebase.

## Versioning

`react-native-resha` versions are `<react-native-version>-resha.<n>`, and the
`peerDependencies.react-native` range is pinned to the exact version:

| react-native-resha | react-native |
|---|---|
| `0.87.1-resha.0` | `0.87.1` |
| `0.88.0-resha.0` | `0.88.0` |
| `0.88.0-resha.1` | `0.88.0` (second Resha revision, same RN) |

`resha upgrade --to <version>` bumps `overrides.json > upstream.version` and the
package's `peerDependencies` / `devDependencies`, but never the `-resha.<n>`
suffix — that is a release decision you make when publishing.

## 1. Run the upgrade

```sh
cd <react-native-resha repo>
node packages/react-native-resha/bin/resha.js upgrade --to 0.88.0
```

What it does, per override entry:

| entry | what happens |
|---|---|
| `patch` | `git merge-file src/<file> <old upstream> <new upstream>`; clean merges are written back and `baseHash` is bumped |
| `patch`, upstream file unchanged | nothing to do |
| `platform` | only checked: if the file now exists upstream it is reported for manual handling |
| `delete` | `baseHash` bumped when the upstream file changed |
| `test` | **skipped** — see step 2.5 |
| upstream file removed or moved | reported as "needs manual work" |

It prints `unchanged upstream / merged cleanly / conflicts / needs manual work`.
**The exit code is 2 when there is at least one conflict**, so CI can tell the
difference between "merged" and "needs a human".

For reference, the 0.87.1 → 0.88.0-rc.3 trial produced: 21 upstream files
unchanged, 36 merged cleanly, 6 conflicts, 0 needing manual work. Conflicts were
`FabricUIManagerBinding.kt`, `ReactInstance.kt`, `MainReactPackage.kt` (2),
`TextLayoutManager.kt`, `Scheduler.cpp`, `UIManagerDelegate.h`.

### Offline / air-gapped upgrades

`upgrade` always fetches pristine `react-native` tarballs rather than trusting the
working tree. They land in `~/.cache/react-native-resha/react-native-<version>/package`,
or in `$RESHA_CACHE_DIR` when that is set. To pre-populate the cache by hand:

```sh
dest="${RESHA_CACHE_DIR:-$HOME/.cache/react-native-resha}/react-native-0.88.0"
mkdir -p "$dest" && npm pack react-native@0.88.0 --pack-destination "$dest"
tar -xzf "$dest"/react-native-0.88.0.tgz -C "$dest" && rm "$dest"/react-native-0.88.0.tgz
```

## 2. Resolve conflicts

Conflicts are written **into `src/`** as ordinary git conflict markers:

```
<<<<<<< resha
...Resha's version...
=======
...upstream's version...
>>>>>>> react-native@0.88.0
```

Resolve them in place, then continue. Rules of thumb that keep phase 2's
"shrink the patch surface" goal intact:

- Prefer deleting Resha code that upstream has now implemented.
- Prefer upstream's structure, and re-apply the smallest Resha change on top.
- If a patch is now empty, delete it: remove the entry from `overrides.json` and
  the file from `src/` (that is what makes `resha status` shrink). A `patch` whose
  content equals upstream is redundant — `resha diff <file>` should show nothing.

Check every conflict is gone:

```sh
grep -rn '<<<<<<<\|>>>>>>>' packages/react-native-resha/src/ || echo "no markers"
```

### 2.5 Android tests

`android-tests/ReactAndroid/src/test/**` is not published to npm, so `upgrade`
cannot 3-way merge it — and it is never materialized. After an upgrade, rebase it
against the new upstream test sources:

1. get a `facebook/react-native` checkout at the new tag (step 4 does this for you
   with `resha overlay`);
2. diff Resha's test files against their upstream counterparts there and
   re-apply the Resha-specific assertions;
3. run them (step 4) — they fail loudly if upstream's test scaffolding changed.

## 3. Validate

```sh
node packages/react-native-resha/bin/resha.js validate
```

Validation fails when: the installed `react-native` version is not the one the
overrides target, an upstream file no longer hashes to its `baseHash`, a
`platform` file now exists upstream, or `src/` contains a file that is not listed
in `overrides.json`. Expect `89 overrides valid`.

Then re-measure what we carry — this is the headline metric for phase 2:

```sh
node packages/react-native-resha/bin/resha.js status
```

## 4. Verify against real apps

Materialize and build the test app with every switch combination:

```sh
cd apps/resha-test-app
npm install
npm run resha:validate

# Android: off, RN_UI_THREAD, +UI_THREAD_RENDERER, +UI_THREAD_RENDERER_WORKER
npm run android:assemble -- -PreactNativeArchitectures=arm64-v8a
(cd android && ./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a -PRN_UI_THREAD=true)
(cd android && ./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true)
(cd android && ./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true)

# iOS: off, renderer, renderer + worker
(cd ios && bundle install && bundle exec pod install)
(cd ios && xcodebuild -workspace ReshaTestApp.xcworkspace -scheme ReshaTestApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' build)
(cd ios && RCT_UI_THREAD_RENDERER_IOS=1 xcodebuild -workspace ReshaTestApp.xcworkspace -scheme ReshaTestApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' build)
(cd ios && RCT_UI_THREAD_RENDERER_IOS=1 RCT_UI_THREAD_RENDERER_WORKER_IOS=1 xcodebuild -workspace ReshaTestApp.xcworkspace -scheme ReshaTestApp -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' build)
```

See `apps/resha-test-app/README.md` for what each switch means and how to confirm
a switch reached the build.

Then run the unit and C++ tests from an upstream checkout:

```sh
node packages/react-native-resha/bin/resha.js overlay --monorepo ~/Work/react-native

# The monorepo has a single gradlew at its root, and the project path carries the
# prefix. The JVM unit tests now include the files from android-tests/:
(cd ~/Work/react-native && \
  ./gradlew :packages:react-native:ReactAndroid:testDebugUnitTest \
    -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true)

# The C++ suites need the tests enabled. All three switches are required: the
# publication tests use the worker API, and the CMake test block rejects a
# worker-less build. RN_BUILD_CPP_TESTS=true attaches the test target to
# `reactnative`, so this build produces it along with the native libraries:
(cd ~/Work/react-native && \
  ./gradlew :packages:react-native:ReactAndroid:assembleDebug \
    -PreactNativeArchitectures=arm64-v8a \
    -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true \
    -PUI_THREAD_RENDERER_WORKER=true -PRN_BUILD_CPP_TESTS=true)

RA=~/Work/react-native/packages/react-native/ReactAndroid
exe=$(find "$RA/build/intermediates/cxx" -type f -name ui_thread_renderer_tests | head -1)
```

The suites are Android binaries, so run them on a device or emulator. AGP stages
the executable and its native dependencies (`libjsi.so`, `libfbjni.so`) under
`build/intermediates/cxx/<buildType>/<hash>/obj/<abi>/`, not next to `build.ninja`;
`libc++_shared.so` comes from the NDK:

```sh
libs=$(dirname "$exe")
adb push "$exe" /data/local/tmp/ui_thread_renderer_tests
adb push "$libs/libjsi.so" "$libs/libfbjni.so" /data/local/tmp/
find "$ANDROID_HOME/ndk" -name 'libc++_shared.so' -path '*arm64-v8a*' -exec adb push {} /data/local/tmp/ \;
adb shell "cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./ui_thread_renderer_tests"
```

Expect `85 tests from 3 test suites ran` and `[  PASSED  ] 85 tests` (68 from
`UIThreadRendererTest`, 7 from `UIThreadRendererPublishTest`, 10 from
`UIThreadRendererWorkerTest`).

and smoke-test the benchmarks in `packages/rn-tester` (`NativeTextBenchmarkActivity`
and the direct-renderer screens) from that same checkout.

## 5. Commit and publish

```sh
git add -A
git commit -m "chore: upgrade react-native to 0.88.0"
```

Then set the Resha revision, publish, and move consumers:

```sh
# packages/react-native-resha/package.json -> "version": "0.88.0-resha.0"
cd packages/react-native-resha && npm publish
```

In consumer apps: bump `react-native@0.88.0` and `react-native-resha@0.88.0-resha.0`
together, `rm -rf node_modules/.resha`, and rebuild.

## Nightly conflict report

`.github/workflows/nightly-upgrade.yml` runs the upgrade against the newest
release candidate on a throwaway branch and reports the conflict count. That
number — not the raw line count in `resha status` — is the real maintenance cost,
so watch its trend.

## Appendix: how the merge is computed

For each `patch` entry the three inputs are:

| input | source |
|---|---|
| ours | `packages/react-native-resha/src/<file>` (Resha's full file) |
| base | pristine `react-native@<old>` from the npm cache |
| theirs | pristine `react-native@<new>` from the npm cache |

`node_modules/react-native` in an app is never part of the merge: it stays the
pristine install that `resha validate` hashes against.

## Appendix: keeping up while the fork still exists

Until the fork is archived, `import-fork` is the bridge in the other direction:

```sh
node packages/react-native-resha/bin/resha.js import-fork --fork ~/Work/resha --base a59eff64fa --version 0.87.1
```

It captures `packages/react-native` (tracked changes plus untracked files, minus
`.bak` / `.rNN_` scratch files) and the rn-tester diff, and it is idempotent.

**Caveat:** the fork's working tree carries local-environment edits that must not
become overrides — a hardcoded `ndkVersion = "27.2.12479018"` in
`ReactAndroid/build.gradle.kts` (and formerly in
`ReactAndroid/hermes-engine/build.gradle.kts`) and a `.commandcode/` line in its
root `.gitignore`. A fresh `import-fork` reintroduces them, because it copies
whatever differs from upstream. After any import, re-drop them:

```sh
node packages/react-native-resha/bin/resha.js diff build.gradle.kts   # should show no ndkVersion hardcode
```

`ReactAndroid/build.gradle.kts` must keep upstream's NDK logic:

```kotlin
if (rootProject.hasProperty("ndkVersion") && rootProject.properties["ndkVersion"] != null) {
  ndkVersion = rootProject.properties["ndkVersion"].toString()
} else {
  ndkVersion = libs.versions.ndkVersion.get()
}
```