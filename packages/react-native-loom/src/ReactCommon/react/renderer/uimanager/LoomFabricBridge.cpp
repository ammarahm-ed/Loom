/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomFabricBridge.h"

#include <react/renderer/bridging/bridging.h>
#include <react/renderer/core/ConcreteComponentDescriptor.h>

#if LOOM_RENDERER
#include <glog/logging.h>
#include <react/renderer/components/root/RootShadowNode.h>
#include <react/renderer/core/LayoutableShadowNode.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <react/renderer/core/ShadowNodeFragment.h>

#include <functional>
#include <unordered_set>
#include <vector>

#include "LoomBinding.h"
#endif

namespace facebook::react {

namespace {
thread_local LoomRawPropsCapture* activeRawPropsCapture = nullptr;
} // namespace

LoomRawPropsCapture* loomActiveRawPropsCapture() {
  return activeRawPropsCapture;
}

void loomRecordRawProps(
    LoomRawPropsCapture& capture,
    const Props* props,
    folly::dynamic rawProps) {
  capture.rawProps[props] = std::move(rawProps);
}

LoomRawPropsCaptureScope::LoomRawPropsCaptureScope(
    LoomRawPropsCapture& capture)
    : previous_(activeRawPropsCapture) {
  activeRawPropsCapture = &capture;
}

LoomRawPropsCaptureScope::~LoomRawPropsCaptureScope() {
  activeRawPropsCapture = previous_;
}

std::shared_ptr<const ShadowNode> loomShadowNodeFromJs(
    jsi::Runtime& rt,
    const jsi::Object& object) {
#if LOOM_RENDERER
  return LoomBinding::shadowNodeFromJs(rt, object);
#else
  (void)rt;
  (void)object;
  return nullptr;
#endif
}

#if LOOM_RENDERER

namespace {

LayoutMetrics layoutMetricsFrom(const LoomRenderer::Metrics& metrics) {
  LayoutMetrics layoutMetrics;
  layoutMetrics.frame.origin = {metrics.x, metrics.y};
  layoutMetrics.frame.size = {metrics.width, metrics.height};
  return layoutMetrics;
}

} // namespace

void installLoomFabricBridge() {
  loomShadowTreeCommitInterceptor().store(
      &LoomBinding::commitOnDemand, std::memory_order_release);
  loomShadowTreeNotifyInterceptor().store(
      &LoomBinding::layoutAnimationFrame, std::memory_order_release);
}

void loomRecordAnimationProps(Tag tag, folly::dynamic props, bool viewApplied) {
#if LOOM_WORKER
  LoomBinding::recordAnimationProps(tag, std::move(props), viewApplied);
#else
  (void)tag;
  (void)props;
  (void)viewApplied;
#endif
}

void loomFlushAnimationFrames() {
#if LOOM_WORKER
  LoomBinding::flushAnimationFrames();
#endif
}

void LoomBinding::synchronizeForFabricRead() {
#if LOOM_WORKER
  worker_->synchronize();
#endif
}

ShadowNodeFamily::Shared LoomBinding::fabricFamilyLocked(
    const ComponentDescriptor& descriptor,
    SurfaceId surfaceId,
    Tag tag,
    uint64_t identity,
    const InstanceHandle::Shared& instanceHandle) {
  hasFabricFamilies_.store(true, std::memory_order_relaxed);
  // Released families are destroyed here when this runs on the JS thread (see
  // header); Reanimated also calls in from the UI thread, which must not.
#if LOOM_WORKER
  if (LoomWorker::kJavaScriptOnUIThread || !worker_->onUIThread()) {
    releasedFabricFamilies_.clear();
  }
#else
  releasedFabricFamilies_.clear();
#endif
  auto& entry = fabricFamilies_[surfaceId][tag];
  if (entry.family != nullptr && entry.identity == identity &&
      &entry.family->getComponentDescriptor() == &descriptor) {
    return entry.family;
  }
  if (entry.family != nullptr) {
    releasedFabricFamilies_.push_back(std::move(entry.family));
  }
  auto family = descriptor.createFamily(
      ShadowNodeFamilyFragment{
          .tag = tag,
          .surfaceId = surfaceId,
          .instanceHandle = instanceHandle});
  entry = FabricFamily{};
  entry.family = family;
  entry.identity = identity;
  return family;
}

void LoomBinding::releaseFabricFamilies(
    SurfaceId surfaceId,
    const std::vector<Tag>* removedTags,
    const TagMap<uint64_t>& identities) {
  std::lock_guard lock(fabricFamiliesMutex_);
  auto families = fabricFamilies_.find(surfaceId);
  if (families == fabricFamilies_.end()) {
    return;
  }
  auto release = [&](TagMap<FabricFamily>::iterator it) {
    releasedFabricFamilies_.push_back(std::move(it->second.family));
    return families->second.erase(it);
  };
  if (removedTags != nullptr) {
    for (const auto tag : *removedTags) {
      if (auto it = families->second.find(tag); it != families->second.end()) {
        release(it);
      }
    }
    return;
  }
  for (auto it = families->second.begin(); it != families->second.end();) {
    it = identities.find(it->first) == identities.end() ? release(it)
                                                         : std::next(it);
  }
}

std::shared_ptr<const ShadowNode> LoomBinding::shadowNodeFromJs(
    jsi::Runtime& runtime,
    const jsi::Object& object) {
  if (!object.isHostObject<NodeRef>(runtime)) {
    return nullptr;
  }
  auto ref = object.getHostObject<NodeRef>(runtime);
  if (ref == nullptr || ref->tag == 0 || ref->draft == nullptr ||
      ref->draft->componentName == nullptr) {
    return nullptr;
  }
  std::shared_ptr<const ShadowNode> result;
  withSurfaceRenderer(
      ref->surfaceId,
      [&](LoomBinding& binding, const std::shared_ptr<LoomRenderer>&) {
        result = binding.handleShadowNode(*ref);
      });
  return result;
}

std::shared_ptr<const ShadowNode> LoomBinding::handleShadowNode(
    const NodeRef& ref) {
  if (compat_ == nullptr) {
    return nullptr;
  }
  const auto& draft = *ref.draft;
  const auto* descriptor = draft.compatDescriptor != nullptr
      ? draft.compatDescriptor
      : compat_->registeredDescriptorFor(*draft.componentName);
  if (descriptor == nullptr) {
    return nullptr;
  }
  ShadowNodeFamily::Shared family;
  Props::Shared props;
  {
    std::lock_guard lock(fabricFamiliesMutex_);
    family = fabricFamilyLocked(
        *descriptor, ref.surfaceId, ref.tag, draft.identity,
        draft.instanceHandle);
    if (draft.compatDescriptor != nullptr && draft.compatProps != nullptr) {
      props = draft.compatProps;
    } else {
      auto& cached = handleProps_[descriptor];
      if (cached == nullptr) {
        // The node stands for a view Loom mounts, so it says it forms one:
        // libraries check this to warn about views that may be flattened.
        PropsParserContext context{ref.surfaceId, compat_->contextContainer()};
        cached = descriptor->cloneProps(
            context,
            nullptr,
            RawProps(folly::dynamic::object("collapsable", false)));
      }
      props = cached;
    }
  }
  auto state = descriptor->createInitialState(props, family);
  return descriptor->createShadowNode(
      ShadowNodeFragment{.props = props, .state = state}, family);
}

std::shared_ptr<ShadowNode> LoomBinding::fabricNodeFromRenderer(
    LoomRenderer& liveRenderer,
    Tag tag,
    std::shared_ptr<const std::vector<std::shared_ptr<const ShadowNode>>>
        children) {
  if (compat_ == nullptr) {
    return nullptr;
  }
  const auto info = liveRenderer.fabricNodeInfo(tag);
  if (!info.has_value()) {
    return nullptr;
  }
  const ComponentDescriptor* descriptor = nullptr;
  Props::Shared props;
  if (info->compat != nullptr) {
    descriptor = &info->compat->shadowNode().getComponentDescriptor();
    props = info->compat->shadowNode().getProps();
  } else {
    descriptor = compat_->registeredDescriptorFor(*info->componentName);
  }
  if (descriptor == nullptr) {
    return nullptr;
  }
  const auto surfaceId = liveRenderer.surfaceId();
  ShadowNodeFamily::Shared family;
  {
    std::lock_guard lock(fabricFamiliesMutex_);
    family = fabricFamilyLocked(
        *descriptor,
        surfaceId,
        tag,
        info->identity,
        liveRenderer.instanceHandle(tag));
    if (props == nullptr) {
      auto& entry = fabricFamilies_[surfaceId][tag];
      if (entry.props != nullptr &&
          entry.propsGeneration == info->propsGeneration &&
          entry.propsDescriptor == descriptor) {
        props = entry.props;
      } else {
        PropsParserContext context{surfaceId, compat_->contextContainer()};
        try {
          props = descriptor->cloneProps(
              context, nullptr, RawProps(folly::dynamic(*info->props)));
        } catch (const std::exception& error) {
          LOG(WARNING) << "Loom compat: cannot parse props of "
                       << *info->componentName << ": " << error.what();
          props = descriptor->cloneProps(
              context, nullptr, RawProps(folly::dynamic::object()));
        }
        entry.props = props;
        entry.propsGeneration = info->propsGeneration;
        entry.propsDescriptor = descriptor;
      }
    }
  }
  auto state = descriptor->createInitialState(props, family);
  auto node = children != nullptr
      ? descriptor->createShadowNode(
            ShadowNodeFragment{
                .props = props, .children = children, .state = state},
            family)
      : descriptor->createShadowNode(
            ShadowNodeFragment{.props = props, .state = state}, family);
  if (auto* layoutable = dynamic_cast<LayoutableShadowNode*>(node.get())) {
    LoomRenderer::Metrics metrics;
    if (liveRenderer.metricsRelativeTo(
            tag,
            info->parent == liveRenderer.rootTag() ? 0 : info->parent,
            metrics,
            /*includeTransform=*/false,
            /*includeScroll=*/false)) {
      layoutable->setLayoutMetrics(layoutMetricsFrom(metrics));
    }
  }
  return node;
}

std::shared_ptr<const ShadowNode> LoomBinding::newestShadowNode(
    const ShadowNode& shadowNode) {
  const auto surfaceId = shadowNode.getSurfaceId();
  const auto tag = shadowNode.getTag();
  if (tag == surfaceId) {
    // The surface root is the shadow tree's own.
    return nullptr;
  }
  std::shared_ptr<const ShadowNode> result;
  withSurfaceRenderer(
      surfaceId,
      [&](LoomBinding& binding,
          const std::shared_ptr<LoomRenderer>& liveRenderer) {
        binding.synchronizeForFabricRead();
        result = binding.fabricNodeFromRenderer(*liveRenderer, tag, nullptr);
      });
  return result;
}

std::optional<LayoutMetrics> LoomBinding::relativeLayoutMetrics(
    const ShadowNode& shadowNode,
    const ShadowNode* ancestorShadowNode,
    LayoutableShadowNode::LayoutInspectingPolicy policy) {
  const auto surfaceId = shadowNode.getSurfaceId();
  std::optional<LayoutMetrics> result;
  withSurfaceRenderer(
      surfaceId,
      [&](LoomBinding& binding,
          const std::shared_ptr<LoomRenderer>& liveRenderer) {
        binding.synchronizeForFabricRead();
        const auto ancestorTag = ancestorShadowNode == nullptr ||
                ancestorShadowNode->getTag() == surfaceId
            ? Tag{0}
            : ancestorShadowNode->getTag();
        LoomRenderer::Metrics metrics;
        if (!liveRenderer->metricsRelativeTo(
                shadowNode.getTag(),
                ancestorTag,
                metrics,
                policy.includeTransform)) {
          result = EmptyLayoutMetrics;
          return;
        }
        if (ancestorTag == 0 && policy.includeViewportOffset) {
          const auto constraints = binding.constraintsFor(surfaceId);
          metrics.x += constraints.viewportOffsetX;
          metrics.y += constraints.viewportOffsetY;
        }
        result = layoutMetricsFrom(metrics);
      });
  return result;
}

std::optional<ShadowTreeCommitStatus> LoomBinding::commitOnDemand(
    const ShadowTree& shadowTree,
    const ShadowTreeCommitTransaction& transaction) {
  std::optional<ShadowTreeCommitStatus> result;
  withSurfaceRenderer(
      shadowTree.getSurfaceId(),
      [&](LoomBinding& binding,
          const std::shared_ptr<LoomRenderer>& liveRenderer) {
        result = binding.commitOnDemandImpl(
            *liveRenderer, shadowTree, transaction);
      });
  return result;
}

std::optional<ShadowTreeCommitStatus> LoomBinding::commitOnDemandImpl(
    LoomRenderer& liveRenderer,
    const ShadowTree& shadowTree,
    const ShadowTreeCommitTransaction& transaction) {
  const auto surfaceId = shadowTree.getSurfaceId();
  std::vector<Tag> held;
  {
    std::lock_guard lock(fabricFamiliesMutex_);
    auto families = fabricFamilies_.find(surfaceId);
    if (families == fabricFamilies_.end()) {
      return std::nullopt;
    }
    for (const auto& [tag, entry] : families->second) {
      held.push_back(tag);
    }
  }
  if (held.empty()) {
    return std::nullopt;
  }

  synchronizeForFabricRead();
  const auto rootTag = liveRenderer.rootTag();

  // The held nodes and every ancestor up to the surface root; a node that is
  // no longer attached to the root is left out.
  std::unordered_set<Tag> included;
  std::vector<Tag> chain;
  for (const auto tag : held) {
    chain.clear();
    auto current = tag;
    bool attached = false;
    while (current != 0) {
      if (current == rootTag || included.contains(current)) {
        attached = true;
        break;
      }
      const auto info = liveRenderer.fabricNodeInfo(current);
      if (!info.has_value() || info->identity == 0) {
        break;
      }
      chain.push_back(current);
      current = info->parent;
    }
    if (attached) {
      included.insert(chain.begin(), chain.end());
    }
  }
  if (included.empty()) {
    return std::nullopt;
  }

  // Built bottom-up, children in the renderer's order.
  std::unordered_map<Tag, std::shared_ptr<const ShadowNode>> built;
  std::function<std::shared_ptr<const ShadowNode>(Tag)> build =
      [&](Tag tag) -> std::shared_ptr<const ShadowNode> {
    auto children =
        std::make_shared<std::vector<std::shared_ptr<const ShadowNode>>>();
    if (const auto* childTags = liveRenderer.childrenOfRef(tag)) {
      for (const auto child : std::vector<Tag>(*childTags)) {
        if (included.contains(child)) {
          if (auto node = build(child)) {
            children->push_back(std::move(node));
          }
        }
      }
    }
    auto node = fabricNodeFromRenderer(liveRenderer, tag, std::move(children));
    if (node != nullptr) {
      built.emplace(tag, node);
    }
    return node;
  };

  auto rootChildren =
      std::make_shared<std::vector<std::shared_ptr<const ShadowNode>>>();
  if (const auto* childTags = liveRenderer.childrenOfRef(rootTag)) {
    for (const auto child : std::vector<Tag>(*childTags)) {
      if (included.contains(child)) {
        if (auto node = build(child)) {
          rootChildren->push_back(std::move(node));
        }
      }
    }
  }
  if (built.empty()) {
    return std::nullopt;
  }

  const auto currentRoot = shadowTree.getCurrentRevision().rootShadowNode;
  auto partialRoot = std::static_pointer_cast<const RootShadowNode>(
      currentRoot->ShadowNode::clone(
          {.props = ShadowNodeFragment::propsPlaceholder(),
           .children = rootChildren}));

  LoomRawPropsCapture capture;
  RootShadowNode::Unshared newRoot;
  {
    LoomRawPropsCaptureScope scope(capture);
    newRoot = transaction(*partialRoot);
  }
  if (newRoot == nullptr) {
    return ShadowTreeCommitStatus::Cancelled;
  }

  // The props the transaction changed, as the raw props it applied.
  std::vector<std::pair<Tag, folly::dynamic>> patches;
  std::function<void(const ShadowNode&)> collect = [&](const ShadowNode& node) {
    for (const auto& child : node.getChildren()) {
      auto previous = built.find(child->getTag());
      if (previous == built.end()) {
        continue;
      }
      if (child == previous->second) {
        continue;
      }
      if (child->getProps() != previous->second->getProps()) {
        auto raw = capture.rawProps.find(child->getProps().get());
        if (raw != capture.rawProps.end() && raw->second.isObject() &&
            !raw->second.empty()) {
          patches.emplace_back(child->getTag(), std::move(raw->second));
        }
      }
      collect(*child);
    }
  };
  collect(*newRoot);
  if (patches.empty()) {
    return std::nullopt;
  }

#if LOOM_WORKER
  auto token = tokenFor(surfaceId);
  for (auto& [tag, props] : patches) {
    stageNativeProps(tag, liveRenderer.identityOf(tag), token, std::move(props));
  }
#else
  for (auto& [tag, props] : patches) {
    liveRenderer.setNativeProps(tag, props);
  }
#endif
  return ShadowTreeCommitStatus::Succeeded;
}

#endif // LOOM_RENDERER

} // namespace facebook::react
