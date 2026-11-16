/*
 * Loom build configuration, for libraries.
 *
 * This header ships with Loom only; stock React Native has no such file. A library detects Loom with
 *
 *   #if __has_include(<react/renderer/LoomConfig.h>)
 *   #include <react/renderer/LoomConfig.h>
 *   #endif
 *   #if defined(LOOM_RENDERER) && LOOM_RENDERER
 *     // talk to Loom
 *   #else
 *     // Fabric
 *   #endif
 *
 * It is the same in prebuilt and from-source builds (Android prefab and iOS headers both carry it).
 * A from-source build with loom.renderer=false defines LOOM_RENDERER as 0 before this header, and keeps it.
 */

#pragma once

#ifndef LOOM_RENDERER
#define LOOM_RENDERER 1
#endif

/**
 * Version of the public Loom API for libraries (<react/loom/Loom.h>). It changes only when Loom's own
 * API changes, never because of the React Native version: each Loom release supports exactly one React
 * Native minor, which packaging enforces. Additions keep it; an incompatible change bumps it. A library
 * checks `LOOM_API_VERSION >= n` at compile time; `Runtime::from` also checks it at run time, so a
 * library compiled against an API this Loom no longer provides gets nullopt (and a log) instead of
 * undefined behaviour.
 *
 *   1: first API.
 *   2: `Runtime` is per React instance; reads are explicit (`latest()` never blocks, `synced()` waits
 *      for the caller's commits); `updateNativeProps` takes the SurfaceId; `surfaceOf(Tag)`.
 */
#define LOOM_API_VERSION 2
/** The oldest library API version this Loom still serves through `Runtime::from`. */
#define LOOM_API_MIN_SUPPORTED_VERSION 2
