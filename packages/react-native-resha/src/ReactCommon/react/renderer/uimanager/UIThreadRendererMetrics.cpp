/* Copyright (c) Meta Platforms, Inc. and its affiliates. */

#include "UIThreadRendererBinding.h"

#if UI_THREAD_RENDERER

namespace facebook::react {

bool UIThreadRendererBinding::parentMetrics(
    Tag tag,
    UIThreadRenderer::Metrics& out) {
  auto* liveRenderer = rendererOwning(tag);
  if (liveRenderer == nullptr) {
    return false;
  }
  // A direct child of the surface root measures from the surface's own origin,
  // which is what metricsRelativeTo(tag, parentTag) accumulates to.
  return liveRenderer->metricsRelativeTo(tag, liveRenderer->parentOf(tag), out);
}

bool UIThreadRendererBinding::windowMetrics(
    Tag tag,
    UIThreadRenderer::Metrics& out,
    bool includeTransform) {
  auto* liveRenderer = rendererOwning(tag);
  if (liveRenderer == nullptr ||
      !liveRenderer->metricsRelativeTo(tag, 0, out, includeTransform)) {
    return false;
  }
  // On Android, the direct Yoga tree does not always have the mounted
  // ScrollView as a layout ancestor (for example, when list cells are hosted
  // by a separate scroll-content view). Use the host view's live root-relative
  // origin for window/responder measurements so native scrolling is reflected.
  if (const auto liveLocation = liveRenderer->viewLocationInSurface(tag)) {
    out.x = liveLocation->first;
    out.y = liveLocation->second;
    return true;
  }
  // The surface itself sits at the window offset the host last reported.
  const auto constraints = constraintsFor(liveRenderer->surfaceId());
  out.x += constraints.viewportOffsetX;
  out.y += constraints.viewportOffsetY;
  return true;
}

} // namespace facebook::react

#endif // UI_THREAD_RENDERER
