---
layout: home

hero:
  name: Loom
  text: A faster renderer for React Native
  tagline: Loom replaces Fabric with a leaner renderer that weaves React straight into native views. Instant updates, your libraries unchanged, and JavaScript on the thread you choose.
  image:
    src: /logo.svg
    alt: Loom
  actions:
    - theme: brand
      text: Get started
      link: /guide/getting-started
    - theme: alt
      text: What is Loom?
      link: /guide/what-is-loom

features:
  - title: Updates in the same frame
    details: With JavaScript on the main thread, a tap reaches your handler during Android's input handling and the result is drawn in the frame a native app would draw it in. A setState is as fast as setNativeProps.
  - title: Your libraries, unchanged
    details: Loom presents Fabric's C++ contracts on its own tree, so libraries work without changes, Reanimated's layout animations and shared element transitions included.
    link: /guide/compatibility
  - title: Your choice of thread
    details: Run JavaScript on the main thread like a native app, or on its own thread so heavy JavaScript never blocks a frame. One setting, one build.
  - title: Faster startup
    details: Ships a baseline profile for React Native's own code, cutting time to first content by about 70 ms on a mid-range Android phone.
  - title: Debug-time jank reports
    details: Debug builds tell you when something blocks the main thread, what it was, and where, with sampled JavaScript stacks mapped to your source.
  - title: Out of tree
    details: Loom layers hash-tracked overrides on the unmodified react-native from npm. Upgrading React Native is a version bump and a 3-way merge, not a fork rebase.
---
