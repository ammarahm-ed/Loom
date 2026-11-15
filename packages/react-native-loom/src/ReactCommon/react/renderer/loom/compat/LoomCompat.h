/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#if LOOM_RENDERER

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <folly/dynamic.h>
#include <react/renderer/componentregistry/ComponentDescriptorRegistry.h>
#include <react/renderer/core/ComponentDescriptor.h>
#include <react/renderer/core/InstanceHandle.h>
#include <react/renderer/core/LayoutConstraints.h>
#include <react/renderer/core/LayoutContext.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/core/ShadowNode.h>
#include <react/renderer/core/State.h>
#include <react/renderer/core/StateUpdate.h>
#include <react/utils/ContextContainer.h>
#include <yoga/Yoga.h>

namespace facebook::react {

/**
 * Loom's Fabric compatibility layer, contract 1 (see .local/compat-audit/DESIGN.md):
 * components whose registered ComponentDescriptor carries its own C++ logic
 * (custom state, `adopt`/`adjustLayoutWithState`, `measureContent`, a
 * hand-written ShadowNode) are backed by a real ShadowNode, so that logic runs
 * exactly as in Fabric. Everything else - core components Loom renders itself
 * and plain codegen components - never gets one.
 *
 * One instance per binding, shared by its surfaces' renderers.
 */
class LoomCompat {
 public:
  explicit LoomCompat(std::shared_ptr<const ContextContainer> contextContainer);

  /**
   * The descriptor that backs `componentName` (a React view name) when that
   * component needs a compat node, otherwise null. Classified once per name,
   * then a cached lookup. Thread-safe.
   */
  const ComponentDescriptor* descriptorFor(const std::string& componentName);

  /**
   * The registered descriptor of any component (the registry's fallback for
   * one it does not know), for building the Fabric node a library asks for.
   * Null until the registry is published. Thread-safe.
   */
  const ComponentDescriptor* registeredDescriptorFor(
      const std::string& componentName);

  const ContextContainer& contextContainer() const {
    return *contextContainer_;
  }

 private:
  std::shared_ptr<const ComponentDescriptorRegistry> registry();
  const ComponentDescriptor* classify(const std::string& componentName);

  std::shared_ptr<const ContextContainer> contextContainer_;
  std::mutex mutex_;
  std::shared_ptr<const ComponentDescriptorRegistry> registry_;
  /** Null for a component that does not need a compat node. */
  std::unordered_map<std::string, const ComponentDescriptor*> cache_;
};

/**
 * One Loom node's real ShadowNode: props parsed by its own descriptor, its own
 * family and state, `adopt` run on every revision. It has no children and is
 * never laid out itself: Loom lays the node out in its own Yoga tree using the
 * Yoga style this ShadowNode resolved, and calls `measureContent` from Loom's
 * measure function. Confined to the renderer's thread.
 */
class LoomCompatNode {
 public:
  LoomCompatNode(
      std::shared_ptr<LoomCompat> compat,
      const ComponentDescriptor& descriptor,
      Tag tag,
      SurfaceId surfaceId,
      InstanceHandle::Shared instanceHandle,
      const folly::dynamic& props);

  /**
   * Same, with props the descriptor already parsed. The JS thread parses them
   * from the JSI value as Fabric does, so props that only exist as JS values
   * (host objects, functions) reach the component intact.
   */
  LoomCompatNode(
      std::shared_ptr<LoomCompat> compat,
      const ComponentDescriptor& descriptor,
      Tag tag,
      SurfaceId surfaceId,
      InstanceHandle::Shared instanceHandle,
      Props::Shared props);

  /** Re-parses the merged props through the descriptor (runs `adopt`). */
  void updateProps(const folly::dynamic& props);
  /** Adopts props the descriptor already parsed. */
  void updateProps(Props::Shared props);

  /**
   * Applies a state update addressed to this node's family, as
   * `UIManager::updateState` does. False when the update is for another family
   * or its callback declined.
   */
  bool updateState(const StateUpdate& update);

  const ShadowNode& shadowNode() const {
    return *node_;
  }
  const State::Shared& state() const {
    return node_->getState();
  }
  const ShadowNodeFamily& family() const {
    return node_->getFamily();
  }
  /** The ShadowNode's own resolved Yoga node (style only; never laid out). */
  YGNodeConstRef yogaNode() const;
  /** The component measures its own content (`MeasurableYogaNode`). */
  bool measurable() const;
  Size measure(
      const LayoutContext& layoutContext,
      const LayoutConstraints& layoutConstraints) const;

  /** Loom props generation this ShadowNode was last cloned from. */
  uint64_t propsGeneration{0};
  /** The state changed since the host last received it. */
  bool stateDirty{true};

 private:
  std::shared_ptr<LoomCompat> compat_;
  const ComponentDescriptor& descriptor_;
  std::shared_ptr<const ShadowNode> node_;
};

} // namespace facebook::react

#endif // LOOM_RENDERER
