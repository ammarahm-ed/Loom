/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <unistd.h>
#include <folly/json.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include "LoomBinding.h"
#include "LoomFabricBridge.h"
#include "LoomLayoutAnimations.h"
#include "LoomRendererInternals.h"
#include "LoomSettingsHost.h"
#include "LoomWorkerProfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <react/renderer/runtimescheduler/JankReporter.h>
#include <react/renderer/runtimescheduler/RuntimeSchedulerFrameCallbackScope.h>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_set>

#include <jsi/JSIDynamic.h>



#if LOOM_RENDERER
namespace facebook::react {

#if LOOM_PROFILE
#define LOOM_HOST_CALL_SCOPE(name, ...) \
  LoomBinding::HostCallScope name(__VA_ARGS__)
#else
#define LOOM_HOST_CALL_SCOPE(name, ...)
#endif
struct LoomBinding::Lifetime {
  std::shared_mutex gate;
  LoomBinding* binding{nullptr};
};

namespace {

using loom::propsFromValue;
using loom::requireArguments;

template <typename Draft>
void classifyDraft(Draft& draft) {
  draft.roles =
      LoomPlatformTraits::current().components.classify(*draft.componentName);
}

/**
 * Whether a node's live child list differs from the list this commit selected.
 * Uses the borrowed accessor: the copying form allocated a vector per node on
 * every commit just to compare it.
 */
bool childrenDifferFrom(
    const LoomRenderer& renderer,
    Tag tag,
    const std::vector<Tag>& expected) {
  const auto* live = renderer.childrenOfRef(tag);
  if (live == nullptr) {
    return !expected.empty();
  }
  return *live != expected;
}

/**
 * Guards the two maps below. Rendering runs on the JS thread, which is the UI
 * thread in this mode, and surface callbacks arrive there too, so this is
 * insurance rather than a real contention point. It is deliberately never held
 * while a renderer runs: a commit can call back into the host, which can call
 * back into this binding.
 */
std::mutex& registryMutex() {
  static std::mutex mutex;
  return mutex;
}

/**
 * Constraints are keyed by surface rather than held per binding, because a
 * surface is started before JavaScript - and therefore before the binding that
 * will own its renderer - exists. Android assigns each surface its root tag;
 * ReactRootViewTagGenerator is a process-wide synchronized counter, so these
 * IDs do not collide across React instances in this process. Keep that
 * uniqueness requirement explicit: SurfaceId is also the key used by the
 * renderer and Java mounting manager.
 */
std::unordered_map<SurfaceId, LoomBinding::SurfaceConstraints>&
surfaceConstraints() {
  static std::
      unordered_map<SurfaceId, LoomBinding::SurfaceConstraints>
          constraints;
  return constraints;
}

/** Bindings that currently own renderers, so a static call can reach them. */
std::unordered_map<
    LoomBinding*,
    std::shared_ptr<LoomBinding::Lifetime>>&
liveBindings() {
  static std::unordered_map<
      LoomBinding*,
      std::shared_ptr<LoomBinding::Lifetime>>
      bindings;
  return bindings;
}

std::vector<std::shared_ptr<LoomBinding::Lifetime>>
bindingSnapshot() {
  std::vector<std::shared_ptr<LoomBinding::Lifetime>> snapshot;
  std::lock_guard lock(registryMutex());
  snapshot.reserve(liveBindings().size());
  for (const auto& entry : liveBindings()) {
    snapshot.push_back(entry.second);
  }
  return snapshot;
}

float& directPointScaleFactorStorage() {
  static float factor = 1.0f;
  return factor;
}

float directPointScaleFactorValue() {
  std::lock_guard lock(registryMutex());
  return directPointScaleFactorStorage();
}



std::optional<std::string> rawTextFromValue(
    jsi::Runtime& runtime,
    const jsi::Value& value) {
  if (!value.isObject()) {
    return std::nullopt;
  }
  auto object = value.asObject(runtime);
  auto names = object.getPropertyNames(runtime);
  if (names.size(runtime) != 1) {
    return std::nullopt;
  }
  auto name = names.getValueAtIndex(runtime, 0);
  if (!name.isString() || name.getString(runtime).utf8(runtime) != "text") {
    return std::nullopt;
  }
  auto text = object.getProperty(runtime, "text");
  if (!text.isString()) {
    return std::nullopt;
  }
  return text.getString(runtime).utf8(runtime);
}

/** `Node.compareDocumentPosition` bit values, as the DOM API defines them. */
constexpr unsigned kDocumentPositionDisconnected = 0x01;

} // namespace

LoomBinding::LoomBinding(
    std::shared_ptr<UIManager> uiManager)
    : UIManagerBinding(std::move(uiManager)) {
  lifetime_ = std::make_shared<Lifetime>();
  lifetime_->binding = this;
  // Registered before the JS runtime installs this binding, by the platform
  // that owns the UI manager; the context container is all this reads.
  if (const auto container = getUIManager().getContextContainer()) {
    hostOwner_ = LoomHost::findIn(*container);
  }
  if (hostOwner_ == nullptr) {
    LOG(FATAL)
        << "Loom: the UIManager's ContextContainer has no LoomHost. The platform must call "
           "LoomHost::registerIn(contextContainer, host) before the JS runtime installs the "
           "UI manager binding (iOS: RCTSurfacePresenter init; Android: "
           "FabricUIManagerBinding::installFabricUIManager).";
  }
  host_ = hostOwner_.get();
  text_ = host_->textServices();
  installLoomFabricBridge();
  // Power-of-two bucket counts keep these on mask indexing (see LoomTagHash).
  mountedIdentities_.reserve(1024);
  mountedTokens_.reserve(1024);
#if LOOM_WORKER
  const std::weak_ptr<Lifetime> weakLifetime = lifetime_;
  // The host's UI executor is resolved on first use, not here: it can need
  // platform state that exists only once the first surface runs. Every use of it
  // comes after the first commit.
  worker_ = std::make_unique<LoomWorker>([weakLifetime] {
    auto lifetime = weakLifetime.lock();
    if (!lifetime || lifetime->binding == nullptr) {
      return;
    }
    auto uiExecutor = lifetime->binding->platformUIExecutor();
    if (!uiExecutor) {
      return;
    }
    uiExecutor([weakLifetime] {
      if (auto lifetime = weakLifetime.lock()) {
        // Result application can stop/restart a surface synchronously. The
        // runtime owns this binding throughout the callback; no registry
        // lock is held across UI effects.
        if (auto* binding = lifetime->binding) {
          if (!LoomWorker::kJavaScriptOnUIThread)
            binding->refreshCanMeasureTextOnWorker();
          binding->worker_->applyReady();
        }
      }
    });
  });
  // The first commit resolves the executor (see publishWorkerCommit).
#endif
#if LOOM_WORKER && LOOM_JS_ON_MAIN
  // Loom: when timers and requestAnimationFrame callbacks have run in
  // the frame's animation phase, apply whatever they committed now, so the
  // traversal that follows draws it instead of the next frame. A synchronous
  // commit waits for the worker; the UI thread is also the JS thread here, so
  // nothing can wait on it in turn.
  FrameCallbackScope::afterFrameCallbacks() = [weakLifetime = std::weak_ptr<Lifetime>(lifetime_)] {
    if (auto lifetime = weakLifetime.lock()) {
      if (auto* binding = lifetime->binding) {
        // Bounded: a commit that is still computing lands next frame rather
        // than delaying this one.
        binding->worker_->synchronizeFor(kFrameDrainBudget);
      }
    }
  };
#endif
  std::lock_guard lock(registryMutex());
  liveBindings().emplace(this, lifetime_);
}

#if LOOM_WORKER
std::function<void(std::function<void()>)> LoomBinding::platformUIExecutor() {
  std::lock_guard lock(uiExecutorMutex_);
  if (uiExecutor_) {
    return uiExecutor_;
  }
  auto executor = host_->uiExecutor();
  if (!executor) {
    return nullptr;
  }
  uiExecutor_ = executor;
  if (!LoomWorker::kJavaScriptOnUIThread) {
    // JS runs off the UI thread, where the Java text-capability query is not
    // allowed; seed the cached answer from the UI thread before the first
    // commit needs it.
    const std::weak_ptr<Lifetime> weakLifetime = lifetime_;
    executor([weakLifetime] {
      if (auto lifetime = weakLifetime.lock()) {
        if (auto* binding = lifetime->binding)
          binding->refreshCanMeasureTextOnWorker();
      }
    });
  }
  return uiExecutor_;
}
#endif

#if LOOM_PROFILE
LoomBinding::HostCallScope::HostCallScope(
    LoomBinding& binding,
    SurfaceId surfaceId,
    ProfiledHostMethod method)
    : binding(binding),
      surfaceId(surfaceId),
      generation(binding.profileCommitGeneration_[surfaceId]),
      method(method),
      start(std::chrono::steady_clock::now()) {
  if (generation == 0) {
    generation = 1;
    binding.profileCommitGeneration_[surfaceId] = generation;
  }
  auto& profile = binding.profileHostCalls_[surfaceId][generation];
  if (profile.firstHostCallEpochNanoseconds == 0) {
    profile.firstHostCallEpochNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    profile.firstHostCallSteadyNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            start.time_since_epoch())
            .count();
  }
}

LoomBinding::HostCallScope::~HostCallScope() {
  const auto finished = std::chrono::steady_clock::now();
  auto& profile = binding.profileHostCalls_[surfaceId][generation];
  auto& stats = profile.methods[static_cast<size_t>(method)];
  ++stats.calls;
  stats.totalNanoseconds += static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - start)
          .count());
}
#endif

LoomBinding::~LoomBinding() {
#if LOOM_WORKER
  for (auto& [surface, token] : surfaceTokens_)
    token->active = false;
  worker_->shutdown();
#if LOOM_JS_ON_MAIN
  FrameCallbackScope::afterFrameCallbacks() = {};
#endif
#endif
  {
    std::lock_guard lock(registryMutex());
    liveBindings().erase(this);
  }
  // Wait for an in-flight static callback that already acquired the token.
  // New callbacks observe a null binding under the same gate. This makes the
  // raw pointer safe without requiring the binding itself to be heap-owned by
  // the registry.
  std::unique_lock lock(lifetime_->gate);
  lifetime_->binding = nullptr;
}

#pragma mark - Surface lifecycle

void LoomBinding::setSurfaceConstraints(
    SurfaceId surfaceId,
    float minWidth,
    float minHeight,
    float maxWidth,
    float maxHeight,
    bool isRTL,
    float viewportOffsetX,
    float viewportOffsetY) {
  SurfaceConstraints constraints;
  constraints.minWidth = minWidth > 0 ? minWidth : 0;
  constraints.minHeight = minHeight > 0 ? minHeight : 0;
  constraints.maxWidth = maxWidth > 0 ? maxWidth : 0;
  constraints.maxHeight = maxHeight > 0 ? maxHeight : 0;
  constraints.isRTL = isRTL;
  constraints.viewportOffsetX = viewportOffsetX;
  constraints.viewportOffsetY = viewportOffsetY;

  {
    std::lock_guard lock(registryMutex());
    surfaceConstraints()[surfaceId] = constraints;
  }

  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    if (auto* binding = lifetime->binding; binding != nullptr) {
      binding->applyConstraints(surfaceId, constraints);
    }
  }
}

void LoomBinding::setPointScaleFactor(float pointScaleFactor) {
  {
    std::lock_guard lock(registryMutex());
    directPointScaleFactorStorage() =
        pointScaleFactor > 0 ? pointScaleFactor : 1.0f;
  }
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    auto* binding = lifetime->binding;
    if (binding == nullptr) {
      continue;
    }
    // `renderers_` is UI-thread state, but the host reports a new density on
    // whichever thread noticed it, so the propagation runs through the platform
    // UI executor. The lifetime token keeps the binding alive until it runs.
    auto uiExecutor = binding->host()->uiExecutor();
    if (!uiExecutor) {
      binding->applyPointScaleFactor(pointScaleFactor);
      continue;
    }
    uiExecutor([lifetime, pointScaleFactor] {
      std::shared_lock held(lifetime->gate);
      if (auto* binding = lifetime->binding; binding != nullptr) {
        binding->applyPointScaleFactor(pointScaleFactor);
      }
    });
  }
}

void LoomBinding::applyPointScaleFactor(float pointScaleFactor) {
#if LOOM_WORKER
  // Density and font scale changes can change what the platform text path
  // supports; this is the UI thread, where the answer may be asked.
  refreshCanMeasureTextOnWorker();
  std::vector<std::pair<SurfaceId, std::shared_ptr<LoomRenderer>>> owners;
  {
    std::lock_guard state(stateMutex_);
    owners.assign(renderers_.begin(), renderers_.end());
  }
  for (auto& entry : owners) {
    auto owner = entry.second;
    submitWorkerOperation(
        tokenFor(entry.first), [owner, pointScaleFactor] {
          // The host republishes the scale on every surface size update, and
          // reflowing unconditionally re-parsed text and re-ran Yoga for the
          // whole tree even when the scale had not changed.
          if (owner->setPointScaleFactor(pointScaleFactor)) {
            owner->reflow();
          }
        });
  }
#else
  for (auto& entry : renderers_) {
    entry.second->setPointScaleFactor(pointScaleFactor);
  }
#endif
}

void LoomBinding::submitOrderedUIEffect(
    SurfaceId surfaceId,
    std::function<void()> effect) {
#if LOOM_WORKER
  auto job = std::make_unique<LoomWorker::Job>();
  // `run()` always invokes `compute`, so it cannot be left empty; the work
  // itself belongs on the UI thread, after the in-flight result has been
  // applied, which is where `applied` runs.
  job->compute = [] {};
  job->applied = std::move(effect);
  submitWorkerJob(tokenFor(surfaceId), std::move(job));
#else
  (void)surfaceId;
  effect();
#endif
}

void LoomBinding::synchronizeWorkerForUIManager(
    UIManager& manager) {
#if LOOM_WORKER
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    if (auto* binding = lifetime->binding;
        binding && &binding->getUIManager() == &manager) {
      binding->worker_->synchronize();
    }
  }
#else
  (void)manager;
#endif
}

void LoomBinding::shutdownWorkerForUIManager(UIManager& manager) {
#if LOOM_WORKER
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    if (auto* binding = lifetime->binding;
        binding && &binding->getUIManager() == &manager) {
      for (auto& [surface, token] : binding->surfaceTokens_)
        token->active = false;
      binding->worker_->shutdown();
    }
  }
#else
  (void)manager;
#endif
}

namespace {

/**
 * The renderer as a layout-animation driver sees it. Layout reads are only
 * answered inside `processBatch` (`inCommit`), where the renderer matches the
 * batch being mounted.
 */
class RendererGeometry final : public LoomLayoutAnimationGeometry {
 public:
  RendererGeometry(const LoomRenderer& renderer, bool inCommit)
      : renderer_(renderer), inCommit_(inCommit) {}

  std::optional<Rect> layoutFrameInSurface(Tag tag) const override {
    LoomRenderer::Metrics metrics;
    if (!inCommit_ ||
        !renderer_.metricsRelativeTo(
            tag, 0, metrics, /*includeTransform*/ false,
            /*includeScroll*/ false)) {
      return std::nullopt;
    }
    return Rect{{metrics.x, metrics.y}, {metrics.width, metrics.height}};
  }

  Tag parentOf(Tag tag) const override {
    if (!inCommit_) {
      return 0;
    }
    const auto info = renderer_.fabricNodeInfo(tag);
    return info.has_value() ? info->parent : 0;
  }

  size_t mountedChildCount(Tag tag) const override {
    return inCommit_ ? renderer_.mountedChildCount(tag) : 0;
  }

  const std::string* componentNameOf(Tag tag) const override {
    if (!inCommit_) {
      return nullptr;
    }
    const auto info = renderer_.fabricNodeInfo(tag);
    return info.has_value() ? info->componentName : nullptr;
  }

  const folly::dynamic* propsOf(Tag tag) const override {
    if (!inCommit_) {
      return nullptr;
    }
    const auto info = renderer_.fabricNodeInfo(tag);
    return info.has_value() ? info->props : nullptr;
  }

  std::optional<Point> nativeOriginInSurface(Tag tag) const override {
    const auto location = renderer_.viewLocationInSurface(tag);
    if (!location.has_value() || std::isnan(location->first) ||
        std::isnan(location->second)) {
      return std::nullopt;
    }
    return Point{location->first, location->second};
  }

 private:
  const LoomRenderer& renderer_;
  const bool inCommit_;
};

} // namespace

bool LoomBinding::layoutAnimationFrame(SurfaceId surfaceId) {
  auto driver = loomLayoutAnimationDriver(surfaceId);
  if (driver == nullptr) {
    return false;
  }
  return withSurfaceRenderer(
      surfaceId,
      [&](LoomBinding& binding,
          const std::shared_ptr<LoomRenderer>& liveRenderer) {
        LoomMountBatch batch;
        batch.animationFrame = true;
        const RendererGeometry geometry(*liveRenderer, /*inCommit*/ false);
        driver->frame(surfaceId, batch, geometry);
        if (batch.operations.empty()) {
          return;
        }
        binding.host()->applyBatch(surfaceId, std::move(batch));
      });
}

bool LoomBinding::withSurfaceRenderer(
    SurfaceId surfaceId,
    const std::function<
        void(LoomBinding&, const std::shared_ptr<LoomRenderer>&)>& body) {
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    auto* binding = lifetime->binding;
    if (binding == nullptr) {
      continue;
    }
    auto liveRenderer = binding->findRenderer(surfaceId);
    if (liveRenderer == nullptr) {
      continue;
    }
    body(*binding, liveRenderer);
    return true;
  }
  return false;
}

void LoomBinding::recordNativeProps(Tag tag, folly::dynamic props) {
  recordNativePropsImpl(tag, std::move(props), false);
}

void LoomBinding::recordNativePaintProps(Tag tag, folly::dynamic paint) {
  recordNativePropsImpl(tag, std::move(paint), true);
}

void LoomBinding::recordAnimationProps(
    Tag tag,
    folly::dynamic props,
    bool viewApplied) {
  recordNativePropsImpl(tag, std::move(props), viewApplied, viewApplied);
}

void LoomBinding::recordNativePropsImpl(
    Tag tag,
    folly::dynamic props,
    bool knownPaint,
    bool viewApplied) {
#if LOOM_WORKER
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    auto* binding = lifetime->binding;
    if (!binding ||
        (binding->replayingNativePropsTag_ == tag &&
         binding->replayingNativeProps_ &&
         props == *binding->replayingNativeProps_))
      continue;
    // The UI thread reaches this while JS may be writing the same maps.
    std::lock_guard state(binding->stateMutex_);
    auto tokenIt = binding->mountedTokens_.find(tag);
    if (tokenIt == binding->mountedTokens_.end() || !tokenIt->second->active)
      continue;
    auto token = tokenIt->second;
    auto owner = binding->renderers_.find(token->surfaceId);
    if (owner == binding->renderers_.end())
      continue;
    // A non-throwing lookup on purpose. An overlay can outlive the identity it
    // was staged for, and the previous `mountedIdentities_.at(tag)` threw
    // `std::out_of_range` out of a JNI frame (`std::terminate`) when the two
    // maps disagreed (design §2, candidate C1). A tag with no identity is
    // simply dropped.
    auto identityIt = binding->mountedIdentities_.find(tag);
    if (identityIt == binding->mountedIdentities_.end())
      continue;
    ClassifiedAnimatedProps classified;
    bool isText = owner->second->isTextNode(tag);
    if (knownPaint) {
      if (!isText)
        classified.paint = std::move(props);
    } else {
      classified = classifyAnimatedProps(props);
      isText = isText || classified.text;
    }
    if (isText) {
      // Font and text keys drive measurement, and any key on a paragraph or raw
      // text node can, which the synchronous lane must not do: the whole payload
      // keeps the ordinary per-call path (design §3.2, "Text").
      binding->stageNativeProps(tag, identityIt->second, token, std::move(props));
      return;
    }
    // The platform's synchronous update has already put these values on the
    // view, or (`!viewApplied`) the lane writes them itself.
    binding->writeAnimatedProps(
        token->surfaceId,
        tag,
        identityIt->second,
        std::move(classified),
        /*applyPaintToView=*/!viewApplied,
        /*viewAlreadyApplied=*/viewApplied);
    return;
  }
#else
  (void)tag;
  (void)props;
  (void)knownPaint;
#endif
}

#if LOOM_WORKER
std::shared_ptr<const folly::dynamic> LoomBinding::stageNativeProps(
    Tag tag,
    uint64_t identity,
    std::shared_ptr<SurfaceToken> token,
    folly::dynamic props) {
  std::unique_lock state(stateMutex_);
  auto owner = renderers_.find(token->surfaceId);
  if (owner == renderers_.end() || !token->active)
    return nullptr;
  auto payload = std::make_shared<const folly::dynamic>(std::move(props));
  auto sequence = ++nativePropsSequence_;
  // Read the surface before the token is handed to the worker: the operation
  // takes it by value, so `token` is empty from here on while the completion
  // callback below still needs the id.
  const auto surfaceId = token->surfaceId;
  auto& overlay = nativePropsOverlays_[surfaceId][tag];
  if (overlay.identity != identity)
    overlay.props = folly::dynamic::object();
  for (const auto& item : payload->items())
    overlay.props[item.first] = item.second;
  overlay.surfaceId = surfaceId;
  overlay.identity = identity;
  overlay.sequence = sequence;
  auto rendererOwner = owner->second;
  state.unlock();
  submitWorkerOperation(
      std::move(token),
      [rendererOwner, tag, identity, payload] {
        if (rendererOwner->identityOf(tag) == identity)
          rendererOwner->setNativeProps(tag, *payload);
      },
      [this, surfaceId, tag, sequence] {
        std::lock_guard state(stateMutex_);
        auto surfaceIt = nativePropsOverlays_.find(surfaceId);
        if (surfaceIt == nativePropsOverlays_.end())
          return;
        auto it = surfaceIt->second.find(tag);
        if (it != surfaceIt->second.end() && it->second.sequence == sequence)
          surfaceIt->second.erase(it);
      });
  return payload;
}

// ---------------------------------------------------------------------------
// Animation fast lane.
//
// Every animated prop write lands here instead of becoming its own worker job:
// writes are coalesced per frame and per tag, paint keys reach the native view
// immediately, and one flush per frame either borrows the renderer while the
// worker is idle or joins the single animation job the surface may have queued.
// ---------------------------------------------------------------------------

namespace {
// The synchronous lane may borrow the renderer only while its measured cost
// stays inside the frame budget (design §5.7). 2 ms is the documented default
// for a 120 Hz display; a surface that exceeds it for three frames in a row
// hands its layout work to the coalesced async job instead, and returns to the
// synchronous lane only after a probe measures it calm again.
constexpr double kAnimationLaneBudgetMicros = 2000.0;
constexpr int kAnimationLaneOverBudgetFrames = 3;
constexpr int kAnimationLaneProbeFrames = 30;

// The patches are consumed: a tag with nothing merged yet (the common case, one
// write per node per frame) takes the patch whole instead of copying it key by
// key.
void mergePatchesInto(
    TagMap<folly::dynamic>& target,
    std::vector<std::pair<Tag, folly::dynamic>>& patches) {
  for (auto& entry : patches) {
    auto [it, inserted] = target.try_emplace(entry.first);
    auto& merged = it->second;
    if (inserted && entry.second.isObject()) {
      merged = std::move(entry.second);
      continue;
    }
    if (!merged.isObject())
      merged = folly::dynamic::object();
    for (auto& item : entry.second.items())
      merged[item.first] = std::move(item.second);
  }
}
} // namespace

bool LoomBinding::AnimationFrame::merge(
    std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& viewPatches) {
  std::lock_guard lock(mutex);
  if (claimed)
    return false;
  mergePatchesInto(paint, paintPatches);
  mergePatchesInto(layout, layoutPatches);
  mergePatchesInto(view, viewPatches);
  // The tags are recorded as they arrive, not when the frame is taken: the
  // applied callback that drops their paint overlays can run before the compute
  // does (a synchronous job applies first, then computes).
  for (const auto& entry : paintPatches)
    tags.push_back(entry.first);
  for (const auto& entry : layoutPatches)
    tags.push_back(entry.first);
  return true;
}

bool LoomBinding::AnimationFrame::take(
    std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& viewPatches) {
  std::lock_guard lock(mutex);
  if (claimed)
    return false;
  claimed = true;
  paintPatches.reserve(paint.size());
  for (auto& entry : paint) {
    paintPatches.emplace_back(entry.first, std::move(entry.second));
  }
  layoutPatches.reserve(layout.size());
  for (auto& entry : layout) {
    layoutPatches.emplace_back(entry.first, std::move(entry.second));
  }
  viewPatches.reserve(view.size());
  for (auto& entry : view) {
    viewPatches.emplace_back(entry.first, std::move(entry.second));
  }
  return true;
}

void LoomBinding::writeAnimatedProps(
    SurfaceId surfaceId,
    Tag tag,
    uint64_t identity,
    ClassifiedAnimatedProps classified,
    bool applyPaintToView,
    bool viewAlreadyApplied) {
  std::unique_lock state(stateMutex_);
  auto& write = animationFrameBuffer_[surfaceId][tag];
  if (write.identity != identity) {
    write.view = folly::dynamic::object();
    // A different draft owns the tag now, so nothing collected for the previous
    // one may survive into this frame.
    write.paint = folly::dynamic::object();
    write.layout = folly::dynamic::object();
    write.text = false;
    write.identity = identity;
  }
  // Paint that no view has seen yet is written to it when the frame is
  // applied. A write the platform already applied (Native Animated) or that
  // this call applies right now is not repeated: re-sending it cost a second
  // dynamic -> NativeMap -> Java round trip per node per frame.
  if (!applyPaintToView && !viewAlreadyApplied) {
    for (const auto& item : classified.paint.items())
      write.view[item.first] = item.second;
  }
  for (const auto& item : classified.layout.items()) {
    write.layout[item.first] = item.second;
  }
  write.text = write.text || classified.text;
  if (classified.paint.empty()) {
    return;
  }
  // Paint keys are the part a later batch could revert, so they stay as the
  // replayable overlay until the frame that carries them is folded. `sequence`
  // stays 0: no staged operation owns this entry, the lane erases it itself.
  auto& overlay = nativePropsOverlays_[surfaceId][tag];
  // `props` must be an object before it is indexed: a freshly inserted entry is
  // value-initialized with a default member initializer, but this also covers a
  // stale entry left behind by an older version of the struct.
  if (overlay.identity != identity || !overlay.props.isObject()) {
    overlay.props = folly::dynamic::object();
  }
  if (overlay.props.empty()) {
    // The common case (the previous frame's overlay was folded and erased): a
    // whole-object copy reuses the stored key hashes instead of hashing and
    // inserting every key again.
    overlay.props = classified.paint;
  } else {
    for (const auto& item : classified.paint.items()) {
      overlay.props[item.first] = item.second;
    }
  }
  overlay.surfaceId = surfaceId;
  overlay.identity = identity;
  overlay.sequence = 0;
  // The frame's copy is the last use of the classified keys unless they are
  // about to be written to the view below.
  if (write.paint.empty() && !applyPaintToView) {
    write.paint = std::move(classified.paint);
  } else {
    for (const auto& item : classified.paint.items())
      write.paint[item.first] = item.second;
  }
  // The view write is a Java call that re-enters `recordNativeProps`.
  state.unlock();
  if (applyPaintToView) {
    applyAnimatedPaintToView(tag, classified.paint);
  }
}

void LoomBinding::applyAnimatedPaintToView(
    Tag tag,
    const folly::dynamic& props) {
  if (props.empty()) {
    return;
  }
  const auto previousTag = replayingNativePropsTag_;
  const auto* previousProps = replayingNativeProps_;
  replayingNativePropsTag_ = tag;
  replayingNativeProps_ = &props;
  try {
    host_->updateViewProps(tag, props);
  } catch (...) {
    replayingNativePropsTag_ = previousTag;
    replayingNativeProps_ = previousProps;
    throw;
  }
  replayingNativePropsTag_ = previousTag;
  replayingNativeProps_ = previousProps;
}

void LoomBinding::erasePaintOverlays(
    SurfaceId surfaceId,
    const std::vector<Tag>& tags) {
  std::lock_guard state(stateMutex_);
  auto surfaceIt = nativePropsOverlays_.find(surfaceId);
  if (surfaceIt == nativePropsOverlays_.end()) {
    return;
  }
  for (const auto tag : tags) {
    auto it = surfaceIt->second.find(tag);
    // Only entries this lane owns (`sequence == 0`); a staged operation's entry
    // is erased by its own completion callback.
    if (it != surfaceIt->second.end() && it->second.sequence == 0) {
      surfaceIt->second.erase(it);
    }
  }
}

void LoomBinding::recordAnimationLaneCost(
    SurfaceId surfaceId,
    double micros) {
  std::lock_guard state(stateMutex_);
  auto& stats = animationLaneStats_[surfaceId];
  stats.emaMicros =
      stats.emaMicros <= 0 ? micros : stats.emaMicros * 0.8 + micros * 0.2;
  if (stats.asyncMode) {
    // Only a probe reports a cost while the surface is in the async lane; the
    // measurement is what decides whether it may come back.
    if (stats.emaMicros < kAnimationLaneBudgetMicros / 2) {
      stats.asyncMode = false;
      stats.overBudgetFrames = 0;
    }
    return;
  }
  if (stats.emaMicros > kAnimationLaneBudgetMicros) {
    if (++stats.overBudgetFrames >= kAnimationLaneOverBudgetFrames) {
      stats.asyncMode = true;
      stats.overBudgetFrames = 0;
    }
    return;
  }
  stats.overBudgetFrames = 0;
}

bool LoomBinding::animationLaneAllowsSync(SurfaceId surfaceId) {
  std::lock_guard state(stateMutex_);
  const auto iterator = animationLaneStats_.find(surfaceId);
  if (iterator == animationLaneStats_.end() || !iterator->second.asyncMode) {
    return true;
  }
  // One probe every `kAnimationLaneProbeFrames` frames measures whether the
  // surface has calmed down; without a measurement there is no way back.
  return ++iterator->second.asyncProbeCounter % kAnimationLaneProbeFrames == 0;
}

void LoomBinding::traceAnimationLane(
    SurfaceId surfaceId,
    bool synchronous,
    double micros,
    size_t nodes) {
#if LOOM_PROFILE
  if (!LoomWorkerProfile::enabled()) {
    return;
  }
  loomLog(
      LoomLogLevel::Profile,
      "LoomAnimLane",
      "sync=%d cost=%.0fus nodes=%lu surface=%d",
      synchronous ? 1 : 0,
      micros,
      static_cast<unsigned long>(nodes),
      static_cast<int>(surfaceId));
#else
  (void)surfaceId;
  (void)synchronous;
  (void)micros;
  (void)nodes;
#endif
}

void LoomBinding::flushAnimationFrames() {
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    auto* binding = lifetime->binding;
    if (binding == nullptr) {
      continue;
    }
    std::vector<SurfaceId> surfaceIds;
    {
      std::lock_guard state(binding->stateMutex_);
      surfaceIds.reserve(binding->animationFrameBuffer_.size());
      for (const auto& entry : binding->animationFrameBuffer_) {
        surfaceIds.push_back(entry.first);
      }
    }
    for (const auto surfaceId : surfaceIds) {
      binding->flushAnimationFrame(surfaceId);
    }
  }
}

void LoomBinding::flushAnimationFrame(SurfaceId surfaceId) {
  std::unique_lock state(stateMutex_);
  auto bufferIt = animationFrameBuffer_.find(surfaceId);
  if (bufferIt == animationFrameBuffer_.end() || bufferIt->second.empty()) {
    return;
  }
  // Take the writes before applying anything: folding paint props re-enters this
  // map through the synchronous-update delegate, and those nested writes belong
  // to the next frame, not this one.
  auto writes = std::move(bufferIt->second);
  animationFrameBuffer_.erase(bufferIt);
  std::vector<std::pair<Tag, folly::dynamic>> paintPatches;
  std::vector<std::pair<Tag, folly::dynamic>> layoutPatches;
  std::vector<std::pair<Tag, folly::dynamic>> viewPatches;
  bool needsAsync = false;
  for (auto& entry : writes) {
    auto& write = entry.second;
    const auto mounted = mountedIdentities_.find(entry.first);
    if (mounted == mountedIdentities_.end() ||
        mounted->second != write.identity) {
      continue; // The node is gone or the tag was reused: this write is stale.
    }
    needsAsync = needsAsync || write.text;
    if (!write.paint.empty()) {
      paintPatches.emplace_back(entry.first, std::move(write.paint));
    }
    if (!write.layout.empty()) {
      layoutPatches.emplace_back(entry.first, std::move(write.layout));
    }
    if (!write.view.empty()) {
      viewPatches.emplace_back(entry.first, std::move(write.view));
    }
  }
  state.unlock();
  if (paintPatches.empty() && layoutPatches.empty()) {
    return;
  }
  deferAnimationFrame(
      surfaceId, paintPatches, layoutPatches, viewPatches, needsAsync);
}

void LoomBinding::deferAnimationFrame(
    SurfaceId surfaceId,
    std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& viewPatches,
    bool needsAsync) {
  std::unique_lock state(stateMutex_);
  if (auto pending = pendingAnimationFrames_.find(surfaceId);
      pending != pendingAnimationFrames_.end()) {
    // The queued job has not started, so the newer frame replaces its payload
    // instead of appending a second job (design §5.4).
    if (pending->second->merge(paintPatches, layoutPatches, viewPatches)) {
      return;
    }
  }
  auto rendererIt = renderers_.find(surfaceId);
  if (rendererIt == renderers_.end()) {
    return;
  }
  auto rendererOwner = rendererIt->second;
  auto tokenIt = surfaceTokens_.find(surfaceId);
  if (tokenIt == surfaceTokens_.end() || !tokenIt->second->active) {
    return;
  }
  auto token = tokenIt->second;
  // Only a frame that has to run Yoga (or text measurement) is subject to the
  // adaptive budget: a paint-only frame is a merge plus an immediate view write
  // either way, so it always takes the synchronous path when the worker is idle
  // (design §5.2 and §5.7).
  const bool hasLayoutOrText = !layoutPatches.empty() || needsAsync;
  const bool synchronous =
      !hasLayoutOrText || animationLaneAllowsSync(surfaceId);
  auto frame = std::make_shared<AnimationFrame>();
  frame->merge(paintPatches, layoutPatches, viewPatches);
  // With JS off the UI thread `writeAnimatedProps` cannot write paint keys to
  // the views as they arrive, so the frame writes them when it is taken (at
  // compute time, so a frame merged into a pending job writes its latest
  // values). A synchronous job computes on the UI thread and writes there; an
  // async one computes on the worker and leaves the write to `applied`, which
  // for async jobs runs on the UI thread after compute. (For synchronous jobs
  // `applied` runs before compute, so it cannot do the write itself.)
  auto paintForView = LoomWorker::kJavaScriptOnUIThread
      ? nullptr
      : std::make_shared<std::vector<std::pair<Tag, folly::dynamic>>>();
  auto job = std::make_unique<LoomWorker::Job>();
  job->synchronous = synchronous;
  job->compute = [this, rendererOwner, frame, paintForView] {
    std::vector<std::pair<Tag, folly::dynamic>> framePaint;
    std::vector<std::pair<Tag, folly::dynamic>> frameLayout;
    std::vector<std::pair<Tag, folly::dynamic>> frameView;
    if (!frame->take(framePaint, frameLayout, frameView)) {
      return;
    }
    if (paintForView) {
      if (worker_->onUIThread()) {
        for (const auto& [tag, paint] : frameView) {
          applyAnimatedPaintToView(tag, paint);
        }
      } else {
        *paintForView = std::move(frameView);
      }
    }
    rendererOwner->applyAnimationFrame(framePaint, frameLayout);
  };
  job->applied = [this, surfaceId, frame, paintForView] {
    if (paintForView) {
      for (const auto& [tag, paint] : *paintForView) {
        applyAnimatedPaintToView(tag, paint);
      }
    }
    std::vector<Tag> tags;
    {
      std::lock_guard lock(frame->mutex);
      tags = frame->tags;
    }
    erasePaintOverlays(surfaceId, tags);
    std::lock_guard state(stateMutex_);
    if (auto pending = pendingAnimationFrames_.find(surfaceId);
        pending != pendingAnimationFrames_.end() && pending->second == frame) {
      pendingAnimationFrames_.erase(pending);
    }
  };
  pendingAnimationFrames_[surfaceId] = frame;
  // A synchronous job may apply right here and call back into this binding.
  state.unlock();
  if (!synchronous) {
    submitWorkerJob(token, std::move(job));
    return;
  }
  const auto start = LoomWorkerProfile::Clock::now();
  bool ranSynchronously = false;
  submitWorkerJob(token, std::move(job), &ranSynchronously);
  if (!ranSynchronously) {
    // The worker was busy, so this frame is the pending job now: its cost is
    // measured by whoever ends up running it, not here.
    return;
  }
  const auto micros = LoomWorkerProfile::ms(
                         start, LoomWorkerProfile::Clock::now()) *
      1000.0;
  recordAnimationLaneCost(surfaceId, micros);
  traceAnimationLane(
      surfaceId, true, micros, paintPatches.size() + layoutPatches.size());
}

void LoomBinding::setWorkerNativeProps(
    const NodeRef& ref,
    folly::dynamic props) {
  if (!ref.draft || !ref.surfaceToken->active)
    return;
  // View writes are Java calls that only the UI thread may make. Off the UI
  // thread the props still reach the view: the animation frame is flushed and
  // applied there, and a staged text/layout write replays on the UI thread.
  const bool onUIThread = worker_->onUIThread();
  std::unique_lock state(stateMutex_);
  const auto mounted = mountedIdentities_.find(ref.tag);
  const bool isMounted = mounted != mountedIdentities_.end() &&
      mounted->second == ref.draft->identity;
  bool isSubmitted = false;
  if (auto surfaceIdentities = submittedIdentities_.find(ref.surfaceId);
      surfaceIdentities != submittedIdentities_.end()) {
    const auto submitted = surfaceIdentities->second.find(ref.tag);
    isSubmitted = submitted != surfaceIdentities->second.end() &&
        submitted->second == ref.draft->identity;
  }
  if (!isMounted && !isSubmitted)
    return;
  if (isMounted) {
    const auto rendererIt = renderers_.find(ref.surfaceId);
    auto classified = classifyAnimatedProps(props);
    if (!classified.text &&
        (rendererIt == renderers_.end() ||
         !rendererIt->second->isTextNode(ref.tag))) {
      // Animation fast lane (design §5.1): the write joins the frame being
      // collected and its paint keys are on the view before this call returns.
      state.unlock();
      writeAnimatedProps(
          ref.surfaceId,
          ref.tag,
          ref.draft->identity,
          std::move(classified),
          /*applyPaintToView=*/onUIThread);
      return;
    }
  }
  state.unlock();
  auto payload = stageNativeProps(
      ref.tag, ref.draft->identity, ref.surfaceToken, std::move(props));
  if (!payload || !isMounted || !onUIThread)
    return; // First mount replays the overlay before any layout events.
  const auto previousTag = replayingNativePropsTag_;
  const auto* previousProps = replayingNativeProps_;
  replayingNativePropsTag_ = ref.tag;
  replayingNativeProps_ = payload.get();
  try {
    host_->updateViewProps(
        ref.tag, *payload);
  } catch (...) {
    replayingNativePropsTag_ = previousTag;
    replayingNativeProps_ = previousProps;
    throw;
  }
  replayingNativePropsTag_ = previousTag;
  replayingNativeProps_ = previousProps;
}
#endif

bool LoomBinding::dispatchComponentState(const StateUpdate& stateUpdate) {
  if (stateUpdate.family == nullptr) {
    return false;
  }
  const auto surfaceId = stateUpdate.family->getSurfaceId();
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    auto* binding = lifetime->binding;
    if (binding != nullptr && binding->submitComponentState(surfaceId, stateUpdate)) {
      return true;
    }
  }
  return false;
}

bool LoomBinding::submitComponentState(
    SurfaceId surfaceId,
    const StateUpdate& update) {
  std::unique_lock state(stateMutex_);
  auto rendererIt = renderers_.find(surfaceId);
  if (rendererIt == renderers_.end()) {
    return false;
  }
  auto renderer = rendererIt->second;
  auto tokenIt = surfaceTokens_.find(surfaceId);
  if (tokenIt == surfaceTokens_.end() || !tokenIt->second->active) {
    // The surface is Loom's but already stopping: the update has no target.
    return true;
  }
  auto token = tokenIt->second;
  state.unlock();
#if LOOM_WORKER
  // Always queued, never run on the calling thread: `unstable_Immediate`
  // updates arrive from inside UIKit layout and from mounting, where applying
  // a batch synchronously would re-enter the batch applier.
  auto job = std::make_unique<LoomWorker::Job>();
  job->compute = [renderer, update] { renderer->applyComponentState(update); };
  submitWorkerJob(token, std::move(job));
#else
  renderer->applyComponentState(update);
#endif
  return true;
}

void LoomBinding::releaseSurface(SurfaceId surfaceId) {
  {
    std::lock_guard lock(registryMutex());
    surfaceConstraints().erase(surfaceId);
  }

  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    if (auto* binding = lifetime->binding; binding != nullptr) {
      binding->releaseSurfaceInternal(surfaceId);
    }
  }
}

void LoomBinding::applyConstraints(
    SurfaceId surfaceId,
    const SurfaceConstraints& constraints) {
  std::unique_lock state(stateMutex_);
  auto iterator = renderers_.find(surfaceId);
  if (iterator == renderers_.end()) {
    // Nothing is mounted for this surface yet. `renderer()` picks these up when
    // it creates the renderer, so there is nothing to reflow.
    return;
  }
#if LOOM_WORKER
  auto owner = iterator->second;
  state.unlock();
  submitWorkerOperation(tokenFor(surfaceId), [owner, constraints] {
    owner->setConstraints(
        constraints.minWidth,
        constraints.minHeight,
        constraints.maxWidth,
        constraints.maxHeight,
        constraints.isRTL);
  });
  return;
#else
  // `setConstraints` reflows on its own when the viewport actually changed.
  iterator->second->setConstraints(
      constraints.minWidth,
      constraints.minHeight,
      constraints.maxWidth,
      constraints.maxHeight,
      constraints.isRTL);
#endif
}

void LoomBinding::releaseSurfaceInternal(SurfaceId surfaceId) {
  {
    std::lock_guard families(fabricFamiliesMutex_);
    fabricFamilies_.erase(surfaceId);
  }
  std::lock_guard state(stateMutex_);
  if (auto token = surfaceTokens_.find(surfaceId);
      token != surfaceTokens_.end()) {
    token->second->active = false;
    surfaceTokens_.erase(token);
  }
#if LOOM_WORKER
  if (auto overlays = nativePropsOverlays_.find(surfaceId);
      overlays != nativePropsOverlays_.end()) {
    for (const auto& entry : overlays->second) {
      mountedIdentities_.erase(entry.first);
    }
    nativePropsOverlays_.erase(overlays);
  }
#endif
#if LOOM_WORKER
  for (auto it = mountedTokens_.begin(); it != mountedTokens_.end();) {
    if (it->second->surfaceId == surfaceId) {
      mountedIdentities_.erase(it->first);
      it = mountedTokens_.erase(it);
    } else
      ++it;
  }
#endif
#if LOOM_WORKER
  submittedIdentities_.erase(surfaceId);
  // Animation fast lane state is per surface: a frame still being collected, the
  // single queued animation job and its adaptive cost history all die with it.
  animationFrameBuffer_.erase(surfaceId);
  pendingAnimationFrames_.erase(surfaceId);
  animationLaneStats_.erase(surfaceId);
#endif
  ++surfaceGenerations_[surfaceId];
  renderers_.erase(surfaceId);
  pendingRootCommits_.erase(surfaceId);
  pendingDocumentInstanceHandles_.erase(surfaceId);
  documentInstanceHandles_.erase(surfaceId);
  LoomEventManager::clearSurface(surfaceId);
}

LoomBinding::SurfaceConstraints
LoomBinding::constraintsFor(SurfaceId surfaceId) const {
  std::lock_guard lock(registryMutex());
  auto iterator = surfaceConstraints().find(surfaceId);
  return iterator == surfaceConstraints().end() ? SurfaceConstraints{}
                                                : iterator->second;
}

#pragma mark - Renderer

LoomRenderer& LoomBinding::renderer(SurfaceId surfaceId) {
  {
    std::lock_guard state(stateMutex_);
    auto iterator = renderers_.find(surfaceId);
    if (iterator != renderers_.end()) {
      return *iterator->second;
    }
  }

  auto rendererToken = tokenFor(surfaceId);
  const std::weak_ptr<SurfaceToken> weakRendererToken = rendererToken;

  LoomRenderer::NativeOperations operations;
  operations.prepareTextForMountProvidesLayout =
      LoomPlatformTraits::current().hasPreparedTextLayout;
  operations.isSurfaceActive = [weakRendererToken](SurfaceId surface) {
    auto token = weakRendererToken.lock();
    return token != nullptr && token->active && token->surfaceId == surface;
  };
  operations.failSurface = [this, weakRendererToken](
                               SurfaceId surface, const std::string& reason) {
    auto token = weakRendererToken.lock();
    if (token == nullptr || !token->active || token->surfaceId != surface) {
      return;
    }
    host_->failSurface(surface, reason);
    token->active = false;
  };
  operations.applyBatch = [this](
                              SurfaceId surface,
                              LoomMountBatch batch) {
    // Layout animations (Reanimated adapter): the driver sees the batch the
    // platform is about to mount. Without a driver this is one atomic load.
    if (auto driver = loomLayoutAnimationDriver(surface)) {
      if (auto liveRenderer = findRenderer(surface)) {
        const RendererGeometry geometry(*liveRenderer, /*inCommit*/ true);
        driver->processBatch(surface, batch, geometry);
      }
    }
    host_->applyBatch(surface, std::move(batch));
#if LOOM_WORKER
    // Only this surface's overlays can be replayed onto this surface, so the
    // scan is bounded by them rather than by every overlay on every surface.
    std::vector<Tag> overlayTags;
    {
      std::lock_guard state(stateMutex_);
      auto surfaceOverlays = nativePropsOverlays_.find(surface);
      if (surfaceOverlays != nativePropsOverlays_.end()) {
        overlayTags.reserve(surfaceOverlays->second.size());
        for (const auto& [tag, overlay] : surfaceOverlays->second) {
          if (overlay.surfaceId == surface)
            overlayTags.push_back(tag);
        }
      }
    }
    for (const auto tag : overlayTags) {
      // Re-looked up rather than stored: applying native props can emit events
      // and reenter this map, which may stage a distinct nested delta.
      folly::dynamic props;
      {
        std::lock_guard state(stateMutex_);
        auto surfaceOverlays = nativePropsOverlays_.find(surface);
        if (surfaceOverlays == nativePropsOverlays_.end())
          break;
        auto it = surfaceOverlays->second.find(tag);
        if (it == surfaceOverlays->second.end() ||
            mountedIdentities_[tag] != it->second.identity)
          continue;
        // Native prop application can emit events and reenter this map. Keep
        // this replay stable while allowing a distinct nested delta to stage.
        props = it->second.props;
      }
      const auto previousTag = replayingNativePropsTag_;
      const auto* previousProps = replayingNativeProps_;
      replayingNativePropsTag_ = tag;
      replayingNativeProps_ = &props;
      try {
        host_->updateViewProps(
            tag, props);
      } catch (...) {
        replayingNativePropsTag_ = previousTag;
        replayingNativeProps_ = previousProps;
        throw;
      }
      replayingNativePropsTag_ = previousTag;
      replayingNativeProps_ = previousProps;
    }
#endif
  };
  operations.registerEventTarget =
      [this](SurfaceId surface, Tag tag, InstanceHandle::Shared handle) {
        host_->registerEventTarget(
            surface, tag, std::move(handle));
      };
  operations.unregisterEventTarget = [this](SurfaceId surface, Tag tag) {
    host_->unregisterEventTarget(surface, tag);
  };
  operations.dispatchEvent = [this](
                                 SurfaceId surface,
                                 Tag tag,
                                 const std::string& eventName,
                                 folly::dynamic payload,
                                 RawEvent::Category category,
                                 bool unique) {
    host_->dispatchEvent(
        surface, tag, eventName, std::move(payload), category, unique);
  };
  operations
      .parseTextAttributes = [this](
                                 SurfaceId surface,
                                 const std::string& componentName,
                                 const folly::dynamic& props,
                                 const TextAttributes& inherited,
                                 ParagraphAttributes* outParagraphAttributes) {
    auto paragraphAttributes = ParagraphAttributes{};
    auto* t = text();
    auto attributes = t == nullptr
        ? inherited
        : t->parseTextAttributes(
              surface, componentName, props, inherited, paragraphAttributes);
    if (outParagraphAttributes != nullptr) {
      *outParagraphAttributes = std::move(paragraphAttributes);
    }
    return attributes;
  };
  operations.measureText = [this](
                               SurfaceId surface,
                               const AttributedString& attributedString,
                               const ParagraphAttributes& paragraphAttributes,
                               const Size& minimumSize,
                               const Size& maximumSize) {
    LoomRenderer::TextMeasurement measurement;
    auto* t = text();
    if (t == nullptr) {
      return measurement;
    }

    // Measured by the platform's own text engine, through the same
    // manager the shadow-tree path uses, so text is laid out for real
    // instead of estimated.
    LoomWorkerProfile::TextScope textProfile;
    auto size = t->measureText(
        surface,
        attributedString,
        paragraphAttributes,
        minimumSize,
        maximumSize);
    if (!size.has_value()) {
      return measurement;
    }

    measurement.valid = true;
    measurement.width = size->width;
    measurement.height = size->height;
    return measurement;
  };
  operations.prepareText = [this](
                               SurfaceId surface,
                               const AttributedString& attributedString,
                               const ParagraphAttributes& paragraphAttributes,
                               const Size& minimumSize,
                               const Size& maximumSize) {
    auto* t = text();
    if (t == nullptr) {
      return LoomTextMeasurement{};
    }
    LoomWorkerProfile::TextScope textProfile;
    auto measurement = t->prepareText(
        surface,
        attributedString,
        paragraphAttributes,
        minimumSize,
        maximumSize);
    if (measurement.valid) {
      measurement.minimumSize = minimumSize;
      measurement.maximumSize = maximumSize;
    }
    return measurement;
  };
  operations.prepareTextForMount =
      [this](
          SurfaceId surface,
          const AttributedString& attributedString,
          const ParagraphAttributes& paragraphAttributes,
          const Size& minimumSize,
          const Size& maximumSize) {
        auto* t = text();
        if (t == nullptr) {
          return LoomTextMeasurement{};
        }
        LoomWorkerProfile::TextScope textProfile;
        return t->prepareTextForMount(
            surface,
            attributedString,
            paragraphAttributes,
            minimumSize,
            maximumSize);
      };
#if LOOM_WORKER
  operations.getScrollOffset = [this](SurfaceId surface, Tag tag) {
    // Live view state belongs to the UI thread; a JS-thread reader asks it.
    return readOnUIThread(
               [surface, tag](LoomHost& h)
                   -> std::optional<std::pair<float, float>> {
                 return h.getScrollOffset(surface, tag);
               },
               std::pair<float, float>{0, 0})
        .value_or(std::pair<float, float>{0, 0});
  };
  operations.getViewLocationInSurface = [this](SurfaceId surface, Tag tag)
      -> std::optional<std::pair<float, float>> {
    return readOnUIThread(
        [surface, tag](LoomHost& h) {
          return h.getViewLocationInSurface(
              surface, tag);
        },
        std::nullopt);
  };
#else
  operations.getScrollOffset = [this](SurfaceId surface, Tag tag) {
    return host_->getScrollOffset(surface, tag);
  };
  operations.getViewLocationInSurface = [this](SurfaceId surface, Tag tag)
      -> std::optional<std::pair<float, float>> {
    return host_->getViewLocationInSurface(surface, tag);
  };
#endif
  operations.reportUnsupportedInlineAttachment = [this](
                                                     SurfaceId surface,
                                                     Tag tag) {
    host_->reportUnsupportedInlineAttachment(surface, tag);
  };
  operations.reportUnsupportedTransform = [](SurfaceId surface, Tag tag) {
    std::fprintf(
        stderr,
        "LOOM_RENDERER does not support perspective/3D transform metrics for tag %d on surface %d\n",
        static_cast<int>(tag),
        static_cast<int>(surface));
  };
  operations.reportUnsupportedIntrinsicMeasurement = [](SurfaceId surface,
                                                        Tag tag,
                                                        const std::string&
                                                            component) {
    std::fprintf(
        stderr,
        "LOOM_RENDERER requires explicit size for intrinsically sized component %s, tag %d on surface %d\n",
        component.c_str(),
        static_cast<int>(tag),
        static_cast<int>(surface));
  };
  operations.updateState = [this](LoomTextStateUpdate update) {
    host_->updateTextState(update);
  };
  operations.updateComponentState = [this](
                                        SurfaceId surface,
                                        Tag tag,
                                        State::Shared state) {
    host_->updateComponentState(surface, tag, state);
  };
  operations.updateScrollContentBounds = [this](
                                             SurfaceId surface,
                                             Tag tag,
                                             const folly::dynamic& props,
                                             const Rect& bounds) {
    host_->updateScrollContentBounds(
        surface, tag, props, bounds);
  };
  operations.dispatchCommand = [this](
                                   SurfaceId surface,
                                   Tag tag,
                                   const std::string& commandName,
                                   const folly::dynamic& args) {
    host_->dispatchCommand(
        surface, tag, commandName, args);
  };
  operations.sendAccessibilityEvent = [this](
                                          SurfaceId surface,
                                          Tag tag,
                                          const std::string& eventType) {
    host_->sendAccessibilityEvent(surface, tag, eventType);
  };
  operations.setIsJSResponder = [this](
                                    SurfaceId surface,
                                    Tag tag,
                                    bool isJSResponder,
                                    bool blockNativeResponder) {
    host_->setIsJSResponder(
        surface, tag, isJSResponder, blockNativeResponder);
  };
  operations.commitFinished = [this, weakRendererToken](SurfaceId surface) {
    auto token = weakRendererToken.lock();
    if (token != nullptr && token->active && token->surfaceId == surface) {
      drainPendingRootCommits(surface);
    }
  };

#if LOOM_WORKER
  // Only pure text preparation callbacks remain executable during computation.
  // All native state/event/view effects are built into the owning UI result.
  operations.failSurface = worker_->defer(std::move(operations.failSurface));
  operations.applyBatchEarly = worker_->early(operations.applyBatch);
  operations.applyBatch = worker_->defer(std::move(operations.applyBatch));
  operations.registerEventTarget =
      worker_->defer(std::move(operations.registerEventTarget));
  operations.unregisterEventTarget =
      worker_->defer(std::move(operations.unregisterEventTarget));
  operations.dispatchEvent =
      worker_->defer(std::move(operations.dispatchEvent));
  operations.reportUnsupportedInlineAttachment =
      worker_->defer(std::move(operations.reportUnsupportedInlineAttachment));
  operations.reportUnsupportedTransform =
      worker_->defer(std::move(operations.reportUnsupportedTransform));
  operations.reportUnsupportedIntrinsicMeasurement = worker_->defer(
      std::move(operations.reportUnsupportedIntrinsicMeasurement));
  operations.updateState = worker_->defer(std::move(operations.updateState));
  operations.updateComponentState =
      worker_->defer(std::move(operations.updateComponentState));
  operations.updateScrollContentBounds =
      worker_->defer(std::move(operations.updateScrollContentBounds));
  operations.commitFinished = {};
#endif
  auto created =
      std::make_shared<LoomRenderer>(surfaceId, std::move(operations));
  created->setPointScaleFactor(directPointScaleFactorValue());
  {
    std::lock_guard state(stateMutex_);
    if (compat_ == nullptr) {
      compat_ =
          std::make_shared<LoomCompat>(getUIManager().getContextContainer());
    }
    created->setCompat(compat_);
  }

  // Constraints can arrive after the surface starts but before the first
  // commit, so a fresh renderer is initialized from whatever is known now.
  auto constraints = constraintsFor(surfaceId);
  created->setConstraints(
      constraints.minWidth,
      constraints.minHeight,
      constraints.maxWidth,
      constraints.maxHeight,
      constraints.isRTL);

  std::lock_guard state(stateMutex_);
  auto inserted = renderers_.emplace(surfaceId, std::move(created));
  return *inserted.first->second;
}

#if LOOM_WORKER
void LoomBinding::submitWorkerOperation(
    std::shared_ptr<SurfaceToken> token,
    LoomWorker::Task compute,
    std::function<void()> applied,
    bool synchronous,
    bool profileCommit,
    bool inlineEligible) {
  auto job = std::make_unique<LoomWorker::Job>();
  job->inlineEligible = inlineEligible;
  job->compute = std::move(compute);
  job->applied = std::move(applied);
  job->synchronous = synchronous;
  job->profileCommit = profileCommit;
  submitWorkerJob(std::move(token), std::move(job));
}

void LoomBinding::submitWorkerJob(
    std::shared_ptr<SurfaceToken> token,
    std::unique_ptr<LoomWorker::Job> job,
    bool* ranSynchronously) {
  if (ranSynchronously != nullptr) {
    *ranSynchronously = false;
  }
  job->active = [token] { return token->active.load(); };
  job->failed = [this, token](std::exception_ptr error) {
    if (!token->active.exchange(false))
      return;
    std::string reason =
        "Async Loom renderer computation or UI application failed";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& exception) {
      reason += ": ";
      reason += exception.what();
    } catch (...) {
    }
    host_->failSurface(token->surfaceId, reason);
  };
  if (const auto scope = LoomPlatformTraits::current().withWorkerScope;
      scope != nullptr && job->compute) {
    auto compute = std::move(job->compute);
    job->compute = [scope, compute = std::move(compute)]() mutable {
      scope([&compute] { compute(); });
    };
  }
  // A forced-sync commit computes on the UI thread either way. Running it here
  // when the worker is idle skips the worker round trip; when the worker still
  // owns work the job is queued behind it, which is what preserves ordering.
  if (job->synchronous && worker_->runIfIdle(job)) {
    if (ranSynchronously != nullptr) {
      *ranSynchronously = true;
    }
    return;
  }
  // A small commit costs less than the worker hand-off, so it is computed right
  // here (UI thread in main-thread mode, JS thread otherwise) when no other
  // work is outstanding.
  if (worker_->tryRunInline(job)) {
    return;
  }
  worker_->submit(std::move(job));
}

void LoomBinding::publishWorkerCommit(
    SurfaceId surfaceId,
    const std::vector<std::shared_ptr<NodeRef>>& children,
    bool forceSync) {
#if RN_JANK_REPORTER
  // Loom (debug): lets the jank report say how much of a slow task
  // was spent committing to the native renderer.
  struct JankCommitTimer {
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    ~JankCommitTimer() {
      jank::Reporter::addCommit(
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - start)
              .count());
    }
  } jankCommitTimer;
#endif
  // First commit: React only renders once startup has finished. Resolving here
  // also seeds the text capability answer from the UI thread.
  (void)platformUIExecutor();
  auto packet = std::make_unique<WorkerCommit>();
  packet->surfaceId = surfaceId;
  packet->token = tokenFor(surfaceId);
  packet->animationFrame = AnimationFrameWork::active();
  // Draft data is built once by the host. Freeze each version; appendChild
  // thereafter creates a new version and copies only its changed child list.
  auto identities = std::make_shared<TagMap<uint64_t>>();
  // The versions this publish newly freezes, i.e. the changed part of the graph.
  // They are marked committed once the commit has been applied, which is what lets
  // a later reconcile stop at an unchanged subtree.
  auto committedDrafts =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  // One publication identity, stamped on every draft this walk reaches. A shared
  // version is then visited once by comparison, instead of inserting every node
  // into a visited set.
  const auto publishSequence = ++publishSequence_;
  // Custom text managers/effect factories/default font handlers remain on UI,
  // so the commit has to know whether it contains text and whether anything in
  // it depends on host state. Both are settled during this visit rather than in a
  // second walk over the graph, so the drafts are reached once, in tree order.
  bool hasText = false;
  bool unsafeTextProps = false;
  bool retainsLiveSnapshot = false;
  // Size of the changed part of the tree, the cost estimate for inlining.
  size_t changedTexts = 0;
  std::function<DraftNode::FreezeFacts(const std::shared_ptr<DraftNode>&)> freeze =
      [&](const auto& draft) -> DraftNode::FreezeFacts {
        if (!draft)
          return {};
        if (draft->frozenCommit == publishSequence)
          return draft->freezeFacts;
        draft->frozenCommit = publishSequence;
        if (draft->surfaceGeneration != packet->token->generation)
          throw std::logic_error("Stale worker draft");
        // An already-published version is unchanged. The host builds a new version
        // whenever props or children change, so this subtree's answers are still
        // the ones recorded when it was published, and reusing them is what keeps
        // an unchanged subtree from being walked on every commit. The generation
        // check above still runs, so a stale draft is caught either way.
        if (draft->published)
          return draft->freezeFacts;

        draft->published = true;
        (*identities)[draft->tag] = draft->identity;
        committedDrafts->push_back(draft);

        DraftNode::FreezeFacts facts;
        facts.retainsLiveSnapshot = !draft->childrenExplicit;
        facts.unsafeTextProps = draft->retainedUnsafeTextProps;
        if (draft->initialProps) {
          if (!draft->initialPropsTextScanned) {
            draft->initialPropsTextScanned = true;
            draft->initialPropsHasTextEffects =
                propsContainTextEffects(*draft->initialProps);
          }
          facts.unsafeTextProps |= draft->initialPropsHasTextEffects;
        }
        // Only the pending patches can still change what this commit measures.
        // Applied patches are folded into `retainedUnsafeTextProps` as their
        // history is dropped, so this stays bounded by the pending work instead
        // of growing with app uptime.
        for (auto patch = draft->propsTail; patch && !patch->applied;
             patch = patch->previous) {
          facts.unsafeTextProps |= patch->unsafeText;
        }
        facts.hasText = draft->roles.carriesText();
        if (facts.hasText)
          ++changedTexts;

        for (const auto& child : *draft->children) {
          const auto childFacts = freeze(child);
          facts.hasText |= childFacts.hasText;
          facts.unsafeTextProps |= childFacts.unsafeTextProps;
          facts.retainsLiveSnapshot |= childFacts.retainsLiveSnapshot;
        }
        draft->freezeFacts = facts;
        return facts;
      };
  for (const auto& ref : children) {
    if (!ref || ref->surfaceToken != packet->token)
      throw std::logic_error("Foreign worker root");
    const auto facts = freeze(ref->draft);
    hasText |= facts.hasText;
    unsafeTextProps |= facts.unsafeTextProps;
    retainsLiveSnapshot |= facts.retainsLiveSnapshot;
    packet->roots.push_back(ref->draft);
  }
  // Insert only the tags this walk newly published. `freeze` skips unchanged
  // subtrees, so the map it produces is a delta rather than the whole graph, and
  // erasing the surface first would drop the entry for every skipped subtree.
  // Reads validate the stored identity against the live draft (see
  // `setWorkerNativeProps`); `applyPublishedIdentities` erases an entry once the
  // version is mounted or removed.
  {
    std::lock_guard state(stateMutex_);
    for (const auto& [tag, identity] : *identities)
      submittedIdentities_[surfaceId][tag] = identity;
  }
  auto document = pendingDocumentInstanceHandles_.find(surfaceId);
  if (document != pendingDocumentInstanceHandles_.end()) {
    packet->documentHandle = std::move(document->second);
    pendingDocumentInstanceHandles_.erase(document);
  }
  // Asked only when the answer can change the decision: the query is a JNI
  // call plus a view-manager registry lookup, and it must not be cached
  // across commits because `TextEffectRegistry.hasRegisteredFactories()`
  // changes when a library registers or unregisters a factory.
  const bool textSafe =
      !hasText || canMeasureTextOnWorker();
  // The text-safety decision belongs to this commit only. Earlier revisions
  // kept it on the surface token, which made every later job on the surface
  // (setNativeProps, constraints) synchronous as well.
  const bool synchronousText =
      retainsLiveSnapshot || (hasText && (!textSafe || unsafeTextProps));
  forceSync = forceSync || synchronousText;
  auto& live = renderer(surfaceId);
  std::shared_ptr<LoomRenderer> owner;
  {
    std::lock_guard state(stateMutex_);
    owner = renderers_.at(surfaceId);
  }
  auto token = packet->token;
  // Filled by the compute step: the tags the renderer dropped, or nothing with
  // `fullIdentities` set when that commit walked the whole tree.
  auto removedTags = std::make_shared<std::vector<Tag>>();
  auto fullIdentities = std::make_shared<bool>(false);
  auto applied = [this, token, identities, committedDrafts, removedTags,
                  fullIdentities] {
    applyPublishedIdentities(
        token,
        *identities,
        *committedDrafts,
        *fullIdentities ? nullptr : removedTags.get());
  };
  // The worker pauses after all preceding UI results. A synchronous job borrows
  // the same renderer on UI for its ENTIRE reconciliation/style/layout/diff.
  // Reentrant submissions remain ordered after the enclosing UI result.
  (void)live;
  // Loom: cost estimate. Units are the changed nodes, with a text
  // counting extra because measuring and preparing it dominates a small update.
  // A commit also carries a fixed cost proportional to the whole tree (layout
  // pass, unsupported-attachment scan, identity collection), so the changed
  // size alone cannot predict its time: small commits additionally have to be
  // cheap in practice, judged by the recently observed time of such commits.
  const double units =
      1.0 + static_cast<double>(committedDrafts->size()) + 4.0 * changedTexts;
  const bool smallCommit = units <= kInlineMaxUnits;
  const bool inlineEligible = !forceSync && smallCommit &&
      smallCommitMs_.load(std::memory_order_relaxed) < kInlineBudgetMs;
  auto compute = LoomWorker::Task([this,
                                               owner,
                                               packet = std::move(packet),
                                               identities,
                                               publishIdentities = applied,
                                               removedTags,
                                               fullIdentities,
                                               forceSync,
                                               smallCommit] {
    const auto computeStart = std::chrono::steady_clock::now();
    const bool fullCommit = owner->nextCommitIsFull() ||
        packet->documentHandle != nullptr;
    owner->setAnimationFrameCommit(packet->animationFrame);
    reconcileWorkerCommit(*owner, *packet);
    owner->setAnimationFrameCommit(false);
    if (smallCommit) {
      // Keeps the budget test honest on this device right now: a cold CPU, or
      // a large tree whose fixed per-commit cost dominates, turns it off.
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - computeStart)
                            .count();
      const double previous = smallCommitMs_.load(std::memory_order_relaxed);
      smallCommitMs_.store(
          static_cast<float>(previous * 0.7 + ms * 0.3),
          std::memory_order_relaxed);
    }

    // An incremental commit reports a delta: the versions it published are
    // already in `identities`, and the renderer lists the tags it dropped.
    // Only a full commit re-collects the whole live set (and then reconciles
    // removals against it), which is what every commit used to do.
    *removedTags = owner->takeDroppedTags();
    if (!fullCommit) {
      if (forceSync)
        publishIdentities();
      return;
    }
    *fullIdentities = true;
    removedTags->clear();
    // Include children retained through compatibility/native DOM snapshots.
    // This metadata is result-owned and is never read concurrently by UI.
    identities->clear();
    std::function<void(Tag)> collect = [&](Tag tag) {
      if (identities->count(tag))
        return;
      (*identities)[tag] = owner->identityOf(tag);
      // Read-only recursion, so the borrowed list is safe and no vector is
      // copied per visited node.
      if (const auto* children = owner->childrenOfRef(tag); children != nullptr) {
        for (const auto child : *children)
          collect(child);
      }
    };
    collect(owner->rootTag());
    if (forceSync)
      publishIdentities();
  });
  submitWorkerOperation(
      token,
      std::move(compute),
      std::move(applied),
      forceSync,
      true,
      inlineEligible);
  // The queued synchronous job computes on the UI thread inside its own apply,
  // so a JS thread has nothing to gain by waiting for it: readers use the
  // barrier, and the UI thread never waits for JS.
  if (forceSync && worker_->onUIThread())
    worker_->synchronize();
}

void LoomBinding::applyPublishedIdentities(
    const std::shared_ptr<SurfaceToken>& token,
    const TagMap<uint64_t>& identities,
    const std::vector<std::shared_ptr<DraftNode>>& committedDrafts,
    const std::vector<Tag>* removedTags) {
  std::lock_guard state(stateMutex_);
  if (removedTags != nullptr) {
    // Delta: drop exactly the tags the renderer removed (when this surface
    // still owns them), then upsert the published versions below.
    for (const auto tag : *removedTags) {
      const auto mountedToken = mountedTokens_.find(tag);
      if (mountedToken != mountedTokens_.end() &&
          mountedToken->second->surfaceId == token->surfaceId) {
        mountedTokens_.erase(mountedToken);
        mountedIdentities_.erase(tag);
      }
    }
  }
  if (hasFabricFamilies_.load(std::memory_order_relaxed)) {
    releaseFabricFamilies(token->surfaceId, removedTags, identities);
  }
  // Erase only what this commit no longer contains. This pass is what discovers
  // removals, and it stays correct because `identities` is the full live set
  // rather than the freeze walk's output.
  for (auto it = mountedTokens_.begin();
       removedTags == nullptr && it != mountedTokens_.end();) {
    if (it->second->surfaceId == token->surfaceId &&
        identities.find(it->first) == identities.end()) {
      mountedIdentities_.erase(it->first);
      it = mountedTokens_.erase(it);
    } else {
      ++it;
    }
  }
  // [LoomMemory] These versions are mounted now (or gone), so they no longer
  // count as "submitted": `setWorkerNativeProps` finds them through
  // `mountedIdentities_`. Entries were only ever dropped with the surface, i.e.
  // one per tag the surface ever published.
  if (auto surfaceSubmitted = submittedIdentities_.find(token->surfaceId);
      surfaceSubmitted != submittedIdentities_.end()) {
    auto& submitted = surfaceSubmitted->second;
    if (removedTags != nullptr) {
      for (const auto tag : *removedTags) {
        submitted.erase(tag);
      }
    }
    for (const auto& [tag, identity] : identities) {
      if (auto entry = submitted.find(tag);
          entry != submitted.end() && entry->second == identity) {
        submitted.erase(entry);
      }
    }
  }
  // Write only the entries that actually differ. Every commit used to erase and
  // re-insert an entry for every reachable tag, which copies a `shared_ptr` and
  // rewrites a hash entry per node even when nothing about it changed.
  // Deliberately idempotent: a synchronous commit publishes from the worker and
  // is then reported on UI, so this runs twice for one commit.
  for (const auto& [tag, identity] : identities) {
    const auto mounted = mountedIdentities_.find(tag);
    if (mounted != mountedIdentities_.end() && mounted->second == identity) {
      const auto mountedToken = mountedTokens_.find(tag);
      if (mountedToken != mountedTokens_.end() && mountedToken->second == token) {
        continue;
      }
    }
    mountedIdentities_[tag] = identity;
    mountedTokens_[tag] = token;
  }
  // The batch has been applied, so the live renderer now holds these versions and a
  // later reconcile may treat them as an untouched subtree. Marking has to happen
  // here rather than in `freeze`: publishes can be in flight, and a version frozen
  // into a packet the live tree has not reached yet must not look committed.
  for (const auto& draft : committedDrafts) {
    if (draft != nullptr) {
      draft->workerCommitted = true;
    }
  }
}

void LoomBinding::reconcileWorkerCommit(
    LoomRenderer& liveRenderer,
    const WorkerCommit& packet) {
  const auto surfaceId = packet.surfaceId;
  const auto& surfaceToken = packet.token;
  if (!surfaceToken->active)
    return;
  try {
    // Selection, draft validation and the inline-attachment check are shared
    // with the synchronous reconciler.
    std::vector<std::shared_ptr<DraftNode>> rootDrafts;
    rootDrafts.reserve(packet.roots.size());
    for (const auto& draft : packet.roots) {
      if (draft == nullptr || draft->surfaceId != surfaceId) {
        throw std::logic_error("completeRoot selected an invalid draft handle");
      }
      rootDrafts.push_back(draft);
    }
    auto commitSelection =
        selectCommitDrafts(liveRenderer, surfaceId, *surfaceToken, rootDrafts);
    auto& selected = commitSelection.selected;
    const auto rootTag = commitSelection.rootTag;
    const auto& rootChildren = commitSelection.rootChildren;
    if (commitSelection.unsupportedAttachmentTag != 0) {
      throw std::logic_error(
          "Direct worker commit contains an unsupported inline text attachment");
    }

    // Frozen drafts retain their complete graph and this path never marks them
    // `committedSnapshot` (it does not compact), so paint-only is detected
    // against worker-owned identities here. The synchronous path asks the
    // complementary question - `committedSnapshot && !hasInitialProps` - because
    // it *does* compact, so it cannot read a frozen graph back. The predicates
    // are deliberately different: each uses the bookkeeping its own mode
    // maintains, and substituting one for the other would disable paint-only in
    // that mode.
    bool paintOnly =
        !selected.empty() && liveRenderer.childrenOf(rootTag) == rootChildren;
    std::vector<std::pair<Tag, folly::dynamic>> paintPatches;
    for (const auto& entry : selected) {
      if (!paintOnly)
        break;
      const auto& draft = *entry.draft;
      const bool rawText = draft.roles.isRawText();
      if (liveRenderer.identityOf(draft.tag) != draft.identity ||
          (entry.childrenChanged &&
           childrenDifferFrom(liveRenderer, draft.tag, entry.childTags)) ||
          (!rawText &&
           !liveRenderer.hasSameInstanceHandle(
               draft.tag, draft.instanceHandle))) {
        paintOnly = false;
        break;
      }
      std::vector<std::shared_ptr<const PropsPatch>> patches;
      for (auto patch = draft.propsTail; patch && !patch->applied;
           patch = patch->previous) {
        if (patch->rawText || rawText) {
          paintOnly = false;
          break;
        }
        patches.push_back(patch);
      }
      for (auto it = patches.rbegin(); it != patches.rend(); ++it)
        paintPatches.emplace_back(draft.tag, (*it)->props);
    }
    if (!packet.documentHandle && paintOnly && !paintPatches.empty() &&
        liveRenderer.tryCommitPaintOnlyProps(paintPatches)) {
      for (const auto& entry : selected) {
        for (auto patch = entry.draft->propsTail; patch && !patch->applied;
             patch = patch->previous)
          patch->applied = true;
      }
      return;
    }

    // The worker path leaves the drafts to the host, which still owns them while
    // this result is in flight, so it does not compact them.
    reconcileSelectedDrafts(liveRenderer, surfaceId, commitSelection, false);
    if (packet.documentHandle)
      liveRenderer.setInstanceHandle(surfaceId, packet.documentHandle);
  } catch (const std::exception& error) {
    liveRenderer.abortCommit(error.what());
    // failSurface is a buffered UI effect. Do not throw through the runtime
    // result callback after the failed surface has already been invalidated.
  } catch (...) {
    liveRenderer.abortCommit("unknown exception in Loom renderer worker");
  }
}
#endif

LoomBinding::CommitSelection
LoomBinding::selectCommitDrafts(
    LoomRenderer& liveRenderer,
    SurfaceId surfaceId,
    const SurfaceToken& surfaceToken,
    const std::vector<std::shared_ptr<DraftNode>>& rootDrafts) {
  struct SelectionRecord {
    enum class Visit : uint8_t { Unseen, Active, Done };
    uint64_t identity{0};
    size_t selectedIndex{std::numeric_limits<size_t>::max()};
    size_t parentCount{0};
    Visit visit{Visit::Unseen};
    bool selected{false};
  };
  constexpr auto noSelectedIndex = std::numeric_limits<size_t>::max();

  CommitSelection selection;
  auto& selected = selection.selected;
  TagMap<SelectionRecord> selectionByTag;

  std::function<void(const std::shared_ptr<DraftNode>&)> selectDraft;
  std::function<void(Tag)> retainLiveSubtree;
  // A retained child is unchanged live structure: recording its root is
  // enough. Its subtree is not walked - that walk made every commit cost the
  // whole tree, and nothing below an unchanged node can be added, removed or
  // re-parented without React cloning the node (which makes it a draft).
  retainLiveSubtree = [&](Tag tag) {
    auto& record = selectionByTag[tag];
    if (record.visit == SelectionRecord::Visit::Active) {
      throw std::logic_error("Cycle in retained UI-thread renderer tree");
    }
    if (record.selected) {
      return;
    }
    record.identity = liveRenderer.identityOf(tag);
    record.selected = true;
    record.visit = SelectionRecord::Visit::Done;
  };
  selectDraft = [&](const std::shared_ptr<DraftNode>& draft) {
    if (draft == nullptr || draft->surfaceId != surfaceId ||
        draft->surfaceGeneration != surfaceToken.generation) {
      throw std::logic_error("Invalid or stale UI-thread renderer draft");
    }
    auto& record = selectionByTag[draft->tag];
    if (record.visit == SelectionRecord::Visit::Active) {
      throw std::logic_error("Cycle in UI-thread renderer draft tree");
    }
    if (record.selected) {
      if (record.identity != draft->identity) {
        throw std::logic_error(
            "Conflicting draft identities share a React tag");
      }
      throw std::logic_error(
          "A UI-thread renderer draft has multiple parents");
    }
    record.identity = draft->identity;
    record.selected = true;
    record.visit = SelectionRecord::Visit::Active;
    const auto liveIdentity = liveRenderer.identityOf(draft->tag);
    const bool sameLiveIdentity =
        liveIdentity != 0 && liveIdentity == draft->identity;
    // The compacting path clears a committed draft's children, so it can only read
    // them back from the live renderer. The worker path never compacts, so it marks
    // its versions committed once the batch has been applied, and their children
    // still agree with the live tree. A draft that retains live children is
    // deliberately excluded: that branch re-validates each retained child's identity,
    // so skipping it would drop the check rather than only the walk.
    const bool useLiveChildren = sameLiveIdentity &&
        ((draft->committedSnapshot && !draft->childrenExplicit) ||
         (draft->workerCommitted && draft->retainedLiveChildren == nullptr));
    record.selectedIndex = selected.size();
    selected.push_back({draft, {}, !useLiveChildren});

    if (useLiveChildren) {
      if (const auto* children = liveRenderer.childrenOfRef(draft->tag);
          children != nullptr) {
        for (const auto child : *children) {
          retainLiveSubtree(child);
        }
      }
      record.visit = SelectionRecord::Visit::Done;
      return;
    }
    if (draft->retainedLiveChildren != nullptr) {
      for (const auto& child : *draft->retainedLiveChildren) {
        auto& childRecord = selectionByTag[child.tag];
        if (++childRecord.parentCount > 1) {
          throw std::logic_error(
              "A UI-thread renderer draft has a child with multiple parents");
        }
        if (child.identity == 0 ||
            liveRenderer.identityOf(child.tag) != child.identity) {
          throw std::logic_error(
              "A retained UI-thread renderer child changed identity");
        }
        selected[record.selectedIndex].childTags.push_back(child.tag);
        retainLiveSubtree(child.tag);
      }
    }
    for (const auto& child : *draft->children) {
      if (child == nullptr) {
        continue;
      }
      auto& childRecord = selectionByTag[child->tag];
      if (++childRecord.parentCount > 1) {
        throw std::logic_error(
            "A UI-thread renderer draft has multiple parents");
      }
      // Recursive selection can grow `selected` and reallocate its storage,
      // so reacquire this parent by index rather than retaining a reference.
      selected[record.selectedIndex].childTags.push_back(child->tag);
      selectDraft(child);
    }
    record.visit = SelectionRecord::Visit::Done;
  };

  selection.rootChildren.reserve(rootDrafts.size());
  for (const auto& draft : rootDrafts) {
    selection.rootChildren.push_back(draft->tag);
    if (++selectionByTag[draft->tag].parentCount > 1) {
      throw std::logic_error("A selected root draft appears more than once");
    }
    selectDraft(draft);
  }
  selection.rootTag =
      liveRenderer.rootTag() == 0 ? surfaceId : liveRenderer.rootTag();
  auto& rootRecord = selectionByTag[selection.rootTag];
  rootRecord.identity = liveRenderer.identityOf(selection.rootTag);
  rootRecord.selected = true;
  rootRecord.visit = SelectionRecord::Visit::Done;

  // Reject inline view attachments while the selected graph is still only a
  // draft. `prepare()` used to discover these after nodes, Yoga, event
  // targets, and child lists had already been changed in the live renderer.
  selection.selectedIdentities.reserve(selectionByTag.size());
  for (const auto& [tag, record] : selectionByTag) {
    if (!record.selected) {
      continue;
    }
    selection.selectedIdentities.emplace(tag, record.identity);
  }
  // Removal roots and the inline-attachment check, both limited to the parents
  // whose child lists changed. Only such a parent can drop a child or gain an
  // unsupported one: an unchanged parent keeps its (already validated)
  // children.
  const auto rolesOf = [&](Tag tag) -> LoomComponentTable::Roles {
    const auto record = selectionByTag.find(tag);
    if (record != selectionByTag.end() &&
        record->second.selectedIndex != noSelectedIndex) {
      const auto& draft = selected[record->second.selectedIndex].draft;
      if (draft->componentName != nullptr) {
        return draft->roles;
      }
    }
    return liveRenderer.componentRoles(tag);
  };
  const auto noteRemovedChildren = [&](Tag parentTag,
                                       const std::vector<Tag>& next) {
    const auto* live = liveRenderer.childrenOfRef(parentTag);
    if (live == nullptr || live->empty()) {
      return;
    }
    if (*live == next) {
      return;
    }
    std::unordered_set<Tag> keep(next.begin(), next.end());
    for (const auto child : *live) {
      if (keep.count(child) == 0) {
        selection.removalRoots.push_back(child);
      }
    }
  };
  noteRemovedChildren(selection.rootTag, selection.rootChildren);
  for (const auto& entry : selected) {
    const auto& draft = *entry.draft;
    const auto liveIdentity = liveRenderer.identityOf(draft.tag);
    if (liveIdentity != 0 && liveIdentity != draft.identity) {
      // The old version of a replaced node leaves the tree as a whole.
      selection.removalRoots.push_back(draft.tag);
    } else if (entry.childrenChanged) {
      noteRemovedChildren(draft.tag, entry.childTags);
    }
    if (!entry.childrenChanged || selection.unsupportedAttachmentTag != 0) {
      continue;
    }
    const auto parentRoles = rolesOf(draft.tag);
    if (!parentRoles.isText() && !parentRoles.textInput) {
      continue;
    }
    for (const auto child : entry.childTags) {
      if (!rolesOf(child).isTextStack()) {
        selection.unsupportedAttachmentTag = child;
        break;
      }
    }
  }
  return selection;
}

void LoomBinding::reconcileSelectedDrafts(
    LoomRenderer& liveRenderer,
    SurfaceId surfaceId,
    const CommitSelection& selection,
    bool compact) {
  static const auto emptyDraftChildren =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  const auto& selected = selection.selected;

  // Preserve cleanup edges owned by a parent that the selected graph removes.
  liveRenderer.captureRemovedMountedEdgesFrom(
      selection.selectedIdentities, selection.removalRoots);

  for (const auto& entry : selected) {
    const auto& draft = *entry.draft;
    const auto currentIdentity = liveRenderer.identityOf(draft.tag);
    const bool sameLiveIdentity =
        currentIdentity != 0 && currentIdentity == draft.identity;
    if (!sameLiveIdentity) {
      if (!draft.hasInitialProps || draft.componentName == nullptr ||
          (draft.initialProps == nullptr && draft.initialRawText == nullptr)) {
        throw std::logic_error("Stale UI-thread renderer draft identity");
      }
      if (draft.initialRawText != nullptr) {
        liveRenderer.createRawTextNode(draft.tag, *draft.initialRawText);
      } else {
        liveRenderer.createNode(
            draft.tag,
            *draft.componentName,
            *draft.initialProps,
            draft.instanceHandle);
      }
      liveRenderer.setIdentity(draft.tag, draft.identity);
    } else {
      liveRenderer.setInstanceHandle(draft.tag, draft.instanceHandle);
    }

    std::vector<std::shared_ptr<const PropsPatch>> patches;
    for (auto patch = draft.propsTail; patch != nullptr;
         patch = patch->previous) {
      // Props patches form an append-only chronological chain (only `applied`
      // is mutable). Publication always applies the unapplied suffix in order,
      // so an applied patch marks the boundary before the committed history.
      // Stop here rather than rescanning the node's full history on later
      // commits.
      if (patch->applied) {
        break;
      }
      patches.push_back(patch);
    }
    for (auto patch = patches.rbegin(); patch != patches.rend(); ++patch) {
      if ((*patch)->rawText.has_value()) {
        liveRenderer.updateRawText(draft.tag, *(*patch)->rawText);
      } else {
        liveRenderer.cloneNodeWithNewProps(draft.tag, (*patch)->props);
      }
    }
    if (draft.compatProps != nullptr) {
      liveRenderer.setCompatProps(draft.tag, draft.compatProps);
    }
  }

  for (const auto& entry : selected) {
    if (!entry.childrenChanged) {
      continue;
    }
    if (childrenDifferFrom(liveRenderer, entry.draft->tag, entry.childTags)) {
      liveRenderer.setChildren(entry.draft->tag, entry.childTags);
    }
  }

  // Mark the selected handles as committed before native callbacks can re-enter
  // completeRoot. Props patches are shared with older refs, so an explicitly
  // applied patch is never replayed over a later setNativeProps.
  for (const auto& entry : selected) {
    auto& draft = *entry.draft;
    for (auto patch = draft.propsTail; patch != nullptr;
         patch = patch->previous) {
      if (patch->applied) {
        break;
      }
      patch->applied = true;
    }
    if (!compact) {
      continue;
    }
    // The commit is applied and the live node is the source of truth, so the
    // applied history is dropped rather than retained per prop update. A
    // reentrant commit that appends a patch while the root completes starts a
    // fresh chain from this assignment.
    draft.propsTail = nullptr;
    draft.committedSnapshot = true;
    draft.hasInitialProps = false;
    draft.initialProps.reset();
    draft.initialPropsTextScanned = false;
    draft.initialPropsHasTextEffects = false;
    draft.initialRawText.reset();
    draft.componentName.reset();
    draft.childrenExplicit = false;
    draft.children = emptyDraftChildren;
    draft.retainedLiveChildren.reset();
  }

  liveRenderer.completeRoot(surfaceId, selection.rootChildren);
}

void LoomBinding::publishRootCommit(
    SurfaceId surfaceId,
    const std::vector<std::shared_ptr<NodeRef>>& children,
    const std::shared_ptr<SurfaceToken>& surfaceToken,
    uint64_t profileGeneration) {
  static const auto emptyDraftChildren =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  if (surfaceToken == nullptr || !surfaceToken->active ||
      surfaceToken->surfaceId != surfaceId) {
    return;
  }
  if (publishingRootCommits_.count(surfaceId) != 0) {
    pendingRootCommits_[surfaceId].push_back(
        {surfaceToken, children, profileGeneration});
    return;
  }
  auto& currentRenderer = renderer(surfaceId);
  if (currentRenderer.isCommitting()) {
    pendingRootCommits_[surfaceId].push_back(
        {surfaceToken, children, profileGeneration});
    return;
  }
  const bool profileSyncCommit = LoomWorkerProfile::enabled();
  const auto syncCommitStart = profileSyncCommit
      ? LoomWorkerProfile::Clock::now()
      : LoomWorkerProfile::Clock::time_point{};
  publishingRootCommits_.insert(surfaceId);
  // releaseSurfaceInternal removes the map entry immediately, but this strong
  // local reference keeps the active commit's renderer alive until callbacks
  // have unwound. A restarted surface gets a new renderer and new token.
  auto liveRendererOwner = renderers_.at(surfaceId);
  auto& liveRenderer = *liveRendererOwner;
  try {
#if LOOM_PROFILE
    const auto reconcileStart = std::chrono::steady_clock::now();
#endif
    // Same selection and validation as the worker reconciler; this path runs it
    // inline on the UI thread instead of on the worker.
    std::vector<std::shared_ptr<DraftNode>> rootDrafts;
    rootDrafts.reserve(children.size());
    for (const auto& childRef : children) {
      if (childRef == nullptr || childRef->draft == nullptr ||
          childRef->surfaceToken != surfaceToken ||
          childRef->surfaceId != surfaceId) {
        throw std::logic_error("completeRoot selected an invalid draft handle");
      }
      rootDrafts.push_back(childRef->draft);
    }
    auto commitSelection =
        selectCommitDrafts(liveRenderer, surfaceId, *surfaceToken, rootDrafts);
    auto& selected = commitSelection.selected;
    const auto rootTag = commitSelection.rootTag;
    const auto& rootChildren = commitSelection.rootChildren;
    if (commitSelection.unsupportedAttachmentTag != 0) {
      // A host view inside Text is not supported. Stop the surface exactly like
      // the worker path does; returning here alone would report the commit as
      // applied and freeze the surface on its previous contents, and every later
      // commit carrying the attachment would be dropped the same way.
      surfaceToken->active = false;
      host_->reportUnsupportedInlineAttachment(
          surfaceId, commitSelection.unsupportedAttachmentTag);
      host_->failSurface(
          surfaceId,
          "Loom renderer commit contains an unsupported inline text attachment");
      publishingRootCommits_.erase(surfaceId);
      drainPendingRootCommits(surfaceId);
      return;
    }

    // A deliberately small direct path handles commits that change only known
    // paint props. Require an unchanged, already-mounted identity/child graph;
    // unknown props, new nodes, changed handles, pending NativeDOM root setup,
    // and every structural/text/layout change fall through to full
    // reconciliation.
    //
    // `committedSnapshot && !hasInitialProps` is this path's way of asking "has
    // this draft already been committed once", which the worker path instead
    // answers from worker-owned identities because it never compacts. See the
    // note on the worker's paint-only block: the predicates are not
    // interchangeable.
    bool paintOnlyCandidate =
        pendingDocumentInstanceHandles_.count(surfaceId) == 0 &&
        !selected.empty() && liveRenderer.childrenOf(rootTag) == rootChildren;
    std::vector<std::pair<Tag, folly::dynamic>> paintOnlyPatches;
    if (paintOnlyCandidate) {
      for (const auto& entry : selected) {
        const auto& draft = *entry.draft;
        if (!draft.committedSnapshot || draft.hasInitialProps ||
            (entry.childrenChanged &&
             childrenDifferFrom(liveRenderer, draft.tag, entry.childTags)) ||
            liveRenderer.identityOf(draft.tag) != draft.identity ||
            !liveRenderer.hasSameInstanceHandle(
                draft.tag, draft.instanceHandle)) {
          paintOnlyCandidate = false;
          break;
        }
        std::vector<std::shared_ptr<const PropsPatch>> newPatches;
        for (auto patch = draft.propsTail; patch != nullptr;
             patch = patch->previous) {
          if (patch->applied) {
            break;
          }
          if (patch->rawText.has_value()) {
            paintOnlyCandidate = false;
            break;
          }
          newPatches.push_back(patch);
        }
        if (!paintOnlyCandidate)
          break;
        for (auto patch = newPatches.rbegin(); patch != newPatches.rend();
             ++patch) {
          paintOnlyPatches.emplace_back(draft.tag, (*patch)->props);
        }
      }
    }
    if (paintOnlyCandidate && !paintOnlyPatches.empty() &&
        liveRenderer.tryCommitPaintOnlyProps(paintOnlyPatches)) {
      for (auto& entry : selected) {
        auto& draft = *entry.draft;
        for (auto patch = draft.propsTail; patch != nullptr;
             patch = patch->previous) {
          if (patch->applied) {
            break;
          }
          patch->applied = true;
        }
        // Everything above is applied, and the live node is now the source of
        // truth for it, so drop the history instead of retaining one patch per
        // prop update. Patches appended by a reentrant commit stay reachable
        // through the new head this assignment gives them.
        draft.propsTail = nullptr;
        draft.committedSnapshot = true;
        draft.hasInitialProps = false;
        draft.initialProps.reset();
    draft.initialPropsTextScanned = false;
    draft.initialPropsHasTextEffects = false;
        draft.initialRawText.reset();
        draft.componentName.reset();
        draft.childrenExplicit = false;
        draft.children = emptyDraftChildren;
        draft.retainedLiveChildren.reset();
      }
      publishingRootCommits_.erase(surfaceId);
      drainPendingRootCommits(surfaceId);
      return;
    }

    // This path owns the renderer outright, so it compacts the drafts it
    // commits: the live node is now the source of truth for every applied patch.
    reconcileSelectedDrafts(liveRenderer, surfaceId, commitSelection, true);
    // This path keeps no mounted-identity delta; drop the record.
    (void)liveRenderer.takeDroppedTags();
#if LOOM_PROFILE
    // Reconciliation and the root commit are one atomic step now, so this marks
    // the end of both.
    const auto reconcileEnd = std::chrono::steady_clock::now();
#endif
#if LOOM_PROFILE
    const auto profileAfterCommit = std::chrono::steady_clock::now();
    if (LoomWorkerProfile::enabled()) {
      const auto profileMs = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
      };
      loomLog(
          LoomLogLevel::Profile,
          "LoomPublishProfile",
          "surface=%d gen=%llu reconciliation=%.3fms rendererCommit=%.3fms",
          static_cast<int>(surfaceId),
          static_cast<unsigned long long>(profileGeneration),
          profileMs(reconcileStart, reconcileEnd),
          profileMs(reconcileEnd, profileAfterCommit));
    }
#endif
    if (surfaceToken->active) {
      auto documentHandle = pendingDocumentInstanceHandles_.find(surfaceId);
      if (documentHandle != pendingDocumentInstanceHandles_.end()) {
        liveRenderer.setInstanceHandle(
            liveRenderer.rootTag(), std::move(documentHandle->second));
        pendingDocumentInstanceHandles_.erase(documentHandle);
      }
    }
    publishingRootCommits_.erase(surfaceId);
    drainPendingRootCommits(surfaceId);
    if (profileSyncCommit) {
      loomLog(
          LoomLogLevel::Profile,
          "LoomDirectSyncProfile",
          "generation=%llu main=%d work=%.3f",
          static_cast<unsigned long long>(profileGeneration),
          LoomPlatformTraits::current().onUIThread() ? 1 : 0,
          LoomWorkerProfile::ms(
              syncCommitStart, LoomWorkerProfile::Clock::now()));
    }
  } catch (const std::exception& error) {
    liveRenderer.abortCommit(error.what());
    publishingRootCommits_.erase(surfaceId);
    pendingRootCommits_.erase(surfaceId);
    drainingRootCommits_.erase(surfaceId);
    throw;
  } catch (...) {
    liveRenderer.abortCommit(
        "unknown exception while reconciling a root commit");
    publishingRootCommits_.erase(surfaceId);
    pendingRootCommits_.erase(surfaceId);
    drainingRootCommits_.erase(surfaceId);
    throw;
  }
}

void LoomBinding::drainPendingRootCommits(SurfaceId surfaceId) {
  if (publishingRootCommits_.count(surfaceId) != 0) {
    return;
  }
  auto queueIterator = pendingRootCommits_.find(surfaceId);
  if (queueIterator == pendingRootCommits_.end() ||
      !drainingRootCommits_.insert(surfaceId).second) {
    return;
  }

  while (true) {
    queueIterator = pendingRootCommits_.find(surfaceId);
    if (queueIterator == pendingRootCommits_.end() ||
        queueIterator->second.empty()) {
      break;
    }
    auto pending = std::move(queueIterator->second.front());
    queueIterator->second.pop_front();
    if (pending.surfaceToken == nullptr || !pending.surfaceToken->active) {
      continue;
    }
    publishRootCommit(
        surfaceId,
        pending.children,
        pending.surfaceToken,
        pending.profileGeneration);
  }

  pendingRootCommits_.erase(surfaceId);
  drainingRootCommits_.erase(surfaceId);
}

#if LOOM_WORKER
void LoomBinding::flushPendingPatches(SurfaceId surfaceId) {
  auto pending = pendingPatches_.find(surfaceId);
  if (pending == pendingPatches_.end()) {
    return;
  }
  auto patches = std::make_shared<std::vector<PendingPatch>>(
      std::move(pending->second));
  pendingPatches_.erase(pending);
  if (patches->empty()) {
    return;
  }
  std::shared_ptr<LoomRenderer> owner;
  std::shared_ptr<SurfaceToken> token;
  {
    std::lock_guard state(stateMutex_);
    const auto renderer = renderers_.find(surfaceId);
    const auto surfaceToken = surfaceTokens_.find(surfaceId);
    if (renderer == renderers_.end() || surfaceToken == surfaceTokens_.end() ||
        !surfaceToken->second->active) {
      return;
    }
    owner = renderer->second;
    token = surfaceToken->second;
  }
  auto job = std::make_unique<LoomWorker::Job>();
  // Queued like a commit, so it applies after every commit React made before
  // it, and its view operations reach the UI thread the same way.
  job->compute = [owner, patches] {
    std::vector<std::pair<Tag, folly::dynamic>> props;
    bool text = false;
    for (auto& patch : *patches) {
      // A node that is gone, or whose tag now names another node, is skipped.
      if (owner->identityOf(patch.tag) != patch.identity) {
        continue;
      }
      if (patch.rawText.has_value()) {
        owner->updateRawText(patch.tag, std::move(*patch.rawText));
        text = true;
      } else {
        props.emplace_back(patch.tag, std::move(patch.props));
      }
    }
    if (!text && !props.empty() && owner->tryCommitPaintOnlyProps(props)) {
      return;
    }
    for (const auto& [tag, patch] : props) {
      owner->cloneNodeWithNewProps(tag, patch);
    }
    if (text || !props.empty()) {
      owner->reflow(); // runs a commit over what was just marked
    }
  };
  job->applied = [] {};
  submitWorkerJob(std::move(token), std::move(job));
}
#endif

std::shared_ptr<LoomBinding::NodeRef> LoomBinding::makeNodeRef(
    const NodeRef& source,
    std::shared_ptr<DraftNode> draft) {
  if (source.surfaceToken != nullptr && source.surfaceToken->active) {
    return std::make_shared<NodeRef>(
        source.surfaceId, source.tag, source.surfaceToken, std::move(draft));
  }
  return makeNodeRef(source.surfaceId, source.tag, std::move(draft));
}

std::shared_ptr<LoomBinding::NodeRef>
LoomBinding::makeNodeRef(
    SurfaceId surfaceId,
    Tag tag,
    std::shared_ptr<DraftNode> draft) {
  return std::make_shared<NodeRef>(
      surfaceId, tag, tokenFor(surfaceId), std::move(draft));
}

std::shared_ptr<LoomBinding::SurfaceToken>
LoomBinding::tokenFor(SurfaceId surfaceId) {
  std::lock_guard state(stateMutex_);
  auto iterator = surfaceTokens_.find(surfaceId);
  if (iterator != surfaceTokens_.end() && iterator->second->active) {
    return iterator->second;
  }
  auto& generation = surfaceGenerations_[surfaceId];
  if (generation == 0) {
    generation = 1;
  }
  auto token = std::make_shared<SurfaceToken>();
  token->surfaceId = surfaceId;
  token->generation = generation;
  surfaceTokens_[surfaceId] = token;
  return token;
}

const ComponentDescriptor* LoomBinding::compatDescriptorFor(
    const std::shared_ptr<const std::string>& componentName) {
  auto found = compatDescriptorByName_.find(componentName.get());
  if (found != compatDescriptorByName_.end()) {
    return found->second;
  }
  std::shared_ptr<LoomCompat> compat;
  {
    std::lock_guard state(stateMutex_);
    compat = compat_;
  }
  if (compat == nullptr) {
    // No renderer yet: decide on a later createNode.
    return nullptr;
  }
  const auto* descriptor = compat->descriptorFor(*componentName);
  compatDescriptorByName_.emplace(componentName.get(), descriptor);
  return descriptor;
}

void LoomBinding::parseCompatProps(
    jsi::Runtime& rt,
    DraftNode& draft,
    const jsi::Value& value) {
  std::shared_ptr<LoomCompat> compat;
  {
    std::lock_guard state(stateMutex_);
    compat = compat_;
  }
  if (compat == nullptr || !value.isObject()) {
    return;
  }
  try {
    PropsParserContext context{draft.surfaceId, compat->contextContainer()};
    draft.compatProps = draft.compatDescriptor->cloneProps(
        context, draft.compatProps, RawProps(rt, value));
  } catch (const std::exception& error) {
    // The renderer falls back to parsing the merged folly props.
    LOG(ERROR) << "Loom compat: cannot parse props for "
               << (draft.componentName ? *draft.componentName : "?") << ": "
               << error.what();
    draft.compatProps.reset();
  }
}

std::shared_ptr<LoomBinding::DraftNode>
LoomBinding::cloneDraft(
    const std::shared_ptr<NodeRef>& source,
    bool replaceChildren) {
  if (source == nullptr || source->draft == nullptr ||
      source->surfaceToken == nullptr || !source->surfaceToken->active) {
    return nullptr;
  }
  auto draft = std::make_shared<DraftNode>(*source->draft);
  // A clone is a new, unpublished version. The publication bookkeeping belongs
  // to the source version only: inheriting `published`/`workerCommitted` would
  // make `freeze` and selection treat the clone as an unchanged, already-live
  // subtree, so its new props and children would never reach the renderer.
  draft->published = false;
  draft->workerCommitted = false;
  draft->frozenCommit = 0;
  draft->freezeFacts = {};
  if (replaceChildren) {
    draft->children =
        std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
    draft->retainedLiveChildren.reset();
    draft->childrenExplicit = true;
  }
  return draft;
}

bool LoomBinding::propsContainTextEffects(const folly::dynamic& value) {
  if (value.isObject()) {
    for (const auto& item : value.items()) {
      if (item.first.isString() && item.first.getString() == "textEffects" &&
          !item.second.isNull())
        return true;
      if (propsContainTextEffects(item.second))
        return true;
    }
  } else if (value.isArray()) {
    for (const auto& item : value) {
      if (propsContainTextEffects(item))
        return true;
    }
  }
  return false;
}

void LoomBinding::linkPendingPatches(
    DraftNode& draft,
    PropsPatch& patch) {
  auto boundary = draft.propsTail;
  while (boundary && boundary->applied.load(std::memory_order_relaxed)) {
    draft.retainedUnsafeTextProps |= boundary->unsafeText;
    boundary = boundary->previous;
  }
  patch.previous = std::move(boundary);
}

bool LoomBinding::isRawTextRef(const NodeRef& ref) const {
  if (ref.draft != nullptr && ref.draft->componentName != nullptr) {
    return ref.draft->roles.isRawText();
  }
  const auto renderer = findRenderer(ref.surfaceId);
  return renderer != nullptr && renderer->componentRoles(ref.tag).isRawText();
}

std::shared_ptr<LoomRenderer> LoomBinding::findRenderer(
    SurfaceId surfaceId) const {
  std::lock_guard state(stateMutex_);
  const auto iterator = renderers_.find(surfaceId);
  return iterator == renderers_.end() ? nullptr : iterator->second;
}

#if LOOM_WORKER
bool LoomBinding::canMeasureTextOnWorker() {
  if (LoomWorker::kJavaScriptOnUIThread) {
    // Asked only when the answer can change the decision, and not cached:
    // `TextEffectRegistry.hasRegisteredFactories()` changes when a library
    // registers or unregisters a factory.
    auto* t = text();
    return t != nullptr && t->canMeasureOnWorker();
  }
  if (worker_->onUIThread())
    refreshCanMeasureTextOnWorker();
  return canMeasureTextOnWorker_.load() == 1;
}

void LoomBinding::refreshCanMeasureTextOnWorker() {
  if (!worker_->onUIThread())
    return;
  if (auto* t = text())
    canMeasureTextOnWorker_ = t->canMeasureOnWorker() ? 1 : 0;
}

std::optional<std::pair<float, float>> LoomBinding::readOnUIThread(
    std::function<std::optional<std::pair<float, float>>(LoomHost&)> read,
    std::optional<std::pair<float, float>> fallback) {
  if (worker_->onUIThread())
    return read(*host_);
  auto uiExecutor = host_->uiExecutor();
  if (!uiExecutor)
    return fallback;
  struct Reply {
    std::mutex mutex;
    std::condition_variable ready;
    bool done{false};
    std::optional<std::pair<float, float>> value;
  };
  auto reply = std::make_shared<Reply>();
  const std::weak_ptr<Lifetime> weakLifetime = lifetime_;
  uiExecutor([weakLifetime, reply, read = std::move(read)] {
    std::optional<std::pair<float, float>> value;
    if (auto lifetime = weakLifetime.lock()) {
      std::shared_lock held(lifetime->gate);
      if (auto* binding = lifetime->binding) {
        if (auto* uiHost = binding->host()) {
          try {
            value = read(*uiHost);
          } catch (...) {
          }
        }
      }
    }
    {
      std::lock_guard lock(reply->mutex);
      reply->value = value;
      reply->done = true;
    }
    reply->ready.notify_all();
  });
  // JS waits for the UI thread here, never the reverse.
  std::unique_lock lock(reply->mutex);
  if (!reply->ready.wait_for(lock, std::chrono::seconds(5), [&] {
        return reply->done;
      })) {
    loomLog(
        LoomLogLevel::Warn,
        "LoomWorker",
        "live view read waited 5 s for the UI thread");
    return fallback;
  }
  return reply->value;
}
#endif

LoomRenderer* LoomBinding::rendererOwning(Tag tag) {
  // Tags are unique across surfaces, so the first renderer that knows the tag
  // is the one that owns it.
  std::lock_guard state(stateMutex_);
  for (auto& entry : renderers_) {
    if (entry.second->exists(tag)) {
      return entry.second.get();
    }
  }
  return nullptr;
}

#pragma mark - Nodes

std::shared_ptr<LoomBinding::NodeRef>
LoomBinding::nodeRef(jsi::Runtime& runtime, const jsi::Value& value)
    const {
  if (!value.isObject()) {
    return nullptr;
  }
  auto ref = value.asObject(runtime).getHostObject<NodeRef>(runtime);
  if (ref == nullptr || ref->surfaceToken == nullptr ||
      !ref->surfaceToken->active ||
      ref->surfaceToken->surfaceId != ref->surfaceId ||
      (ref->tag != 0 &&
       (ref->draft == nullptr || ref->draft->identity == 0 ||
        ref->draft->surfaceGeneration != ref->surfaceToken->generation))) {
    return nullptr;
  }
  return ref;
}

bool LoomBinding::isLiveNodeRef(const NodeRef& ref) const {
  const auto renderer = findRenderer(ref.surfaceId);
  if (renderer == nullptr) {
    return false;
  }
  const auto tag = ref.tag == 0 ? renderer->rootTag() : ref.tag;
  if (tag == 0 || !renderer->exists(tag)) {
    return false;
  }
  if (tag == renderer->rootTag() && (ref.tag == 0 || ref.draft == nullptr)) {
    return true;
  }
  return ref.draft != nullptr && renderer->identityOf(tag) == ref.draft->identity;
}

std::vector<Tag> LoomBinding::tagsOf(
    const std::shared_ptr<ChildSet>& set) {
  std::vector<Tag> tags;
  if (set == nullptr) {
    return tags;
  }
  tags.reserve(set->nodes.size());
  for (const auto& node : set->nodes) {
    if (node != nullptr) {
      tags.push_back(node->tag);
    }
  }
  return tags;
}

#pragma mark - JS API

jsi::Value LoomBinding::get(
    jsi::Runtime& runtime,
    const jsi::PropNameID& name) {
#if LOOM_PROFILE
  // The JS-side cached Fabric host functions use this opt-in marker to measure
  // complete JS→JSI-call elapsed time. It is set only in diagnostic builds.
  runtime.global().setProperty(
      runtime, "__LOOM_PROFILE", true);
#endif
  const auto method = name.utf8(runtime);

  // Read once at module load by the Lists override: JavaScript shares the main
  // thread with the UI, so long synchronous render passes block frames.
  if (method == "loomJsOnMain") {
    return jsi::Value(LoomWorker::kJavaScriptOnUIThread);
  }

  // `Loom` (react-native-loom): the runtime settings registry. The wrapper reads
  // it once and keeps the object.
  if (method == "loomSettings") {
#if LOOM_WORKER
    constexpr bool kWorker = true;
#else
    constexpr bool kWorker = false;
#endif
    return jsi::Object::createFromHostObject(
        runtime,
        std::make_shared<LoomSettingsHostObject>(
            LoomWorker::kJavaScriptOnUIThread, kWorker));
  }

  // Read once at module load by the React renderer override: lets it use the
  // host operations only this renderer guarantees (updating a raw text node in
  // place, for one).
  if (method == "loomRenderer") {
    return jsi::Value(true);
  }

  // React Native's Pressability and other legacy UIManager call sites still
  // measure by React tag through FabricUIManager.measure(). In Loom renderer
  // mode, findShadowNodeByTag_DEPRECATED returns a NodeRef, but falling through
  // to UIManagerBinding::get would measure the backing shadow-tree snapshot.
  // That snapshot does not include the live native scroll offset, so responder
  // regions drift out of alignment as soon as a ScrollView moves.
  if (method == "measure" || method == "measureInWindow" ||
      method == "measureLayout") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        method == "measureLayout" ? 4 : 2,
        [this, method](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          return getDirectDOM(rt, method, arguments, count);
        });
  }

#if LOOM_WORKER
  if (method == "unstable_forceNextCommitSync") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        0,
        [this](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
          forceNextCommitSync_ = true;
          return jsi::Value::undefined();
        });
  }
  if (method == "configureNextLayoutAnimation") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* args,
            size_t count) {
          // Direct mounts do not feed LayoutAnimationKeyFrameManager. Never
          // accept an animation configuration whose callbacks cannot finish.
          forceNextCommitSync_ = true;
          if (count > 2 && args[2].isObject() &&
              args[2].asObject(rt).isFunction(rt)) {
            args[2].asObject(rt).asFunction(rt).call(rt);
          }
          return jsi::Value::undefined();
        });
  }
#endif

  if (method == "createNode") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        5,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "createNode", 5, count);
          auto surface = surfaceIdFromValue(rt, arguments[2]);
          LOOM_HOST_CALL_SCOPE(
              profile, *this, surface, ProfiledHostMethod::CreateNode);
          auto tag = tagFromValue(arguments[0]);
          auto token = tokenFor(surface);
          auto draft = std::make_shared<DraftNode>();
          draft->surfaceId = surface;
          draft->surfaceGeneration = token->generation;
          draft->identity = ++nextDraftIdentity_[surface];
          draft->tag = tag;
          const auto componentName = stringFromValue(rt, arguments[1]);
          // One shared name per component type instead of an allocation per
          // node; React only creates a handful of distinct host types.
          auto& interned = internedComponentNames_[componentName];
          if (interned == nullptr) {
            interned = std::make_shared<const std::string>(componentName);
          }
          draft->componentName = interned;
          classifyDraft(*draft);
          draft->compatDescriptor = compatDescriptorFor(interned);
          const auto rawText = draft->roles.isRawText()
              ? rawTextFromValue(rt, arguments[3])
              : std::nullopt;
          if (rawText.has_value()) {
            draft->initialRawText =
                std::make_shared<const std::string>(*rawText);
          } else {
            // Unknown raw-text props retain the general conversion path so
            // future host props are not silently discarded.
            draft->initialProps = std::make_shared<const folly::dynamic>(
                propsFromValue(rt, arguments[3]));
          }
          draft->hasInitialProps = true;
          if (draft->compatDescriptor != nullptr) {
            parseCompatProps(rt, *draft, arguments[3]);
          }
          draft->instanceHandle =
              instanceHandleFromValue(rt, arguments[4], arguments[0]);
          return jsi::Object::createFromHostObject(
              rt,
              std::make_shared<NodeRef>(
                  surface, tag, std::move(token), std::move(draft)));
        });
  }

  if (method == "cloneNode") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNode", 1, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile, *this, ref->surfaceId, ProfiledHostMethod::CloneNode);
          auto draft = cloneDraft(ref);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  if (method == "cloneNodeWithNewProps") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeWithNewProps", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewProps);
          auto draft = cloneDraft(ref);
          auto patch = std::make_shared<PropsPatch>();
          if (isRawTextRef(*ref)) {
            patch->rawText = rawTextFromValue(rt, arguments[1]);
          }
          if (!patch->rawText.has_value()) {
            patch->props = propsFromValue(rt, arguments[1]);
            patch->unsafeText = propsContainTextEffects(patch->props);
            if (draft->compatDescriptor != nullptr) {
              parseCompatProps(rt, *draft, arguments[1]);
            }
          }
          linkPendingPatches(*draft, *patch);
          draft->propsTail = std::move(patch);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  if (method == "cloneNodeWithNewChildren") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeWithNewChildren", 1, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewChildren);
          auto draft = cloneDraft(ref, true);
          if (count > 1 && arguments[1].isObject()) {
            auto set = arguments[1].asObject(rt).getHostObject<ChildSet>(rt);
            if (set != nullptr) {
              for (const auto& child : set->nodes) {
                if (child != nullptr && child->draft != nullptr &&
                    child->surfaceToken == ref->surfaceToken) {
                  draft->children->push_back(child->draft);
                }
              }
            }
          }
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  if (method == "cloneNodeWithNewChildrenAndProps") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeWithNewChildrenAndProps", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewChildrenAndProps);
          // The host API allows both arities:
          //   cloneNodeWithNewChildrenAndProps(node, props)
          //   cloneNodeWithNewChildrenAndProps(node, children, props)
          // Without a children argument the children are dropped, exactly as
          // the shadow-tree path does.
          const bool hasChildren = count == 3;
          auto draft = cloneDraft(ref, true);
          if (hasChildren) {
            auto set = arguments[1].asObject(rt).getHostObject<ChildSet>(rt);
            if (set != nullptr) {
              for (const auto& child : set->nodes) {
                if (child != nullptr && child->draft != nullptr &&
                    child->surfaceToken == ref->surfaceToken) {
                  draft->children->push_back(child->draft);
                }
              }
            }
          }
          auto patch = std::make_shared<PropsPatch>();
          const auto& propsValue = arguments[hasChildren ? 2 : 1];
          if (isRawTextRef(*ref)) {
            patch->rawText = rawTextFromValue(rt, propsValue);
          }
          if (!patch->rawText.has_value()) {
            patch->props = propsFromValue(rt, propsValue);
            patch->unsafeText = propsContainTextEffects(patch->props);
            if (draft->compatDescriptor != nullptr) {
              parseCompatProps(rt, *draft, propsValue);
            }
          }
          linkPendingPatches(*draft, *patch);
          draft->propsTail = std::move(patch);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

  // Loom: clone a parent whose host children are the same nodes in the same
  // order, swapping in the new versions of only the children React cloned.
  // React calls this instead of cloneNodeWithNewChildren(AndProps) plus one
  // appendChild per child. Returns null when the source version does not hold
  // an explicit child list, or a changed child is not one of its children, and
  // React then takes the regular path.
  //   cloneNodeReplacingChildren(node, changedChildren, propsOrNull)
  if (method == "cloneNodeReplacingChildren") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "cloneNodeReplacingChildren", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr || ref->draft == nullptr ||
              !ref->draft->childrenExplicit ||
              ref->draft->retainedLiveChildren != nullptr) {
            return jsi::Value::null();
          }
          LOOM_HOST_CALL_SCOPE(
              profile,
              *this,
              ref->surfaceId,
              ProfiledHostMethod::CloneNodeWithNewChildren);
          // The changed children arrive as null (none), a single node (the
          // common case, no array to build or read through JSI), or an array.
          std::vector<std::shared_ptr<DraftNode>> changed;
          const auto acceptChild = [&](const jsi::Value& value) {
            auto child = nodeRef(rt, value);
            if (child == nullptr || child->draft == nullptr ||
                child->surfaceToken != ref->surfaceToken) {
              return false;
            }
            changed.push_back(child->draft);
            return true;
          };
          if (arguments[1].isObject()) {
            auto object = arguments[1].asObject(rt);
            if (object.isHostObject(rt)) {
              if (!acceptChild(arguments[1])) {
                return jsi::Value::null();
              }
            } else if (object.isArray(rt)) {
              auto changedArray = object.getArray(rt);
              const auto changedCount = changedArray.size(rt);
              changed.reserve(changedCount);
              for (size_t index = 0; index < changedCount; ++index) {
                if (!acceptChild(changedArray.getValueAtIndex(rt, index))) {
                  return jsi::Value::null();
                }
              }
            } else {
              return jsi::Value::null();
            }
          } else if (!arguments[1].isNull() && !arguments[1].isUndefined()) {
            return jsi::Value::null();
          }
          const auto& sourceChildren = *ref->draft->children;
          std::shared_ptr<std::vector<std::shared_ptr<DraftNode>>> children;
          if (!changed.empty()) {
            children =
                std::make_shared<std::vector<std::shared_ptr<DraftNode>>>(
                    sourceChildren);
            size_t replaced = 0;
            if (changed.size() <= 8) {
              for (auto& slot : *children) {
                if (slot == nullptr) {
                  continue;
                }
                for (const auto& next : changed) {
                  if (slot->tag == next->tag) {
                    slot = next;
                    ++replaced;
                    break;
                  }
                }
              }
            } else {
              TagMap<size_t> changedIndex;
              changedIndex.reserve(changed.size());
              for (size_t index = 0; index < changed.size(); ++index) {
                changedIndex.emplace(changed[index]->tag, index);
              }
              for (auto& slot : *children) {
                if (slot == nullptr) {
                  continue;
                }
                const auto found = changedIndex.find(slot->tag);
                if (found != changedIndex.end()) {
                  slot = changed[found->second];
                  ++replaced;
                }
              }
            }
            if (replaced != changed.size()) {
              return jsi::Value::null();
            }
          }
          auto draft = cloneDraft(ref);
          if (draft == nullptr) {
            return jsi::Value::null();
          }
          if (children != nullptr) {
            draft->children = std::move(children);
          }
          if (count > 2 && arguments[2].isObject()) {
            auto patch = std::make_shared<PropsPatch>();
            patch->props = propsFromValue(rt, arguments[2]);
            patch->unsafeText = propsContainTextEffects(patch->props);
            if (draft->compatDescriptor != nullptr) {
              parseCompatProps(rt, *draft, arguments[2]);
            }
            linkPendingPatches(*draft, *patch);
            draft->propsTail = std::move(patch);
          }
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(*ref, std::move(draft)));
        });
  }

#if LOOM_WORKER
  // In-place updates (see `pendingPatches_`).
  //   loomPatchNode(node, payload): payload is a props diff, or a string for
  //   a raw text node. Returns true when recorded.
  if (method == "loomPatchNode") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "loomPatchNode", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref == nullptr || ref->draft == nullptr) {
            return jsi::Value(false);
          }
          PendingPatch patch;
          patch.tag = ref->tag;
          patch.identity = ref->draft->identity;
          if (arguments[1].isString()) {
            patch.rawText = arguments[1].asString(rt).utf8(rt);
          } else if (arguments[1].isObject()) {
            patch.props = propsFromValue(rt, arguments[1]);
          } else {
            return jsi::Value(false);
          }
          pendingPatches_[ref->surfaceId].push_back(std::move(patch));
          return jsi::Value(true);
        });
  }
  //   loomFlushPatches(surfaceId)
  if (method == "loomFlushPatches") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "loomFlushPatches", 1, count);
          flushPendingPatches(surfaceIdFromValue(rt, arguments[0]));
          return jsi::Value::undefined();
        });
  }
#endif

  if (method == "createChildSet") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t)
            -> jsi::Value {
          auto set = std::make_shared<ChildSet>();
#if LOOM_PROFILE
          const auto start = std::chrono::steady_clock::now();
          set->createdAt = start;
          set->createdAtEpochNanoseconds =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
          set->createCallNanoseconds = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
#endif
          return jsi::Object::createFromHostObject(rt, std::move(set));
        });
  }

  if (method == "appendChildToSet") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "appendChildToSet", 2, count);
          auto set = arguments[0].asObject(rt).getHostObject<ChildSet>(rt);
          auto child = nodeRef(rt, arguments[1]);
          if (set != nullptr && child != nullptr && child->draft != nullptr) {
#if LOOM_PROFILE
            auto generation = profileCommitGeneration_[child->surfaceId];
            if (generation == 0) {
              generation = 1;
              profileCommitGeneration_[child->surfaceId] = generation;
            }
            auto& hostProfile = profileHostCalls_[child->surfaceId][generation];
            if (hostProfile.firstHostCallEpochNanoseconds == 0) {
              hostProfile.firstHostCallEpochNanoseconds =
                  set->createdAtEpochNanoseconds;
              hostProfile.firstHostCallSteadyNanoseconds =
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      set->createdAt.time_since_epoch())
                      .count();
            }
            LOOM_HOST_CALL_SCOPE(
                profile,
                *this,
                child->surfaceId,
                ProfiledHostMethod::AppendChildToSet);
#endif
            if (set->surfaceToken == nullptr) {
              set->surfaceToken = child->surfaceToken;
              set->surfaceId = child->surfaceId;
            }
            if (set->surfaceToken == child->surfaceToken) {
              set->nodes.push_back(std::move(child));
            }
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "appendChild") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "appendChild", 2, count);
          auto parent = nodeRef(rt, arguments[0]);
          auto child = nodeRef(rt, arguments[1]);
          if (parent != nullptr && child != nullptr &&
              parent->draft != nullptr && child->draft != nullptr &&
              parent->surfaceToken == child->surfaceToken) {
            LOOM_HOST_CALL_SCOPE(
                profile,
                *this,
                parent->surfaceId,
                ProfiledHostMethod::AppendChild);
#if LOOM_WORKER
            if (parent->draft->published)
              parent->draft = cloneDraft(parent);
#endif
            auto& draft = *parent->draft;
            if (!draft.childrenExplicit) {
#if LOOM_WORKER
              worker_->synchronize();
#endif
              std::vector<RetainedLiveChild> currentChildren;
              auto owner = renderers_.find(parent->surfaceId);
              if (owner != renderers_.end() &&
                  owner->second->identityOf(parent->tag) == draft.identity) {
                for (const auto childTag :
                     owner->second->childrenOf(parent->tag)) {
                  currentChildren.push_back(
                      {childTag, owner->second->identityOf(childTag)});
                }
              }
              draft.children =
                  std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
              draft.retainedLiveChildren =
                  std::make_shared<const std::vector<RetainedLiveChild>>(
                      std::move(currentChildren));
              draft.childrenExplicit = true;
            } else if (draft.children.use_count() != 1) {
              draft.children =
                  std::make_shared<std::vector<std::shared_ptr<DraftNode>>>(
                      *draft.children);
            }
            draft.children->push_back(child->draft);
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "completeRoot") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "completeRoot", 2, count);
          auto surface = surfaceIdFromValue(rt, arguments[0]);
          [[maybe_unused]] uint64_t generation = 0;
#if LOOM_PROFILE
          generation = profileCommitGeneration_[surface];
          if (generation == 0) {
            generation = 1;
          }
          auto& currentHostProfiles = profileHostCalls_[surface];
          HostProfile hostProfile;
          if (auto existing = currentHostProfiles.find(generation);
              existing != currentHostProfiles.end()) {
            hostProfile = std::move(existing->second);
            currentHostProfiles.erase(existing);
          }
          if (hostProfile.firstHostCallEpochNanoseconds == 0) {
            hostProfile.firstHostCallEpochNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
          }
          profileCommitGeneration_[surface] = generation + 1;
          const auto completeRootStart = std::chrono::steady_clock::now();
          const auto completeRootStartSteadyNanoseconds =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  completeRootStart.time_since_epoch())
                  .count();
          const auto hostCallsToCompleteRootMs =
              hostProfile.firstHostCallSteadyNanoseconds == 0
              ? 0.0
              : std::max(
                    0.0,
                    (completeRootStartSteadyNanoseconds -
                     hostProfile.firstHostCallSteadyNanoseconds) /
                        1.0e6);
#endif
          auto set = arguments[1].asObject(rt).getHostObject<ChildSet>(rt);
          auto token = tokenFor(surface);
          if (set != nullptr && set->surfaceToken != nullptr &&
              set->surfaceToken != token) {
            throw jsi::JSError(
                rt, "completeRoot received a stale or foreign child set");
          }
          const auto selectedRoots = set == nullptr
              ? std::vector<std::shared_ptr<NodeRef>>{}
              : set->nodes;
#if LOOM_PROFILE
          if (set != nullptr) {
            const auto createChildSetIndex =
                static_cast<size_t>(ProfiledHostMethod::CreateChildSet);
            ++hostProfile.methods[createChildSetIndex].calls;
            hostProfile.methods[createChildSetIndex].totalNanoseconds +=
                set->createCallNanoseconds;
          }
          const auto completeRootProfileIndex =
              static_cast<size_t>(ProfiledHostMethod::CompleteRoot);
          ++hostProfile.methods[completeRootProfileIndex].calls;
#endif
#if LOOM_WORKER
          // In-place patches recorded by this commit go first, so they are
          // never queued behind (or merged into) the structural commit.
          flushPendingPatches(surface);
          const bool forceSync = forceNextCommitSync_ ||
              (count > 2 && arguments[2].isBool() && arguments[2].getBool());
          forceNextCommitSync_ = false;
          publishWorkerCommit(surface, selectedRoots, forceSync);
#else
          publishRootCommit(surface, selectedRoots, token, generation);
#endif
#if LOOM_PROFILE
          hostProfile.methods[completeRootProfileIndex].totalNanoseconds +=
              static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - completeRootStart)
                      .count());
          if (LoomWorkerProfile::enabled()) {
            const auto method =
                [&hostProfile](
                    ProfiledHostMethod value) -> const HostMethodStats& {
              return hostProfile.methods[static_cast<size_t>(value)];
            };
            const auto methodMs = [](const HostMethodStats& stats) {
              return static_cast<double>(stats.totalNanoseconds) / 1.0e6;
            };
            loomLog(
                LoomLogLevel::Profile,
                "LoomHostProfile",
                "surface=%d gen=%llu firstHostToCompleteRootMs=%.3f "
                "createNode=%llu/%.3f clone=%llu/%.3f cloneProps=%llu/%.3f "
                "cloneChildren=%llu/%.3f cloneChildrenProps=%llu/%.3f "
                "createSet=%llu/%.3f appendSet=%llu/%.3f appendChild=%llu/%.3f "
                "completeRoot=%llu/%.3f",
                static_cast<int>(surface),
                static_cast<unsigned long long>(generation),
                hostCallsToCompleteRootMs,
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CreateNode).calls),
                methodMs(method(ProfiledHostMethod::CreateNode)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNode).calls),
                methodMs(method(ProfiledHostMethod::CloneNode)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNodeWithNewProps).calls),
                methodMs(method(ProfiledHostMethod::CloneNodeWithNewProps)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNodeWithNewChildren).calls),
                methodMs(method(ProfiledHostMethod::CloneNodeWithNewChildren)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CloneNodeWithNewChildrenAndProps)
                        .calls),
                methodMs(
                    method(ProfiledHostMethod::CloneNodeWithNewChildrenAndProps)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CreateChildSet).calls),
                methodMs(method(ProfiledHostMethod::CreateChildSet)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::AppendChildToSet).calls),
                methodMs(method(ProfiledHostMethod::AppendChildToSet)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::AppendChild).calls),
                methodMs(method(ProfiledHostMethod::AppendChild)),
                static_cast<unsigned long long>(
                    method(ProfiledHostMethod::CompleteRoot).calls),
                methodMs(method(ProfiledHostMethod::CompleteRoot)));
          }
#endif
          return jsi::Value::undefined();
        });
  }

  if (method == "findShadowNodeByTag_DEPRECATED") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
#if LOOM_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "findShadowNodeByTag_DEPRECATED", 1, count);
          auto tag = tagFromValue(arguments[0]);
          auto* owner = rendererOwning(tag);
          if (owner == nullptr) {
            return jsi::Value::null();
          }
          auto token = tokenFor(owner->surfaceId());
          auto draft = std::make_shared<DraftNode>();
          draft->surfaceId = owner->surfaceId();
          draft->surfaceGeneration = token->generation;
          draft->identity = owner->identityOf(tag);
          draft->tag = tag;
          draft->componentName =
              std::make_shared<const std::string>(owner->componentName(tag));
          classifyDraft(*draft);
          draft->instanceHandle = owner->instanceHandle(tag);
          draft->committedSnapshot = true;
          draft->childrenExplicit = false;
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(owner->surfaceId(), tag, std::move(draft)));
        });
  }

  if (method == "setNativeProps") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
          requireArguments(rt, "setNativeProps", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
#if LOOM_WORKER
            setWorkerNativeProps(*ref, propsFromValue(rt, arguments[1]));
#else
            auto owner = renderers_.find(ref->surfaceId);
            if (owner != renderers_.end() && isLiveNodeRef(*ref)) {
              owner->second->setNativeProps(
                  ref->tag, propsFromValue(rt, arguments[1]));
            }
#endif
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "dispatchCommand") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
requireArguments(rt, "dispatchCommand", 3, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            // Ordered after the commits the worker already owns instead of
            // blocking this thread on them: the guard reads the renderer's node
            // registry, which the worker mutates while it computes.
            auto commandName = stringFromValue(rt, arguments[1]);
            auto commandArgs = commandArgsFromValue(rt, arguments[2]);
            submitOrderedUIEffect(
                ref->surfaceId,
                [this,
                 surfaceId = ref->surfaceId,
                 ref,
                 commandName = std::move(commandName),
                 commandArgs = std::move(commandArgs)] {
                  auto owner = findRenderer(surfaceId);
                  if (owner != nullptr && isLiveNodeRef(*ref)) {
                    owner->dispatchCommand(
                        ref->tag, commandName, commandArgs);
                  }
                });
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "sendAccessibilityEvent") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
requireArguments(rt, "sendAccessibilityEvent", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            auto eventType = stringFromValue(rt, arguments[1]);
            submitOrderedUIEffect(
                ref->surfaceId,
                [this,
                 surfaceId = ref->surfaceId,
                 ref,
                 eventType = std::move(eventType)] {
                  auto owner = findRenderer(surfaceId);
                  if (owner != nullptr && isLiveNodeRef(*ref)) {
                    owner->sendAccessibilityEvent(ref->tag, eventType);
                  }
                });
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "setIsJSResponder") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        3,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
requireArguments(rt, "setIsJSResponder", 3, count);
          auto ref = nodeRef(rt, arguments[0]);
          if (ref != nullptr) {
            // Called on every touch grant, so blocking here stalled the
            // responder handoff behind whatever the worker was computing.
            const auto isJSResponder = arguments[1].getBool();
            const auto blockNativeResponder = arguments[2].getBool();
            submitOrderedUIEffect(
                ref->surfaceId,
                [this,
                 surfaceId = ref->surfaceId,
                 ref,
                 isJSResponder,
                 blockNativeResponder] {
                  auto owner = findRenderer(surfaceId);
                  if (owner != nullptr && isLiveNodeRef(*ref)) {
                    owner->setIsJSResponder(
                        ref->tag, isJSResponder, blockNativeResponder);
                  }
                });
          }
          return jsi::Value::undefined();
        });
  }

  if (method == "getRelativeLayoutMetrics") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
#if LOOM_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "getRelativeLayoutMetrics", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto ancestorRef = nodeRef(rt, arguments[1]);

          LoomRenderer::Metrics metrics;
          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          const auto ancestorTag =
              ancestorRef == nullptr ? 0 : ancestorRef->tag;
          if (owner == nullptr ||
              !owner->metricsRelativeTo(ref->tag, ancestorTag, metrics)) {
            metrics = LoomRenderer::Metrics{};
          }

          auto result = jsi::Object(rt);
          result.setProperty(rt, "left", metrics.x);
          result.setProperty(rt, "top", metrics.y);
          result.setProperty(rt, "width", metrics.width);
          result.setProperty(rt, "height", metrics.height);
          return result;
        });
  }

  if (method == "getBoundingClientRect") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
#if LOOM_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "getBoundingClientRect", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          const bool includeTransform = arguments[1].getBool();

          LoomRenderer::Metrics metrics;
          if (ref == nullptr ||
              !windowMetrics(ref->tag, metrics, includeTransform)) {
            return jsi::Value::undefined();
          }

          return jsi::Array::createWithElements(
              rt,
              jsi::Value{rt, static_cast<double>(metrics.x)},
              jsi::Value{rt, static_cast<double>(metrics.y)},
              jsi::Value{rt, static_cast<double>(metrics.width)},
              jsi::Value{rt, static_cast<double>(metrics.height)});
        });
  }

  if (method == "measureInstance") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
#if LOOM_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "measureInstance", 1, count);
          auto ref = nodeRef(rt, arguments[0]);

          LoomRenderer::Metrics metrics;
          if (ref != nullptr) {
            windowMetrics(ref->tag, metrics);
          }

          auto result = jsi::Object(rt);
          result.setProperty(rt, "x", metrics.x);
          result.setProperty(rt, "y", metrics.y);
          result.setProperty(rt, "width", metrics.width);
          result.setProperty(rt, "height", metrics.height);
          return result;
        });
  }

  if (method == "compareDocumentPosition") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        2,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
#if LOOM_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "compareDocumentPosition", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto otherRef = nodeRef(rt, arguments[1]);

          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          if (owner == nullptr || otherRef == nullptr ||
              !owner->exists(otherRef->tag)) {
            return jsi::Value{
                rt, static_cast<double>(kDocumentPositionDisconnected)};
          }
          return jsi::Value{
              rt,
              static_cast<double>(
                  owner->compareDocumentPosition(ref->tag, otherRef->tag))};
        });
  }

  if (method == "findNodeAtPoint") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        4,
        [this](
            jsi::Runtime& rt,
            const jsi::Value&,
            const jsi::Value* arguments,
            size_t count) -> jsi::Value {
#if LOOM_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "findNodeAtPoint", 4, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto callback = arguments[3].getObject(rt).getFunction(rt);

          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          if (owner == nullptr) {
            callback.call(rt, jsi::Value::null());
            return jsi::Value::undefined();
          }

          const auto hit = owner->findNodeAtPoint(
              ref->tag,
              static_cast<float>(arguments[1].getNumber()),
              static_cast<float>(arguments[2].getNumber()));
          auto handle = hit == 0 ? nullptr : owner->instanceHandle(hit);
          if (handle == nullptr) {
            callback.call(rt, jsi::Value::null());
            return jsi::Value::undefined();
          }

          // The handle is stored weak, so it has to be retained to be handed
          // back to JavaScript - the same dance the shadow-tree binding does.
          EventTarget target(handle, ref->surfaceId);
          target.setEnabled(true);
          target.retain(rt);
          auto instanceHandle = target.getInstanceHandle(rt);
          target.release(rt);

          callback.call(rt, std::move(instanceHandle));
          return jsi::Value::undefined();
        });
  }

  if (method == "applyViewTransitionName" ||
      method == "cancelViewTransitionName" ||
      method == "restoreViewTransitionName") {
    // View transitions are driven from shadow nodes, which do not exist in
    // this mode. Answering with a no-op keeps a node-taking method from
    // throwing; there is nothing meaningful to apply.
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        1,
        [](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t)
            -> jsi::Value { return jsi::Value::undefined(); });
  }

  // Nothing that takes a node is left: event handler registration, layout
  // animation configuration and the event priority constants are all
  // node-free.
  return UIManagerBinding::get(runtime, name);
}

} // namespace facebook::react
#endif
