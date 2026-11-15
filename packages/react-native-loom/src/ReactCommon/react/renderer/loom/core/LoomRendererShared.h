/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

// Private to the LoomRenderer*.cpp files: the includes they share and the few helpers
// that more than one of them uses. Defined in LoomRendererProps.cpp unless inline here.

#include "LoomRenderer.h"
#include "LoomRendererInternals.h"
#include "../worker/LoomWorkerProfile.h"

#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/renderer/core/DynamicEventPayload.h>
#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <limits>
#include <tuple>
#include <utility>




#if LOOM_RENDERER
namespace facebook::react {
namespace loom::internal {

/** `display: none` and visibility, matching `DisplayType` in the shadow tree.
 */
constexpr int kDisplayNone = 0;
constexpr int kDisplayFlex = 1;

/** `LayoutDirection` values as Android's `updateLayout` expects them. */
constexpr int kLayoutDirectionLTR = 1;
constexpr int kLayoutDirectionRTL = 2;

inline uint64_t childPairKey(Tag parent, Tag child) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(parent)) << 32) |
      static_cast<uint32_t>(child);
}

/**
 * The host-ready shape of a props payload: a flat object, with any nested
 * `style` hoisted into it. React already sends props in this shape, so the copy
 * is skipped whenever there is no `style` key.
 */
folly::dynamic hoistProps(const folly::dynamic& props);

/** True when the payload has no nested `style`, so it is already host-ready. */
inline bool isFlatProps(const folly::dynamic& props) {
  return props.isObject() && props.count("style") == 0;
}

/** True when every key of the patch is `backgroundColor` or `opacity`. */
bool isConservativePaintOnlyPatch(const folly::dynamic& patch);

// Which components take only the changed keys on update (see `Node::roleSendsPropDiffs`).
// Depends on the component name alone, so `createNode` evaluates it once and `mergeProps`
// builds the delta only for nodes that will ship it.
inline bool componentSendsPropDiffs(const std::string& name) {
  const auto policy = LoomPlatformTraits::current().sendsPropDiffs;
  return policy != nullptr && policy(name);
}

} // namespace loom::internal

using loom::findProp;
using loom::numberProp;
using loom::propsFromValue;
using loom::requireArguments;
using loom::stringProp;
using loom::internal::childPairKey;
using loom::internal::componentSendsPropDiffs;
using loom::internal::hoistProps;
using loom::internal::isConservativePaintOnlyPatch;
using loom::internal::isFlatProps;
using loom::internal::kDisplayFlex;
using loom::internal::kDisplayNone;
using loom::internal::kLayoutDirectionLTR;
using loom::internal::kLayoutDirectionRTL;

} // namespace facebook::react
#endif // LOOM_RENDERER
