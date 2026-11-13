# Instant updates

In main-thread mode, a tap reaches your JavaScript while Android is still handling the touch,
and React's commit is applied before that frame is drawn. A state update shows up in the same
frame a native app would draw it in.

What that means in practice:

- No need for `setNativeProps` or animated values just to make a tap feel instant: a plain
  `setState` costs the same.
- `requestAnimationFrame` callbacks run before the frame is drawn, so per-frame updates land in
  that frame.

What is still bounded by the hardware: the touch has to reach the app (a few milliseconds) and
the next screen refresh has to come (up to one frame: 16.7 ms at 60 Hz, 8.3 ms at 120 Hz).

::: tip Keep handlers small
A handler that runs for 40 ms delays that frame by 40 ms. Large renders belong in
`startTransition`; see [Large updates and transitions](/guide/transitions).
:::
