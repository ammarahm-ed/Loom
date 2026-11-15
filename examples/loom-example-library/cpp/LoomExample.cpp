#include "LoomExample.h"

#include <jsi/jsi.h>
#include <atomic>
#include <memory>
#include <optional>

// Detect Loom: stock React Native has no LoomConfig.h, so the include is guarded; a Loom build defines
// LOOM_RENDERER (and LOOM_API_VERSION). Everything Loom-specific is behind `LOOM_RENDERER`.
#if __has_include(<react/renderer/LoomConfig.h>)
#include <react/renderer/LoomConfig.h>
#endif

#if defined(LOOM_RENDERER) && LOOM_RENDERER
#include <react/loom/Loom.h>
static_assert(LOOM_API_VERSION >= 1, "LoomExample needs Loom API 1");
#else
#include <react/renderer/core/LayoutableShadowNode.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/UIManagerMountHook.h>
#endif

namespace loomexample {

using namespace facebook;
using namespace facebook::react;

namespace {

// ===================================================================================================
// Loom: talk to the renderer through <react/loom/Loom.h>.
// ===================================================================================================
#if defined(LOOM_RENDERER) && LOOM_RENDERER

class Backend {
 public:
  explicit Backend(loom::Runtime runtime) : runtime_(std::move(runtime)) {
    // onFrameApplied runs on the UI thread right after a frame's batch was applied.
    subscription_ = runtime_.onFrameApplied([this](const loom::FrameInfo&) { frames_.fetch_add(1); });
  }

  static std::unique_ptr<Backend> create(jsi::Runtime& rt) {
    auto runtime = loom::Runtime::from(rt);
    return runtime ? std::make_unique<Backend>(*runtime) : nullptr;
  }

  const char* name() const {
    return "loom";
  }

  std::optional<SurfaceId> surfaceOf(Tag tag) const {
    std::optional<SurfaceId> found;
    runtime_.forEachSurface([&](SurfaceId surfaceId) {
      if (!found && runtime_.hasNode(surfaceId, tag)) {
        found = surfaceId;
      }
    });
    return found;
  }

  // The frame in the surface: relative to the root (no ancestor given).
  std::optional<facebook::react::Rect> measure(Tag tag) const {
    if (auto surfaceId = surfaceOf(tag)) {
      if (auto metrics = runtime_.layoutRelativeTo(*surfaceId, tag, std::nullopt)) {
        return metrics->frame;
      }
    }
    return std::nullopt;
  }

  // The animation lane: a typed prop bag, no folly::dynamic, written to the view without a commit.
  bool setOpacity(Tag tag, double opacity) const {
    auto surfaceId = surfaceOf(tag);
    if (!surfaceId) {
      return false;
    }
    loom::AnimatedPropBag bag;
    bag.props.push_back(std::make_unique<AnimatedProp<Float>>(OPACITY, static_cast<Float>(opacity)));
    runtime_.updateAnimatedProps(*surfaceId, tag, std::move(bag));
    return true;
  }

  int frames() const {
    return frames_.load();
  }

 private:
  loom::Runtime runtime_;
  loom::Subscription subscription_;
  std::atomic<int> frames_{0};
};

// ===================================================================================================
// Fabric: the same four things through the shadow tree, a commit and a mount hook.
// ===================================================================================================
#else

class Backend : public UIManagerMountHook {
 public:
  explicit Backend(UIManager& uiManager) : uiManager_(&uiManager) {
    uiManager_->registerMountHook(*this);
  }
  ~Backend() override {
    uiManager_->unregisterMountHook(*this);
  }

  static std::unique_ptr<Backend> create(jsi::Runtime& rt) {
    auto binding = UIManagerBinding::getBinding(rt);
    return binding ? std::make_unique<Backend>(binding->getUIManager()) : nullptr;
  }

  const char* name() const {
    return "fabric";
  }

  void shadowTreeDidMount(const RootShadowNode::Shared&, HighResTimeStamp) noexcept override {
    frames_.fetch_add(1);
  }

  std::optional<facebook::react::Rect> measure(Tag tag) const {
    std::optional<facebook::react::Rect> frame;
    uiManager_->getShadowTreeRegistry().enumerate([&](const ShadowTree& tree, bool& stop) {
      auto root = tree.getCurrentRevision().rootShadowNode;
      if (auto node = find(*root, tag)) {
        frame = LayoutableShadowNode::computeRelativeLayoutMetrics(node->getFamily(), *root, {}).frame;
        stop = true;
      }
    });
    return frame;
  }

  // A commit that clones the node with a new opacity.
  bool setOpacity(Tag tag, double opacity) const {
    bool done = false;
    uiManager_->getShadowTreeRegistry().enumerate([&](const ShadowTree& tree, bool& stop) {
      auto root = tree.getCurrentRevision().rootShadowNode;
      auto node = find(*root, tag);
      if (!node) {
        return;
      }
      stop = true;
      tree.commit(
          [&](const RootShadowNode& oldRoot) {
            return std::static_pointer_cast<RootShadowNode>(oldRoot.cloneTree(
                node->getFamily(), [&](const ShadowNode& old) {
                  PropsParserContext context{old.getSurfaceId(), *uiManager_->getContextContainer()};
                  auto props = old.getComponentDescriptor().cloneProps(
                      context, old.getProps(), RawProps(folly::dynamic::object("opacity", opacity)));
                  return old.clone({.props = props});
                }));
          },
          {});
      done = true;
    });
    return done;
  }

  int frames() const {
    return frames_.load();
  }

 private:
  static const ShadowNode* find(const ShadowNode& from, Tag tag) {
    if (from.getTag() == tag) {
      return &from;
    }
    for (const auto& child : from.getChildren()) {
      if (auto found = find(*child, tag)) {
        return found;
      }
    }
    return nullptr;
  }

  UIManager* uiManager_;
  std::atomic<int> frames_{0};
};

#endif // LOOM_RENDERER

// ===================================================================================================
// The module: the same for both renderers.
// ===================================================================================================
class LoomExampleModule : public TurboModule {
 public:
  explicit LoomExampleModule(std::shared_ptr<CallInvoker> jsInvoker) : TurboModule("LoomExample", std::move(jsInvoker)) {
    methodMap_["renderer"] = MethodMetadata{0, &LoomExampleModule::renderer};
    methodMap_["measure"] = MethodMetadata{1, &LoomExampleModule::measure};
    methodMap_["setOpacity"] = MethodMetadata{2, &LoomExampleModule::setOpacity};
    methodMap_["frames"] = MethodMetadata{0, &LoomExampleModule::frames};
  }

 private:
  // The backend is created on first use, with the JS runtime of the call.
  Backend* backend(jsi::Runtime& rt) {
    if (!backend_) {
      backend_ = Backend::create(rt);
    }
    return backend_.get();
  }

  static jsi::Value renderer(jsi::Runtime& rt, TurboModule& self, const jsi::Value*, size_t) {
    auto* backend = static_cast<LoomExampleModule&>(self).backend(rt);
    return backend ? jsi::Value(jsi::String::createFromAscii(rt, backend->name())) : jsi::Value::null();
  }

  static jsi::Value measure(jsi::Runtime& rt, TurboModule& self, const jsi::Value* args, size_t) {
    auto* backend = static_cast<LoomExampleModule&>(self).backend(rt);
    auto frame = backend ? backend->measure(static_cast<Tag>(args[0].asNumber())) : std::nullopt;
    if (!frame) {
      return jsi::Value::null();
    }
    jsi::Object result(rt);
    result.setProperty(rt, "x", frame->origin.x);
    result.setProperty(rt, "y", frame->origin.y);
    result.setProperty(rt, "width", frame->size.width);
    result.setProperty(rt, "height", frame->size.height);
    return result;
  }

  static jsi::Value setOpacity(jsi::Runtime& rt, TurboModule& self, const jsi::Value* args, size_t) {
    auto* backend = static_cast<LoomExampleModule&>(self).backend(rt);
    return jsi::Value(backend != nullptr && backend->setOpacity(static_cast<Tag>(args[0].asNumber()), args[1].asNumber()));
  }

  static jsi::Value frames(jsi::Runtime& rt, TurboModule& self, const jsi::Value*, size_t) {
    auto* backend = static_cast<LoomExampleModule&>(self).backend(rt);
    return jsi::Value(backend ? backend->frames() : 0);
  }

  std::unique_ptr<Backend> backend_;
};

} // namespace

std::shared_ptr<TurboModule> createLoomExampleModule(std::shared_ptr<CallInvoker> jsInvoker) {
  return std::make_shared<LoomExampleModule>(std::move(jsInvoker));
}

} // namespace loomexample
