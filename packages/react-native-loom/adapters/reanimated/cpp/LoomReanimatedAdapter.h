/* Loom adapter for react-native-reanimated. See ../README.md. */

#pragma once

#include <reanimated/LayoutAnimations/LayoutAnimationsProxyCommon.h>
#include <reanimated/LayoutAnimations/LayoutAnimationsProxyRegistry.h>

#include <memory>

namespace reanimated::loom {

/**
 * The layout-animations registry Reanimated uses on Loom: every surface gets a
 * `LoomLayoutAnimationsProxy`. Called by the adapter's copy of
 * `ReanimatedModuleProxy.cpp`, the only Reanimated file the adapter replaces.
 */
std::shared_ptr<LayoutAnimationsProxyRegistry> createLayoutAnimationsProxyRegistry(
    const LayoutAnimationsProxyDependencies &dependencies);

} // namespace reanimated::loom
