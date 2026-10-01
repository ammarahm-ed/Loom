# Plan: move Resha out of tree (react-native-windows model)

## Goal

Stop carrying the UI-thread / direct-renderer work as edits to a fork of
`facebook/react-native`. Ship it as a package, `react-native-resha`, that works on
top of the **unmodified `react-native` from npm** in an app's `node_modules`.
Taking an upstream fix should then be a version bump plus a mostly automatic
3-way merge, not a fork rebase.

Honest caveat: react-native-windows owns an entire platform, so most of its code
never touches RN. Resha changes RN core itself: Fabric scheduler, mounting,
threading and text. Patching core files can't be avoided entirely. This model
makes those patches **explicit, hash-tracked, measurable and 3-way mergeable**.
Phase 2 then shrinks them.

## Model

```
react-native-resha/                        (new repo, ~/Work/react-native-resha)
  packages/react-native-resha/
    overrides.json      upstream {package, version} + one entry per file:
                        {type: patch|platform|delete|test, file, baseHash, area}
    src/                full-file overrides, mirroring react-native package paths
    android-tests/      ReactAndroid/src/test/** (npm does not ship these)
    bin/resha.js        CLI
    lib/                manifest, materialize, validate, upgrade, import-fork, overlay, status/diff
    integration/        android/resha.settings.gradle, ios/resha_pods.rb, metro.js
  examples/rn-tester/rn-tester.patch        benchmark screens + NativeTextBenchmarkActivity
  docs/
```

- **patch**: a full copy of an upstream file with Resha edits. `baseHash` is the
  sha256 of the pristine npm file it was derived from.
- **platform**: a Resha-only file (the ~13k-line renderer core, UIThreadDirectViewApplier, ...).
- **delete**: an upstream file Resha removes. There are none today.
- **test**: an Android unit test. It is kept for CI and never materialized.

### Materialization (instead of patching node_modules in place)

`resha materialize` builds `node_modules/.resha/react-native`. That is a copy of
the pristine `node_modules/react-native` with the overrides applied, plus
`.resha-stamp.json`. The step is skipped when the stamp is current.
`node_modules/react-native` stays untouched: it is the merge base and the thing
`validate` hashes against.

It refuses to run if validation fails. Validation fails when:
- the installed RN version is not the one the overrides target;
- an upstream file's hash no longer matches its patch's `baseHash`;
- a platform file now also exists upstream;
- there are untracked files in `src/`.

An included Gradle build can't see the app's `gradle.properties`. So materialize
copies the switches `RN_UI_THREAD`, `UI_THREAD_RENDERER`,
`UI_THREAD_RENDERER_WORKER`, `RN_PROFILE_UI_THREAD_RENDERER` and
`RN_BUILD_CPP_TESTS` from `<app>/android/gradle.properties` into the
materialized `gradle.properties`. `-P` still wins.

### App integration

| Consumer | How |
|---|---|
| Android | `settings.gradle`: `apply from: .../integration/android/resha.settings.gradle`. It runs materialize, then `includeBuild(materialized)`, and substitutes `react-android`/`react-native` → `:packages:react-native:ReactAndroid` and `hermes-android`/`hermes-engine` → `:...:hermes-engine`. `app/build.gradle`: `react { reactNativeDir = file("../node_modules/.resha/react-native") }` |
| iOS | Podfile: `require_relative '.../integration/ios/resha_pods'`, then `require File.join(Resha.react_native_path(__dir__), 'scripts/react_native_pods')`, then `use_react_native!(:path => Resha.react_native_path(__dir__))`. Forces `RCT_USE_PREBUILT_RNCORE=0`. |
| Autolinking / CLI | `react-native.config.js`: `reactNativePath: './node_modules/.resha/react-native'` |
| Metro | `withResha(config, __dirname)` maps `react-native` and `react-native/*` to the materialized tree |
| Jest | `moduleNameMapper: reshaJestModuleNameMapper(__dirname)` |

Consequence: RN can no longer come from prebuilt binaries on either platform. It
always builds from source, which is already true for the fork today.

### CLI

- `materialize` / `validate`: used in apps and CI.
- `status`: overrides by area, with lines added and removed. This is the
  headline metric for phase 2.
- `diff [paths]`: patches vs pristine upstream.
- `add <file> [--platform] [--area]`: start overriding a file.
- `upgrade --to <ver>`: `npm pack` the old and new RN into
  `~/.cache/react-native-resha`, then run `git merge-file ours=src base=old theirs=new`
  on each patch and bump `baseHash`, the manifest version and the peerDependency.
  Conflicts are written as markers and reported; the exit code is 2 if any exist.
- `overlay --monorepo <dir>`: write overrides, Android tests and the rn-tester
  patch into a `facebook/react-native` checkout at the upstream tag. This is the
  dev loop in rn-tester.
- `import-fork --fork <dir> --base <sha>`: the reverse of overlay. It captures
  `packages/react-native` changes (working tree plus untracked files; `.bak` and
  `.rNN_` scratch files are skipped) and the rn-tester diff. It is idempotent.

## Status: done so far (uncommitted in ~/Work/react-native-resha)

- CLI and all lib modules are written. The integration helpers are written.
- Imported the fork (`~/Work/resha` working tree vs `a59eff64fa` = v0.87.1):
  **90 overrides** = 63 patch + 22 platform + 5 test.

  | area | files | +lines | -lines |
  |---|---|---|---|
  | renderer-core | 20 | 13262 | 0 |
  | mounting | 12 | 3209 | 33 |
  | delegate-plumbing | 11 | 1266 | 9 |
  | text | 14 | 503 | 22 |
  | threading | 9 | 270 | 33 |
  | build | 10 | 205 | 16 |
  | misc | 2 | 145 | 0 |
  | animation | 3 | 87 | 5 |
  | dom | 5 | 77 | 4 |
  | views | 4 | 60 | 1 |

- **Verified: zero behaviour change.** All 4,493 files in the materialized tree
  are byte-identical to the fork's `packages/react-native`. The only exceptions
  are the publish artifacts `types_generated/` and `README.md`, which don't exist
  in the fork checkout. Every changed fork file is present.
- The `overlay` → `import-fork` round trip on a clean v0.87.1 worktree is
  lossless.
- `rn-tester.patch` (20 files) applies cleanly to v0.87.1.
- Metro redirection and the Gradle switch forwarding were both smoke-tested.
- **Upgrade trial, 0.87.1 → 0.88.0-rc.3, run on a scratch copy:** 21 upstream
  files unchanged, 36 merged cleanly, 6 conflicts, 0 needing manual work.
  Conflicts:
  - FabricUIManagerBinding.kt
  - ReactInstance.kt
  - MainReactPackage.kt (2)
  - TextLayoutManager.kt
  - Scheduler.cpp
  - UIManagerDelegate.h

  So an upgrade is roughly "resolve 6 hunks". The fork would need a full rebase.

## Remaining work (phase 1: migration, no behaviour change)

1. Repo scaffolding:
   - root `package.json` (workspaces), `.gitignore` (`node_modules`, `.resha`)
   - LICENSE, README with the integration snippets above
   - `.gitkeep`-free tree
2. Drop local-environment cruft from the overrides: the hardcoded
   `ndkVersion = "27.2.12479018"` in ReactAndroid and hermes-engine
   `build.gradle.kts`. Take it from `rootProject.ext` or leave RN's default.
   Also leave the fork's `.gitignore` `.commandcode/` entry out.
3. Test app `apps/resha-test-app` (RN 0.87.1 template) wired through the
   integrations. Build Android and iOS with each switch combination:
   - off
   - `RN_UI_THREAD`
   - `+UI_THREAD_RENDERER`
   - `+UI_THREAD_RENDERER_WORKER`
4. Confirm autolinking and codegen see the materialized tree: `reactNativePath`,
   `reactNativeDir`, and that `@react-native/codegen` resolves from `.resha/`
   (Node lookup walks up to `<app>/node_modules`).
5. CI:
   - `resha validate`, which also catches unregistered files;
   - Android: `overlay` into a v0.87.1 checkout, then run the `ReactAndroid`
     unit tests (including `android-tests/`) and the C++ tests
     (`RN_BUILD_CPP_TESTS`: UIThreadRendererTest, UIThreadRendererWorkerTest);
   - the test app builds for both platforms;
   - a nightly job running `resha upgrade --to <latest rc>` on a throwaway
     branch, which reports the conflict count early.
6. Cut over:
   - freeze the fork;
   - run a final `import-fork`;
   - commit;
   - publish `react-native-resha@0.87.1-resha.0` (versioning:
     `<rn-version>-resha.<n>`, peerDependency pinned to the exact RN version);
   - switch consumer apps over;
   - archive the fork.
7. Upgrade runbook in `docs/upgrading.md`:
   - `resha upgrade --to X`
   - resolve markers
   - `resha validate`
   - `overlay` + rn-tester smoke test
   - commit
   - publish `X-resha.0`

## Phase 2: shrink the patch surface (tracked by `resha status`)

Target: only renderer-core and a few small hook patches remain.

1. **renderer-core → own build unit.** Move `UIThreadRenderer*` and
   `UIThreadEventManager` out of `ReactCommon/react/renderer/uimanager/` into
   `react-native-resha/cpp/`, with its own CMake target and podspec
   (`React-ReshaRenderer`) linked into ReactAndroid / React-Fabric. These become
   package sources instead of 20 platform overrides. The patches remaining are
   only the CMakeLists and podspec wiring.
2. **delegate-plumbing (11 files, +1266).** Replace the ~20 `uiThreadRenderer*`
   virtuals threaded through the four layers (UIManagerDelegate → Scheduler →
   SchedulerDelegate → RCTScheduler/FabricUIManagerBinding) with one
   `UIThreadRendererHost` interface. Register it in `ContextContainer` and fetch
   it where needed. Spike needed on whether mount-time calls can bypass Scheduler.
3. **Binding install.** Replace the `#if UI_THREAD_RENDERER` swap in
   `UIManagerBinding.cpp` with a factory hook (spike). Per the attach-architecture
   rule, NativeScript keeps attaching as a core runtime binding via
   `BindingsInstallFunc`, never through a TurboModule.
4. **dom.** Register `UIThreadNativeDOM` via the app's cxx TurboModule provider
   instead of patching `NativeDOM.cpp` / `DefaultTurboModules.cpp` (spike).
5. **Upstream PRs to facebook/react-native**, each of which deletes patches:
   - the RuntimeScheduler_Modern re-entrancy / deadlock fix (sync call from the
     runtime's own thread);
   - a queue-config hook on ReactHostDelegate (removes the `ReactInstance.kt`
     `mainThreadSpec` patch);
   - the ReactFontManager thread-safety fix;
   - the `RegistryMutex` typedef in ComponentDescriptorRegistry;
   - `RCTMessageThread` / `RCTJSThreadManager` run-on-main support.
6. **mounting / text / views.** Move Android pieces behind existing extension
   points where possible: ViewManager / MainReactPackage registration and a custom
   `MountingManager` delegate. Whatever remains stays as tagged patches.

Re-run `resha status` after each step. Also re-measure the conflict count of the
nightly upgrade job; it is the real maintenance-cost metric.
