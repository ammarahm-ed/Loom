# Supported libraries

Most libraries work on Loom unchanged, with no Loom-specific setup. Loom replaces Fabric's
shadow tree and mounting, not the interfaces libraries are written against:

- **Generated (codegen) components and view managers** mount through the same component views
  and view managers as on stock React Native.
- **Turbo Modules and Nitro modules** are untouched.
- **Fabric's C++ contracts** that libraries reach into are presented by Loom on its own tree:
  - a component whose descriptor carries its own logic (custom state, `adopt`,
    `measureContent`, a hand-written ShadowNode) gets a real ShadowNode, so its layout, state
    updates from native code and measuring behave as in Fabric;
  - `ShadowTree::commit` from a library is applied to Loom's tree;
  - `UIManager::getNewestCloneOfShadowNode` and `getRelativeLayoutMetrics` read Loom's tree;
  - React Native's shared animation backend writes animated props straight to views.

Each of these costs nothing until a library uses it: a plain generated component gets no
ShadowNode, and an app without animations pays nothing for the animation backend.

One library needs more than these contracts: Reanimated's layout animations and shared element
transitions. Loom builds a small adapter into the app for it, automatically. See
[Reanimated](/guide/reanimated).

## Verified libraries

Each library below has a self-checking screen in Loom's compatibility app
(`apps/loom-compat-app`) that exercises its native side (mounting, layout, measuring, commands,
state from native code, events) and compares the results with what the library should
produce. The screens run on iOS and Android devices, with JavaScript on the main thread and on
its own thread, on React Native 0.89.

| Library | Version | Notes |
|---|---|---|
| react-native-reanimated | 4.7.1 | Animations, layout animations, shared element transitions. See [Reanimated](/guide/reanimated) |
| react-native-worklets | 0.13.1 | |
| react-native-screens | 4.30.1 | With @react-navigation/native-stack: push, pop, header, modal, transparent modal |
| @react-navigation/native-stack | 7.21.0 | |
| @react-navigation/bottom-tabs | 7.21.0 | |
| react-native-safe-area-context | 5.11.1 | Insets and `SafeAreaView` layout from native state |
| react-native-gesture-handler | 3.5.0 | Gesture detectors mount and lay out; touch handling is checked with a separate touch-driven flow |
| @gorhom/bottom-sheet | 5.2.14 | Built on Reanimated and gesture-handler |
| react-native-keyboard-controller | 1.23.0 | |
| react-native-unistyles | 3.8.1 | Style updates applied from C++ to mounted views |
| @shopify/flash-list | 2.3.3 | |
| @legendapp/list | 3.7.2 | Including `scrollToIndex` |
| react-native-pager-view | 10.0.0 | Including `setPage` |
| @shopify/react-native-skia | 2.14.0 | |
| react-native-svg | 15.16.1 | Custom layout and measuring |
| lottie-react-native | 7.5.0 | |
| react-native-webview | 14.0.1 | |
| react-native-maps | 1.29.11 | |
| react-native-video | 6.19.3 | Playback needs network access on the device |
| react-native-vision-camera | 5.2.4 | Real camera preview (Nitro props with JS-only values) |
| @react-native-community/slider | 5.2.2 | Intrinsic size measured as on Fabric |
| @react-native-community/datetimepicker | 9.3.0 | |
| react-native-linear-gradient | 2.8.3 | |

A library that reaches into the renderer can also talk to Loom directly; see
[Supporting Loom in your library](/guide/supporting-loom).

A library missing from the list is likely to work if it uses only the interfaces above, which
is what most libraries do. If one does not, a minimal reproduction in the compatibility app is
the fastest way to get it fixed: Loom treats it as a Loom bug, since the goal is that libraries
work unchanged.

## React Native 0.89 and library releases

Loom targets React Native 0.89. Some library releases predate it and declare support only up
to 0.88 (Reanimated 4.7.x, for example). Where an older release fails to build or misbehaves on
0.89 itself, independently of Loom, Loom's integration applies the smallest build-level fix
that makes it work, without modifying `node_modules`.
