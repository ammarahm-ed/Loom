/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomCompat.h"

#if LOOM_RENDERER

#include <cstring>
#include <limits>
#include <typeinfo>
#include <unordered_set>

#include <glog/logging.h>
#include <react/renderer/componentregistry/componentNameByReactViewName.h>
#include <react/renderer/components/view/YogaLayoutableShadowNode.h>
#include <react/renderer/core/LayoutableShadowNode.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <react/renderer/core/ShadowNodeFragment.h>

namespace facebook::react {

/** Named friend of YogaLayoutableShadowNode (see the Loom patch there). */
class LoomCompatAccess {
 public:
  static YGNodeConstRef yogaNode(const YogaLayoutableShadowNode& node) {
    return &node.yogaNode_;
  }
  static const StateData::Shared& data(const State& state) {
    return state.getDataPointer();
  }
};

namespace {

constexpr const char* kRegistryKey =
    "ComponentDescriptorRegistry_DO_NOT_USE_PRETTY_PLEASE";

/**
 * Components Loom renders with its own logic (unified Fabric names). They keep
 * Loom's path even though their descriptors are custom: Loom already
 * reproduces what their ShadowNodes do (text, scroll, image, root).
 */
bool isLoomNativeComponent(const std::string& unifiedName) {
  static const std::unordered_set<std::string> names{
      "View",
      "RootView",
      "Root",
      "Paragraph",
      "SelectableParagraph",
      "Text",
      "RawText",
      "Image",
      "ScrollView",
      "AndroidHorizontalScrollContentView",
      "TextInput",
      "AndroidTextInput",
      "LayoutConformance",
      "UnimplementedNativeView",
  };
  return names.count(unifiedName) != 0;
}

bool hasTypePrefix(const std::type_info& type, const char* prefix) {
  return std::strncmp(type.name(), prefix, std::strlen(prefix)) == 0;
}

/**
 * A component whose descriptor and ShadowNode are the bare codegen templates,
 * without custom state or measurement, has no C++ behaviour Loom's generic
 * path does not already reproduce.
 */
bool isPlainCodegenComponent(
    const ComponentDescriptor& descriptor,
    const ShadowNode& node) {
  // Itanium mangling: facebook::react::ConcreteComponentDescriptor<...> and
  // facebook::react::ConcreteViewShadowNode<...>. A hand-written subclass has
  // its own name and may override `adopt`, `layout` or `measureContent`.
  if (!hasTypePrefix(
          typeid(descriptor),
          "N8facebook5react27ConcreteComponentDescriptorI")) {
    return false;
  }
  if (!hasTypePrefix(typeid(node), "N8facebook5react22ConcreteViewShadowNodeI")) {
    return false;
  }
  if (node.getState() != nullptr) {
    return false;
  }
  return !node.getTraits().check(ShadowNodeTraits::Trait::MeasurableYogaNode);
}

} // namespace

LoomCompat::LoomCompat(std::shared_ptr<const ContextContainer> contextContainer)
    : contextContainer_(std::move(contextContainer)) {}

std::shared_ptr<const ComponentDescriptorRegistry> LoomCompat::registry() {
  if (registry_ == nullptr && contextContainer_ != nullptr) {
    auto weak =
        contextContainer_->find<std::weak_ptr<const ComponentDescriptorRegistry>>(
            kRegistryKey);
    if (weak.has_value()) {
      registry_ = weak->lock();
    }
  }
  return registry_;
}

const ComponentDescriptor* LoomCompat::descriptorFor(
    const std::string& componentName) {
  std::lock_guard lock(mutex_);
  auto cached = cache_.find(componentName);
  if (cached != cache_.end()) {
    return cached->second;
  }
  const auto* descriptor = classify(componentName);
  // A missing registry is not cached: it may simply not be published yet.
  if (descriptor != nullptr || registry_ != nullptr) {
    cache_.emplace(componentName, descriptor);
  }
  return descriptor;
}

const ComponentDescriptor* LoomCompat::registeredDescriptorFor(
    const std::string& componentName) {
  std::lock_guard lock(mutex_);
  auto descriptors = registry();
  if (descriptors == nullptr) {
    return nullptr;
  }
  try {
    return &descriptors->at(componentName);
  } catch (const std::exception& error) {
    LOG(WARNING) << "Loom compat: no descriptor for " << componentName << ": "
                 << error.what();
    return nullptr;
  }
}

const ComponentDescriptor* LoomCompat::classify(
    const std::string& componentName) {
  const auto unifiedName = componentNameByReactViewName(componentName);
  if (isLoomNativeComponent(unifiedName)) {
    return nullptr;
  }
  auto descriptors = registry();
  if (descriptors == nullptr) {
    return nullptr;
  }
  try {
    const auto& descriptor = descriptors->at(componentName);
    const auto fallback = descriptors->getFallbackComponentDescriptor();
    if (fallback != nullptr && &descriptor == fallback.get()) {
      return nullptr;
    }
    // One throwaway node answers what only an instance can: its dynamic type
    // and whether the component has state.
    PropsParserContext context{-1, *contextContainer_};
    auto props = descriptor.cloneProps(
        context, nullptr, RawProps(folly::dynamic::object()));
    auto family = descriptor.createFamily(
        ShadowNodeFamilyFragment{.tag = 0, .surfaceId = -1, .instanceHandle = nullptr});
    auto state = descriptor.createInitialState(props, family);
    auto probe = descriptor.createShadowNode(
        ShadowNodeFragment{.props = props, .state = state}, family);
    if (isPlainCodegenComponent(descriptor, *probe)) {
      return nullptr;
    }
    const ShadowNode& probeNode = *probe;
    LOG(INFO) << "Loom compat: " << componentName
              << " is backed by its ComponentDescriptor ("
              << typeid(probeNode).name() << ")";
    return &descriptor;
  } catch (const std::exception& error) {
    LOG(WARNING) << "Loom compat: cannot classify " << componentName << ": "
                 << error.what();
    return nullptr;
  }
}

LoomCompatNode::LoomCompatNode(
    std::shared_ptr<LoomCompat> compat,
    const ComponentDescriptor& descriptor,
    Tag tag,
    SurfaceId surfaceId,
    InstanceHandle::Shared instanceHandle,
    const folly::dynamic& props)
    : LoomCompatNode(
          compat,
          descriptor,
          tag,
          surfaceId,
          std::move(instanceHandle),
          descriptor.cloneProps(
              PropsParserContext{surfaceId, compat->contextContainer()},
              nullptr,
              RawProps(props))) {}

LoomCompatNode::LoomCompatNode(
    std::shared_ptr<LoomCompat> compat,
    const ComponentDescriptor& descriptor,
    Tag tag,
    SurfaceId surfaceId,
    InstanceHandle::Shared instanceHandle,
    Props::Shared parsed)
    : compat_(std::move(compat)), descriptor_(descriptor) {
  auto family = descriptor_.createFamily(ShadowNodeFamilyFragment{
      .tag = tag,
      .surfaceId = surfaceId,
      .instanceHandle = std::move(instanceHandle)});
  auto state = descriptor_.createInitialState(parsed, family);
  node_ = descriptor_.createShadowNode(
      ShadowNodeFragment{.props = parsed, .state = state}, family);
  node_->setMounted(true);
}

void LoomCompatNode::updateProps(const folly::dynamic& props) {
  PropsParserContext context{node_->getSurfaceId(), compat_->contextContainer()};
  auto parsed = descriptor_.cloneProps(context, node_->getProps(), RawProps(props));
  node_ = node_->clone(ShadowNodeFragment{.props = parsed});
  node_->setMounted(true);
}

void LoomCompatNode::updateProps(Props::Shared props) {
  node_ = node_->clone(ShadowNodeFragment{.props = std::move(props)});
  node_->setMounted(true);
}

bool LoomCompatNode::updateState(const StateUpdate& update) {
  if (update.family.get() != &node_->getFamily()) {
    return false;
  }
  const auto& oldState = node_->getState();
  if (oldState == nullptr) {
    return false;
  }
  auto newData = update.callback(LoomCompatAccess::data(*oldState));
  if (!newData) {
    return false;
  }
  auto newState = descriptor_.createState(node_->getFamily(), newData);
  node_ = node_->clone(ShadowNodeFragment{.state = newState});
  node_->setMounted(true);
  stateDirty = true;
  return true;
}

YGNodeConstRef LoomCompatNode::yogaNode() const {
  const auto* layoutable =
      dynamic_cast<const YogaLayoutableShadowNode*>(node_.get());
  return layoutable == nullptr ? nullptr : LoomCompatAccess::yogaNode(*layoutable);
}

bool LoomCompatNode::measurable() const {
  // Fabric measures any node whose Yoga node has a measure function. Besides the
  // MeasurableYogaNode trait, a descriptor can switch measurement on at runtime
  // (`adopt` calling `enableMeasurement()`, as the slider's does).
  if (node_->getTraits().check(ShadowNodeTraits::Trait::MeasurableYogaNode)) {
    return true;
  }
  auto yoga = yogaNode();
  return yoga != nullptr && YGNodeHasMeasureFunc(yoga);
}

Size LoomCompatNode::measure(
    const LayoutContext& layoutContext,
    const LayoutConstraints& layoutConstraints) const {
  const auto* layoutable =
      dynamic_cast<const LayoutableShadowNode*>(node_.get());
  if (layoutable == nullptr) {
    return {};
  }
  return layoutable->measureContent(layoutContext, layoutConstraints);
}

} // namespace facebook::react

#endif // LOOM_RENDERER
