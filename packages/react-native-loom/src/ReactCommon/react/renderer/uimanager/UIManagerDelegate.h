/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/FrameworksGuard.h>

#include <React/Graphics.h>
#include <React/RendererCore.h>

#include <folly/dynamic.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/uimanager/LoomMountBatch.h>
#include <react/renderer/uimanager/LoomTextMeasurement.h>
#include <react/renderer/mounting/MountingCoordinator.h>
#include <react/renderer/mounting/ShadowTree.h>

namespace facebook::react {

struct AnimatedProps;

/*
 * Abstract class for UIManager's delegate.
 */
class UIManagerDelegate {
 public:
// Loom delegate methods: declared in every build so the class has one layout for code compiled without LOOM_RENDERER (third-party libraries).
  // Direct host operations used by the independent UI-thread renderer. They
  // are deliberately separate from the Fabric transaction callbacks below.
  //
  // The renderer speaks in flat, ordered operation batches (see
  // `LoomRenderer::DirectOperation`); everything else here exists because
  // the JS side hands the renderer *tags* rather than shadow nodes, so the
  // operations that Fabric used to reach through a shadow node have to be
  // forwarded by tag.
  /**
   * Publishes the viewport a surface is laid out against: the surface size in
   * DIP plus the pixel density and window offset that text measurement and
   * `measureInWindow` need to answer in the same coordinate space the
   * shadow-tree path does.
   */
  virtual void loomSetConstraints(
      SurfaceId,
      float minWidth,
      float minHeight,
      float maxWidth,
      float maxHeight,
      bool isRTL,
      float viewportOffsetX,
      float viewportOffsetY) {}
  virtual std::function<void(std::function<void()>)> loomGetUIExecutor() { return {}; }
  virtual bool loomCanMeasureTextOnWorker() { return false; }
  virtual void loomApplyBatch(
      SurfaceId,
      LoomMountBatch operations) {}
  virtual void loomFailSurface(
      SurfaceId,
      const std::string& /*reason*/) {}
  virtual void loomRegisterEventTarget(
      SurfaceId,
      Tag,
      InstanceHandle::Shared) {}
  virtual void loomUnregisterEventTarget(SurfaceId, Tag) {}
  virtual void loomDispatchEvent(
      SurfaceId,
      Tag,
      const std::string& eventName,
      folly::dynamic payload,
      RawEvent::Category category,
      bool unique) {}
  /**
   * Resolves a node's text props through React Native's own props parser,
   * applying them on top of the enclosing text's `inherited` attributes.
   * `outParagraphAttributes` is only filled for paragraph components.
   *
   * Text props live in a module this interface cannot reach, so the parsing
   * itself has to happen on the platform side.
   */
  virtual TextAttributes loomParseTextAttributes(
      SurfaceId,
      const std::string& componentName,
      const folly::dynamic& props,
      const TextAttributes& inherited,
      ParagraphAttributes& outParagraphAttributes) {
    return inherited;
  }
  /**
   * Measures a resolved text payload with the platform's own text engine,
   * inside the given constraints. `std::nullopt` means the platform could not
   * measure, and the renderer falls back to an estimate.
   *
   * Like the attribute parsing above, this has to happen on the platform side:
   * the text layout manager is not part of this module.
   */
  virtual std::optional<Size> loomMeasureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& /*minimumSize*/,
      const Size& /*maximumSize*/) {
    return std::nullopt;
  }
  virtual LoomTextMeasurement loomPrepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return {};
  }
  virtual LoomTextMeasurement loomPrepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return {};
  }
  /** Returns the live native scroll position for a mounted direct view. */
  virtual std::pair<float, float> loomGetScrollOffset(
      SurfaceId,
      Tag) {
    return {0, 0};
  }
  /** Returns a live direct view origin in DIP relative to its React root. */
  virtual std::optional<std::pair<float, float>>
  loomGetViewLocationInSurface(SurfaceId, Tag) {
    return std::nullopt;
  }
  virtual void loomReportUnsupportedInlineAttachment(
      SurfaceId,
      Tag) {}
  /**
   * Publishes the resolved text payload for a paragraph. Android renders text
   * exclusively from this state, so it cannot be expressed as props.
   */
  virtual void loomUpdateState(const LoomTextStateUpdate&) {}
  /**
   * Loom compatibility layer: publishes a component's own state object (made
   * by its ComponentDescriptor) to its host view, exactly as Fabric's
   * UpdateState mutation would.
   */
  virtual void loomUpdateComponentState(
      SurfaceId,
      Tag,
      const std::shared_ptr<const State>&) {}
  virtual void loomUpdateScrollContentBounds(
      SurfaceId,
      Tag,
      const folly::dynamic&,
      const Rect&) {}
  virtual void loomDispatchCommand(
      SurfaceId,
      Tag,
      const std::string& commandName,
      const folly::dynamic& args) {}
  virtual void loomSendAccessibilityEvent(
      SurfaceId,
      Tag,
      const std::string& eventType) {}
  virtual void loomSetIsJSResponder(
      SurfaceId,
      Tag,
      bool isJSResponder,
      bool blockNativeResponder) {}
// (end of Loom delegate methods)

  /*
   * Called right after a new/updated Shadow Node tree is constructed.
   * For this moment the tree is already laid out and sealed.
   */
  virtual void uiManagerDidFinishTransaction(
      std::shared_ptr<const MountingCoordinator> mountingCoordinator,
      bool mountSynchronously) = 0;

  /*
   * Called each time when UIManager constructs a new Shadow Node. Receiver
   * might use this to optimistically allocate a new native view
   * instances.
   */
  virtual void uiManagerDidCreateShadowNode(const ShadowNode &shadowNode) = 0;

  /*
   * Called when UIManager wants to dispatch a command to the mounting layer.
   */
  virtual void uiManagerDidDispatchCommand(
      const std::shared_ptr<const ShadowNode> &shadowNode,
      const std::string &commandName,
      const folly::dynamic &args) = 0;

  /*
   * Called when UIManager wants to dispatch some accessibility event
   * to the mounting layer. eventType is platform-specific and not all
   * platforms will necessarily implement the same set of events.
   */
  virtual void uiManagerDidSendAccessibilityEvent(
      const std::shared_ptr<const ShadowNode> &shadowNode,
      const std::string &eventType) = 0;

  /*
   * Set JS responder for a view.
   */
  virtual void uiManagerDidSetIsJSResponder(
      const std::shared_ptr<const ShadowNode> &shadowNode,
      bool isJSResponder,
      bool blockNativeResponder) = 0;

  /*
   * Synchronous view update.
   */
  virtual void uiManagerShouldSynchronouslyUpdateViewOnUIThread(Tag tag, const folly::dynamic &props) = 0;

  /*
   * Synchronous update of animated props for many views at once.
   */
  virtual void uiManagerShouldSynchronouslyUpdateAnimatedProps(
      const std::unordered_map<Tag, AnimatedProps> &updates) = 0;

  /*
   * Called after updateShadowTree is invoked.
   */
  virtual void uiManagerDidUpdateShadowTree(const std::unordered_map<Tag, folly::dynamic> &tagToProps) = 0;

  /*
   * Add event listener.
   */
  virtual void uiManagerShouldAddEventListener(std::shared_ptr<const EventListener> listener) = 0;

  /*
   * Remove event listener.
   */
  virtual void uiManagerShouldRemoveEventListener(const std::shared_ptr<const EventListener> &listener) = 0;

  /*
   * Start surface.
   */
  virtual void uiManagerDidStartSurface(const ShadowTree &shadowTree) = 0;

  /*
   * Called after a new React revision of the shadow tree is committed.
   */
  virtual void uiManagerDidFinishReactCommit(const ShadowTree &shadowTree) = 0;

  using OnSurfaceStartCallback = std::function<void(const ShadowTree &shadowTree)>;
  virtual void uiManagerShouldAddOnSurfaceStartCallback(OnSurfaceStartCallback &&callback) = 0;

  // View transition bitmap snapshot capture and application.
  virtual void uiManagerDidCaptureViewSnapshot(Tag tag, SurfaceId surfaceId) = 0;
  virtual void uiManagerDidSetViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId) = 0;
  virtual void uiManagerDidClearPendingSnapshots() = 0;

  virtual ~UIManagerDelegate() noexcept = default;
};

} // namespace facebook::react
