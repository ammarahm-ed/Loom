# Asynchronous text on iOS

Drawing text is one of the most expensive things the main thread does on iOS: React Native lays
out and rasterizes every paragraph in `drawRect:` on the main thread, every time its text changes.
Loom moves that work to a background queue in the common case, and keeps it on the main thread
where moving it would show.

## What draws where

| A paragraph… | Drawn | What you see |
|---|---|---|
| changes its text but keeps its size (a label, a counter, a row's content) | in the background | the new text, normally a frame later |
| appears for the first time | in the background | the text, normally a frame after the view |
| changes size (new text that is longer or wraps differently, LayoutAnimation, resizing) | on the main thread, in the same frame | exactly what React Native draws |
| is selectable, has a pressed link, or contains inline views | on the main thread | exactly what React Native draws |
| is redrawn by UIKit (trait or display-scale change) | on the main thread | exactly what React Native draws |

Transform and opacity animations never redraw text, so they are unaffected.

The previous drawing stays on screen until the new one is ready, and it is only kept while the
view still has the size it was drawn for, so text is never shown stretched, clipped or out of
place. Background drawings use an 8-bit bitmap unless the text uses a color outside sRGB
(Display P3), matching what UIKit's own backing stores do.

## Cost

On an iPhone XS Max, 300 labels getting new text every frame (JavaScript on the main thread,
same build, switch flipped):

| | Per update | Main thread | Total CPU |
|---|---|---|---|
| Synchronous (React Native's way) | 26.6 ms | 7.1 s | 12.7 s |
| Asynchronous | 16.9 ms | 3.8 s | 8.7 s |

See [Benchmarks](/benchmarks) for the full set.

## Switching it off

Asynchronous text is on with the Loom renderer. Two switches, read once at launch from
`NSUserDefaults` (so they also work as launch arguments):

| Key | Default | Effect |
|---|---|---|
| `LoomAsyncText` | `YES` | `NO` draws all text synchronously, as React Native does |
| `LoomAsyncTextFirstDraw` | `YES` | `NO` draws a paragraph's first appearance synchronously, so text never appears a frame after its view |

```bash
# launch arguments, e.g. in an Xcode scheme
-LoomAsyncText NO
-LoomAsyncTextFirstDraw NO
```

```swift
// or from the app, before React Native starts
UserDefaults.standard.set(false, forKey: "LoomAsyncTextFirstDraw")
```

At compile time, `LOOM_IOS_ASYNC_TEXT=0` removes the feature and
`LOOM_IOS_ASYNC_TEXT_FIRST_DRAW=0` changes the first-draw default.
