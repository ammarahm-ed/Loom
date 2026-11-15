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
 * Version of the public Loom API for libraries. It only grows, by additions.
 * A library that needs API n checks `LOOM_API_VERSION >= n`.
 */
#define LOOM_API_VERSION 1
