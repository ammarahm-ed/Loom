/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UIThreadRendererBinding.h"
#include "UIThreadRendererInternals.h"
#include "UIThreadRendererWorkerProfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <react/renderer/runtimescheduler/JankReporter.h>
#include <react/renderer/runtimescheduler/RuntimeSchedulerFrameCallbackScope.h>
#include <shared_mutex>
#include <unordered_set>

#if defined(__ANDROID__)
#include <android/log.h>
#include <fbjni/fbjni.h>
#endif

#include <jsi/JSIDynamic.h>

#if UI_THREAD_RENDERER
namespace facebook::react {

#if RN_PROFILE_UI_THREAD_RENDERER
#define RNX_HOST_CALL_SCOPE(name, ...) \
  UIThreadRendererBinding::HostCallScope name(__VA_ARGS__)
#else
#define RNX_HOST_CALL_SCOPE(name, ...)
#endif
struct UIThreadRendererBinding::Lifetime {
  std::shared_mutex gate;
  UIThreadRendererBinding* binding{nullptr};
};

namespace {

using uiThreadRenderer::isParagraphName;
using uiThreadRenderer::isRawTextName;
using uiThreadRenderer::isVirtualTextName;
using uiThreadRenderer::propsFromValue;
using uiThreadRenderer::requireArguments;

/**
 * Whether a node's live child list differs from the list this commit selected.
 * Uses the borrowed accessor: the copying form allocated a vector per node on
 * every commit just to compare it.
 */
bool childrenDifferFrom(
    const UIThreadRenderer& renderer,
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
std::unordered_map<SurfaceId, UIThreadRendererBinding::SurfaceConstraints>&
surfaceConstraints() {
  static std::
      unordered_map<SurfaceId, UIThreadRendererBinding::SurfaceConstraints>
          constraints;
  return constraints;
}

/** Bindings that currently own renderers, so a static call can reach them. */
std::unordered_map<
    UIThreadRendererBinding*,
    std::shared_ptr<UIThreadRendererBinding::Lifetime>>&
liveBindings() {
  static std::unordered_map<
      UIThreadRendererBinding*,
      std::shared_ptr<UIThreadRendererBinding::Lifetime>>
      bindings;
  return bindings;
}

std::vector<std::shared_ptr<UIThreadRendererBinding::Lifetime>>
bindingSnapshot() {
  std::vector<std::shared_ptr<UIThreadRendererBinding::Lifetime>> snapshot;
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

UIThreadRendererBinding::UIThreadRendererBinding(
    std::shared_ptr<UIManager> uiManager)
    : UIManagerBinding(std::move(uiManager)) {
  lifetime_ = std::make_shared<Lifetime>();
  lifetime_->binding = this;
#if UI_THREAD_RENDERER_WORKER
  const std::weak_ptr<Lifetime> weakLifetime = lifetime_;
  auto* delegate = getUIManager().getDelegate();
  auto uiExecutor =
      delegate ? delegate->uiThreadRendererGetUIExecutor() : nullptr;
  if (!uiExecutor)
    throw std::logic_error(
        "Direct worker renderer requires a platform UI executor");
  worker_ = std::make_unique<UIThreadRendererWorker>(
      [weakLifetime, uiExecutor] {
        uiExecutor([weakLifetime] {
          if (auto lifetime = weakLifetime.lock()) {
            // Result application can stop/restart a surface synchronously. The
            // runtime owns this binding throughout the callback; no registry
            // lock is held across UI effects.
            if (auto* binding = lifetime->binding) {
              if (!UIThreadRendererWorker::kJavaScriptOnUIThread)
                binding->refreshCanMeasureTextOnWorker();
              binding->worker_->applyReady();
            }
          }
        });
      });
  if (!UIThreadRendererWorker::kJavaScriptOnUIThread) {
    // JS runs off the UI thread, where the Java text-capability query is not
    // allowed; seed the cached answer from the UI thread before the first commit
    // needs it.
    uiExecutor([weakLifetime] {
      if (auto lifetime = weakLifetime.lock()) {
        if (auto* binding = lifetime->binding)
          binding->refreshCanMeasureTextOnWorker();
      }
    });
  }
#endif
#if UI_THREAD_RENDERER_WORKER && RN_UI_THREAD
  // React Native X: when timers and requestAnimationFrame callbacks have run in
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

#if RN_PROFILE_UI_THREAD_RENDERER
UIThreadRendererBinding::HostCallScope::HostCallScope(
    UIThreadRendererBinding& binding,
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

UIThreadRendererBinding::HostCallScope::~HostCallScope() {
  const auto finished = std::chrono::steady_clock::now();
  auto& profile = binding.profileHostCalls_[surfaceId][generation];
  auto& stats = profile.methods[static_cast<size_t>(method)];
  ++stats.calls;
  stats.totalNanoseconds += static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - start)
          .count());
}
#endif

UIThreadRendererBinding::~UIThreadRendererBinding() {
#if UI_THREAD_RENDERER_WORKER
  for (auto& [surface, token] : surfaceTokens_)
    token->active = false;
  worker_->shutdown();
#if RN_UI_THREAD
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

void UIThreadRendererBinding::setSurfaceConstraints(
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

void UIThreadRendererBinding::setPointScaleFactor(float pointScaleFactor) {
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
    auto* delegate = binding->getUIManager().getDelegate();
    auto uiExecutor =
        delegate ? delegate->uiThreadRendererGetUIExecutor() : nullptr;
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

void UIThreadRendererBinding::applyPointScaleFactor(float pointScaleFactor) {
#if UI_THREAD_RENDERER_WORKER
  // Density and font scale changes can change what the platform text path
  // supports; this is the UI thread, where the answer may be asked.
  refreshCanMeasureTextOnWorker();
  std::vector<std::pair<SurfaceId, std::shared_ptr<UIThreadRenderer>>> owners;
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

void UIThreadRendererBinding::submitOrderedUIEffect(
    SurfaceId surfaceId,
    std::function<void()> effect) {
#if UI_THREAD_RENDERER_WORKER
  auto job = std::make_unique<UIThreadRendererWorker::Job>();
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

void UIThreadRendererBinding::synchronizeWorkerForUIManager(
    UIManager& manager) {
#if UI_THREAD_RENDERER_WORKER
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

void UIThreadRendererBinding::shutdownWorkerForUIManager(UIManager& manager) {
#if UI_THREAD_RENDERER_WORKER
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

void UIThreadRendererBinding::recordNativeProps(Tag tag, folly::dynamic props) {
#if UI_THREAD_RENDERER_WORKER
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
    auto classified = classifyAnimatedProps(props);
    if (classified.text || owner->second->isTextNode(tag)) {
      // Font and text keys drive measurement, and any key on a paragraph or raw
      // text node can, which the synchronous lane must not do: the whole payload
      // keeps the ordinary per-call path (design §3.2, "Text").
      binding->stageNativeProps(tag, identityIt->second, token, std::move(props));
      return;
    }
    // The platform's synchronous update has already put these values on the
    // view, so this call only folds them into the frame being collected.
    binding->writeAnimatedProps(
        token->surfaceId,
        tag,
        identityIt->second,
        std::move(classified),
        /*applyPaintToView=*/false);
    return;
  }
#else
  (void)tag;
  (void)props;
#endif
}

#if UI_THREAD_RENDERER_WORKER
std::shared_ptr<const folly::dynamic> UIThreadRendererBinding::stageNativeProps(
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
// Animation fast lane (docs/resha/animation-fast-lane-design.md).
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

void mergePatchesInto(
    std::unordered_map<Tag, folly::dynamic>& target,
    const std::vector<std::pair<Tag, folly::dynamic>>& patches) {
  for (const auto& entry : patches) {
    auto& merged = target[entry.first];
    if (!merged.isObject())
      merged = folly::dynamic::object();
    for (const auto& item : entry.second.items())
      merged[item.first] = item.second;
  }
}
} // namespace

bool UIThreadRendererBinding::AnimationFrame::merge(
    const std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    const std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches) {
  std::lock_guard lock(mutex);
  if (claimed)
    return false;
  mergePatchesInto(paint, paintPatches);
  mergePatchesInto(layout, layoutPatches);
  // The tags are recorded as they arrive, not when the frame is taken: the
  // applied callback that drops their paint overlays can run before the compute
  // does (a synchronous job applies first, then computes).
  for (const auto& entry : paintPatches)
    tags.push_back(entry.first);
  for (const auto& entry : layoutPatches)
    tags.push_back(entry.first);
  return true;
}

bool UIThreadRendererBinding::AnimationFrame::take(
    std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches) {
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
  return true;
}

void UIThreadRendererBinding::writeAnimatedProps(
    SurfaceId surfaceId,
    Tag tag,
    uint64_t identity,
    ClassifiedAnimatedProps classified,
    bool applyPaintToView) {
  std::unique_lock state(stateMutex_);
  auto& write = animationFrameBuffer_[surfaceId][tag];
  if (write.identity != identity) {
    // A different draft owns the tag now, so nothing collected for the previous
    // one may survive into this frame.
    write.paint = folly::dynamic::object();
    write.layout = folly::dynamic::object();
    write.text = false;
    write.identity = identity;
  }
  for (const auto& item : classified.paint.items()) {
    write.paint[item.first] = item.second;
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
  for (const auto& item : classified.paint.items()) {
    overlay.props[item.first] = item.second;
  }
  overlay.surfaceId = surfaceId;
  overlay.identity = identity;
  overlay.sequence = 0;
  // The view write is a Java call that re-enters `recordNativeProps`.
  state.unlock();
  if (applyPaintToView) {
    applyAnimatedPaintToView(tag, classified.paint);
  }
}

void UIThreadRendererBinding::applyAnimatedPaintToView(
    Tag tag,
    const folly::dynamic& props) {
  if (props.empty()) {
    return;
  }
  auto* delegate = getUIManager().getDelegate();
  if (delegate == nullptr) {
    return;
  }
  const auto previousTag = replayingNativePropsTag_;
  const auto* previousProps = replayingNativeProps_;
  replayingNativePropsTag_ = tag;
  replayingNativeProps_ = &props;
  try {
    delegate->uiManagerShouldSynchronouslyUpdateViewOnUIThread(tag, props);
  } catch (...) {
    replayingNativePropsTag_ = previousTag;
    replayingNativeProps_ = previousProps;
    throw;
  }
  replayingNativePropsTag_ = previousTag;
  replayingNativeProps_ = previousProps;
}

void UIThreadRendererBinding::erasePaintOverlays(
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

void UIThreadRendererBinding::recordAnimationLaneCost(
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

bool UIThreadRendererBinding::animationLaneAllowsSync(SurfaceId surfaceId) {
  std::lock_guard state(stateMutex_);
  const auto iterator = animationLaneStats_.find(surfaceId);
  if (iterator == animationLaneStats_.end() || !iterator->second.asyncMode) {
    return true;
  }
  // One probe every `kAnimationLaneProbeFrames` frames measures whether the
  // surface has calmed down; without a measurement there is no way back.
  return ++iterator->second.asyncProbeCounter % kAnimationLaneProbeFrames == 0;
}

void UIThreadRendererBinding::traceAnimationLane(
    SurfaceId surfaceId,
    bool synchronous,
    double micros,
    size_t nodes) {
#if RN_PROFILE_UI_THREAD_RENDERER
  if (!UIThreadRendererWorkerProfile::enabled()) {
    return;
  }
#if defined(__ANDROID__)
  __android_log_print(
      ANDROID_LOG_INFO,
      "RNXAnimLane",
      "sync=%d cost=%.0fus nodes=%zu surface=%d",
      synchronous ? 1 : 0,
      micros,
      nodes,
      static_cast<int>(surfaceId));
#elif defined(__APPLE__)
  os_log(
      OS_LOG_DEFAULT,
      "RNXAnimLane sync=%d cost=%.0fus nodes=%lu surface=%d",
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
#else
  (void)surfaceId;
  (void)synchronous;
  (void)micros;
  (void)nodes;
#endif
}

void UIThreadRendererBinding::flushAnimationFrames() {
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

void UIThreadRendererBinding::flushAnimationFrame(SurfaceId surfaceId) {
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
  }
  state.unlock();
  if (paintPatches.empty() && layoutPatches.empty()) {
    return;
  }
  deferAnimationFrame(surfaceId, paintPatches, layoutPatches, needsAsync);
}

void UIThreadRendererBinding::deferAnimationFrame(
    SurfaceId surfaceId,
    const std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    const std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
    bool needsAsync) {
  std::unique_lock state(stateMutex_);
  if (auto pending = pendingAnimationFrames_.find(surfaceId);
      pending != pendingAnimationFrames_.end()) {
    // The queued job has not started, so the newer frame replaces its payload
    // instead of appending a second job (design §5.4).
    if (pending->second->merge(paintPatches, layoutPatches)) {
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
  frame->merge(paintPatches, layoutPatches);
  // With JS off the UI thread `writeAnimatedProps` cannot write paint keys to
  // the views as they arrive, so the frame writes them when it is taken (at
  // compute time, so a frame merged into a pending job writes its latest
  // values). A synchronous job computes on the UI thread and writes there; an
  // async one computes on the worker and leaves the write to `applied`, which
  // for async jobs runs on the UI thread after compute. (For synchronous jobs
  // `applied` runs before compute, so it cannot do the write itself.)
  auto paintForView = UIThreadRendererWorker::kJavaScriptOnUIThread
      ? nullptr
      : std::make_shared<std::vector<std::pair<Tag, folly::dynamic>>>();
  auto job = std::make_unique<UIThreadRendererWorker::Job>();
  job->synchronous = synchronous;
  job->compute = [this, rendererOwner, frame, paintForView] {
    std::vector<std::pair<Tag, folly::dynamic>> framePaint;
    std::vector<std::pair<Tag, folly::dynamic>> frameLayout;
    if (!frame->take(framePaint, frameLayout)) {
      return;
    }
    if (paintForView) {
      if (worker_->onUIThread()) {
        for (const auto& [tag, paint] : framePaint) {
          applyAnimatedPaintToView(tag, paint);
        }
      } else {
        *paintForView = framePaint;
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
  const auto start = UIThreadRendererWorkerProfile::Clock::now();
  bool ranSynchronously = false;
  submitWorkerJob(token, std::move(job), &ranSynchronously);
  if (!ranSynchronously) {
    // The worker was busy, so this frame is the pending job now: its cost is
    // measured by whoever ends up running it, not here.
    return;
  }
  const auto micros = UIThreadRendererWorkerProfile::ms(
                         start, UIThreadRendererWorkerProfile::Clock::now()) *
      1000.0;
  recordAnimationLaneCost(surfaceId, micros);
  traceAnimationLane(
      surfaceId, true, micros, paintPatches.size() + layoutPatches.size());
}

void UIThreadRendererBinding::setWorkerNativeProps(
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
  if (auto* delegate = getUIManager().getDelegate()) {
    const auto previousTag = replayingNativePropsTag_;
    const auto* previousProps = replayingNativeProps_;
    replayingNativePropsTag_ = ref.tag;
    replayingNativeProps_ = payload.get();
    try {
      delegate->uiManagerShouldSynchronouslyUpdateViewOnUIThread(
          ref.tag, *payload);
    } catch (...) {
      replayingNativePropsTag_ = previousTag;
      replayingNativeProps_ = previousProps;
      throw;
    }
    replayingNativePropsTag_ = previousTag;
    replayingNativeProps_ = previousProps;
  }
}
#endif

void UIThreadRendererBinding::releaseSurface(SurfaceId surfaceId) {
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

void UIThreadRendererBinding::applyConstraints(
    SurfaceId surfaceId,
    const SurfaceConstraints& constraints) {
  std::unique_lock state(stateMutex_);
  auto iterator = renderers_.find(surfaceId);
  if (iterator == renderers_.end()) {
    // Nothing is mounted for this surface yet. `renderer()` picks these up when
    // it creates the renderer, so there is nothing to reflow.
    return;
  }
#if UI_THREAD_RENDERER_WORKER
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

void UIThreadRendererBinding::releaseSurfaceInternal(SurfaceId surfaceId) {
  std::lock_guard state(stateMutex_);
  if (auto token = surfaceTokens_.find(surfaceId);
      token != surfaceTokens_.end()) {
    token->second->active = false;
    surfaceTokens_.erase(token);
  }
#if UI_THREAD_RENDERER_WORKER
  if (auto overlays = nativePropsOverlays_.find(surfaceId);
      overlays != nativePropsOverlays_.end()) {
    for (const auto& entry : overlays->second) {
      mountedIdentities_.erase(entry.first);
    }
    nativePropsOverlays_.erase(overlays);
  }
#endif
#if UI_THREAD_RENDERER_WORKER
  for (auto it = mountedTokens_.begin(); it != mountedTokens_.end();) {
    if (it->second->surfaceId == surfaceId) {
      mountedIdentities_.erase(it->first);
      it = mountedTokens_.erase(it);
    } else
      ++it;
  }
#endif
#if UI_THREAD_RENDERER_WORKER
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
  UIThreadEventManager::clearSurface(surfaceId);
}

UIThreadRendererBinding::SurfaceConstraints
UIThreadRendererBinding::constraintsFor(SurfaceId surfaceId) const {
  std::lock_guard lock(registryMutex());
  auto iterator = surfaceConstraints().find(surfaceId);
  return iterator == surfaceConstraints().end() ? SurfaceConstraints{}
                                                : iterator->second;
}

#pragma mark - Renderer

UIThreadRenderer& UIThreadRendererBinding::renderer(SurfaceId surfaceId) {
  {
    std::lock_guard state(stateMutex_);
    auto iterator = renderers_.find(surfaceId);
    if (iterator != renderers_.end()) {
      return *iterator->second;
    }
  }

  auto rendererToken = tokenFor(surfaceId);
  const std::weak_ptr<SurfaceToken> weakRendererToken = rendererToken;

  // The delegate is resolved on every call rather than captured, because it can
  // be replaced (and cleared) independently of this binding's lifetime.
  UIThreadRenderer::NativeOperations operations;
#if defined(__APPLE__)
  // The iOS ParagraphComponentView consumes attributed-string state and draws
  // it through TextLayoutManager; the current iOS measurement API has no
  // renderable prepared-layout handle to publish.
  operations.prepareTextForMountProvidesLayout = false;
#endif
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
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererFailSurface(surface, reason);
      token->active = false;
    }
  };
  operations.applyBatch = [this](
                              SurfaceId surface,
                              UIThreadDirectMountBatch batch) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererApplyBatch(surface, std::move(batch));
#if UI_THREAD_RENDERER_WORKER
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
          delegate->uiManagerShouldSynchronouslyUpdateViewOnUIThread(
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
    }
  };
  operations.registerEventTarget =
      [this](SurfaceId surface, Tag tag, InstanceHandle::Shared handle) {
        if (auto* delegate = getUIManager().getDelegate();
            delegate != nullptr) {
          delegate->uiThreadRendererRegisterEventTarget(
              surface, tag, std::move(handle));
        }
      };
  operations.unregisterEventTarget = [this](SurfaceId surface, Tag tag) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererUnregisterEventTarget(surface, tag);
    }
  };
  operations.dispatchEvent = [this](
                                 SurfaceId surface,
                                 Tag tag,
                                 const std::string& eventName,
                                 folly::dynamic payload,
                                 RawEvent::Category category,
                                 bool unique) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererDispatchEvent(
          surface, tag, eventName, std::move(payload), category, unique);
    }
  };
  operations
      .parseTextAttributes = [this](
                                 SurfaceId surface,
                                 const std::string& componentName,
                                 const folly::dynamic& props,
                                 const TextAttributes& inherited,
                                 ParagraphAttributes* outParagraphAttributes) {
    auto paragraphAttributes = ParagraphAttributes{};
    auto* delegate = getUIManager().getDelegate();
    auto attributes = delegate == nullptr
        ? inherited
        : delegate->uiThreadRendererParseTextAttributes(
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
    UIThreadRenderer::TextMeasurement measurement;
    auto* delegate = getUIManager().getDelegate();
    if (delegate == nullptr) {
      return measurement;
    }

    // Measured by the platform's own text engine, through the same
    // manager the shadow-tree path uses, so text is laid out for real
    // instead of estimated.
    UIThreadRendererWorkerProfile::TextScope textProfile;
    auto size = delegate->uiThreadRendererMeasureText(
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
    auto* delegate = getUIManager().getDelegate();
    if (delegate == nullptr) {
      return UIThreadTextMeasurement{};
    }
    UIThreadRendererWorkerProfile::TextScope textProfile;
    auto measurement = delegate->uiThreadRendererPrepareText(
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
        auto* delegate = getUIManager().getDelegate();
        if (delegate == nullptr) {
          return UIThreadTextMeasurement{};
        }
        UIThreadRendererWorkerProfile::TextScope textProfile;
        return delegate->uiThreadRendererPrepareTextForMount(
            surface,
            attributedString,
            paragraphAttributes,
            minimumSize,
            maximumSize);
      };
#if UI_THREAD_RENDERER_WORKER
  operations.getScrollOffset = [this](SurfaceId surface, Tag tag) {
    // Live view state belongs to the UI thread; a JS-thread reader asks it.
    return readOnUIThread(
               [surface, tag](UIManagerDelegate& delegate)
                   -> std::optional<std::pair<float, float>> {
                 return delegate.uiThreadRendererGetScrollOffset(surface, tag);
               },
               std::pair<float, float>{0, 0})
        .value_or(std::pair<float, float>{0, 0});
  };
  operations.getViewLocationInSurface = [this](SurfaceId surface, Tag tag)
      -> std::optional<std::pair<float, float>> {
    return readOnUIThread(
        [surface, tag](UIManagerDelegate& delegate) {
          return delegate.uiThreadRendererGetViewLocationInSurface(
              surface, tag);
        },
        std::nullopt);
  };
#else
  operations.getScrollOffset = [this](SurfaceId surface, Tag tag) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      return delegate->uiThreadRendererGetScrollOffset(surface, tag);
    }
    return std::pair<float, float>{0, 0};
  };
  operations.getViewLocationInSurface = [this](SurfaceId surface, Tag tag)
      -> std::optional<std::pair<float, float>> {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      return delegate->uiThreadRendererGetViewLocationInSurface(surface, tag);
    }
    return std::nullopt;
  };
#endif
  operations.reportUnsupportedInlineAttachment = [this](
                                                     SurfaceId surface,
                                                     Tag tag) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererReportUnsupportedInlineAttachment(surface, tag);
    }
  };
  operations.reportUnsupportedTransform = [](SurfaceId surface, Tag tag) {
    std::fprintf(
        stderr,
        "UI_THREAD_RENDERER does not support perspective/3D transform metrics for tag %d on surface %d\n",
        static_cast<int>(tag),
        static_cast<int>(surface));
  };
  operations.reportUnsupportedIntrinsicMeasurement = [](SurfaceId surface,
                                                        Tag tag,
                                                        const std::string&
                                                            component) {
    std::fprintf(
        stderr,
        "UI_THREAD_RENDERER requires explicit size for intrinsically sized component %s, tag %d on surface %d\n",
        component.c_str(),
        static_cast<int>(tag),
        static_cast<int>(surface));
  };
  operations.updateState = [this](
                               SurfaceId surface,
                               Tag tag,
                               const std::string& componentName,
                               const folly::dynamic& props,
                               const AttributedString& attributedString,
                               const ParagraphAttributes& paragraphAttributes,
                               const UIThreadTextMeasurement& prepared) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererUpdateState(
          surface,
          tag,
          componentName,
          props,
          attributedString,
          paragraphAttributes,
          prepared);
    }
  };
  operations.updateScrollContentBounds = [this](
                                             SurfaceId surface,
                                             Tag tag,
                                             const folly::dynamic& props,
                                             const Rect& bounds) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererUpdateScrollContentBounds(
          surface, tag, props, bounds);
    }
  };
  operations.dispatchCommand = [this](
                                   SurfaceId surface,
                                   Tag tag,
                                   const std::string& commandName,
                                   const folly::dynamic& args) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererDispatchCommand(
          surface, tag, commandName, args);
    }
  };
  operations.sendAccessibilityEvent = [this](
                                          SurfaceId surface,
                                          Tag tag,
                                          const std::string& eventType) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererSendAccessibilityEvent(surface, tag, eventType);
    }
  };
  operations.setIsJSResponder = [this](
                                    SurfaceId surface,
                                    Tag tag,
                                    bool isJSResponder,
                                    bool blockNativeResponder) {
    if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
      delegate->uiThreadRendererSetIsJSResponder(
          surface, tag, isJSResponder, blockNativeResponder);
    }
  };
  operations.commitFinished = [this, weakRendererToken](SurfaceId surface) {
    auto token = weakRendererToken.lock();
    if (token != nullptr && token->active && token->surfaceId == surface) {
      drainPendingRootCommits(surface);
    }
  };

#if UI_THREAD_RENDERER_WORKER
  // Only pure text preparation callbacks remain executable during computation.
  // All native state/event/view effects are built into the owning UI result.
  operations.failSurface = worker_->defer(std::move(operations.failSurface));
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
  operations.updateScrollContentBounds =
      worker_->defer(std::move(operations.updateScrollContentBounds));
  operations.commitFinished = {};
#endif
  auto created =
      std::make_shared<UIThreadRenderer>(surfaceId, std::move(operations));
  created->setPointScaleFactor(directPointScaleFactorValue());

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

#if UI_THREAD_RENDERER_WORKER
void UIThreadRendererBinding::submitWorkerOperation(
    std::shared_ptr<SurfaceToken> token,
    UIThreadRendererWorker::Task compute,
    std::function<void()> applied,
    bool synchronous,
    bool profileCommit,
    bool inlineEligible) {
  auto job = std::make_unique<UIThreadRendererWorker::Job>();
  job->inlineEligible = inlineEligible;
  job->compute = std::move(compute);
  job->applied = std::move(applied);
  job->synchronous = synchronous;
  job->profileCommit = profileCommit;
  submitWorkerJob(std::move(token), std::move(job));
}

void UIThreadRendererBinding::submitWorkerJob(
    std::shared_ptr<SurfaceToken> token,
    std::unique_ptr<UIThreadRendererWorker::Job> job,
    bool* ranSynchronously) {
  if (ranSynchronously != nullptr) {
    *ranSynchronously = false;
  }
  job->active = [token] { return token->active.load(); };
  job->failed = [this, token](std::exception_ptr error) {
    if (!token->active.exchange(false))
      return;
    std::string reason =
        "Async direct renderer computation or UI application failed";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& exception) {
      reason += ": ";
      reason += exception.what();
    } catch (...) {
    }
    if (auto* delegate = getUIManager().getDelegate()) {
      delegate->uiThreadRendererFailSurface(token->surfaceId, reason);
    }
  };
  #if defined(__ANDROID__)
  if (job->compute) {
    auto compute = std::move(job->compute);
    job->compute = [compute = std::move(compute)]() mutable {
      jni::ThreadScope::WithClassLoader([&compute] { compute(); });
    };
  }
#else
  // Task is already owned by the job.
#endif
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

void UIThreadRendererBinding::publishWorkerCommit(
    SurfaceId surfaceId,
    const std::vector<std::shared_ptr<NodeRef>>& children,
    bool forceSync) {
#if RN_JANK_REPORTER
  // React Native X (debug): lets the jank report say how much of a slow task
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
  auto packet = std::make_unique<WorkerCommit>();
  packet->surfaceId = surfaceId;
  packet->token = tokenFor(surfaceId);
  // Draft data is built once by the host. Freeze each version; appendChild
  // thereafter creates a new version and copies only its changed child list.
  auto identities = std::make_shared<std::unordered_map<Tag, uint64_t>>();
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
        facts.hasText = draft->componentName &&
            (*draft->componentName == "RCTText" ||
             *draft->componentName == "Paragraph" ||
             *draft->componentName == "Text");
        if (facts.hasText && draft->componentName &&
            (*draft->componentName == "RCTText" ||
             *draft->componentName == "Paragraph" ||
             *draft->componentName == "Text"))
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
  // Stale entries are harmless: both reads validate the stored identity against
  // the live draft (see `setWorkerNativeProps`), so a removed or reused tag simply
  // fails the comparison, and surface teardown prunes by surface.
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
  if (auto* delegate = getUIManager().getDelegate()) {
    // Asked only when the answer can change the decision: the query is a JNI
    // call plus a view-manager registry lookup, and it must not be cached
    // across commits because `TextEffectRegistry.hasRegisteredFactories()`
    // changes when a library registers or unregisters a factory.
    const bool textSafe =
        !hasText || canMeasureTextOnWorker(*delegate);
    // The text-safety decision belongs to this commit only. Earlier revisions
    // kept it on the surface token, which made every later job on the surface
    // (setNativeProps, constraints) synchronous as well.
    const bool synchronousText =
        retainsLiveSnapshot || (hasText && (!textSafe || unsafeTextProps));
    forceSync = forceSync || synchronousText;
  } else
    forceSync = true;
  auto& live = renderer(surfaceId);
  std::shared_ptr<UIThreadRenderer> owner;
  {
    std::lock_guard state(stateMutex_);
    owner = renderers_.at(surfaceId);
  }
  auto token = packet->token;
  auto applied = [this, token, identities, committedDrafts] {
    applyPublishedIdentities(token, *identities, *committedDrafts);
  };
  // The worker pauses after all preceding UI results. A synchronous job borrows
  // the same renderer on UI for its ENTIRE reconciliation/style/layout/diff.
  // Reentrant submissions remain ordered after the enclosing UI result.
  (void)live;
  // React Native X: cost estimate. Units are the changed nodes, with a text
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
  auto compute = UIThreadRendererWorker::Task([this,
                                               owner,
                                               packet = std::move(packet),
                                               identities,
                                               publishIdentities = applied,
                                               forceSync,
                                               smallCommit] {
    const auto computeStart = std::chrono::steady_clock::now();
    reconcileWorkerCommit(*owner, *packet);
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

void UIThreadRendererBinding::applyPublishedIdentities(
    const std::shared_ptr<SurfaceToken>& token,
    const std::unordered_map<Tag, uint64_t>& identities,
    const std::vector<std::shared_ptr<DraftNode>>& committedDrafts) {
  std::lock_guard state(stateMutex_);
  // Erase only what this commit no longer contains. This pass is what discovers
  // removals, and it stays correct because `identities` is the full live set
  // rather than the freeze walk's output.
  for (auto it = mountedTokens_.begin(); it != mountedTokens_.end();) {
    if (it->second->surfaceId == token->surfaceId &&
        identities.find(it->first) == identities.end()) {
      mountedIdentities_.erase(it->first);
      it = mountedTokens_.erase(it);
    } else {
      ++it;
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

void UIThreadRendererBinding::reconcileWorkerCommit(
    UIThreadRenderer& liveRenderer,
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
      const bool rawText = draft.componentName &&
          (*draft.componentName == "RCTRawText" ||
           *draft.componentName == "RawText");
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
    liveRenderer.abortCommit("unknown exception in direct renderer worker");
  }
}
#endif

UIThreadRendererBinding::CommitSelection
UIThreadRendererBinding::selectCommitDrafts(
    UIThreadRenderer& liveRenderer,
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
  std::unordered_map<Tag, SelectionRecord> selectionByTag;

  std::function<void(const std::shared_ptr<DraftNode>&)> selectDraft;
  std::function<void(Tag)> retainLiveSubtree;
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
    record.visit = SelectionRecord::Visit::Active;
    if (const auto* children = liveRenderer.childrenOfRef(tag);
        children != nullptr) {
      for (const auto child : *children) {
        retainLiveSubtree(child);
      }
    }
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
  // Borrowed views for the attachment scan: it only reads a name and a child
  // list per node, so materialising both for every selected node copied a
  // string and a vector per node on every commit.
  const auto componentNamePointer = [&](Tag tag) -> const std::string* {
    const auto record = selectionByTag.find(tag);
    if (record != selectionByTag.end() &&
        record->second.selectedIndex != noSelectedIndex) {
      const auto& draft = selected[record->second.selectedIndex].draft;
      if (draft->componentName != nullptr) {
        return draft->componentName.get();
      }
    }
    // A retained node carries no name on its draft, so the live renderer
    // answers - exactly as the copying form did. Returning null here would make
    // a retained paragraph look like an unsupported inline attachment and fail
    // the surface.
    return liveRenderer.componentNameRef(tag);
  };
  const auto childrenPointer = [&](Tag tag) -> const std::vector<Tag>* {
    const auto record = selectionByTag.find(tag);
    if (record != selectionByTag.end() &&
        record->second.selectedIndex != noSelectedIndex &&
        selected[record->second.selectedIndex].childrenChanged) {
      return &selected[record->second.selectedIndex].childTags;
    }
    return liveRenderer.childrenOfRef(tag);
  };
  selection.unsupportedAttachmentTag =
      UIThreadRenderer::findUnsupportedInlineAttachment(
          selection.rootChildren, componentNamePointer, childrenPointer);
  return selection;
}

void UIThreadRendererBinding::reconcileSelectedDrafts(
    UIThreadRenderer& liveRenderer,
    SurfaceId surfaceId,
    const CommitSelection& selection,
    bool compact) {
  static const auto emptyDraftChildren =
      std::make_shared<std::vector<std::shared_ptr<DraftNode>>>();
  const auto& selected = selection.selected;

  // Preserve cleanup edges owned by a parent that the selected graph removes.
  liveRenderer.captureRemovedMountedEdges(selection.selectedIdentities);

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

void UIThreadRendererBinding::publishRootCommit(
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
#if defined(__APPLE__)
  const bool profileSyncCommit = UIThreadRendererWorkerProfile::enabled();
  const auto syncCommitStart = profileSyncCommit
      ? UIThreadRendererWorkerProfile::Clock::now()
      : UIThreadRendererWorkerProfile::Clock::time_point{};
#endif
  publishingRootCommits_.insert(surfaceId);
  // releaseSurfaceInternal removes the map entry immediately, but this strong
  // local reference keeps the active commit's renderer alive until callbacks
  // have unwound. A restarted surface gets a new renderer and new token.
  auto liveRendererOwner = renderers_.at(surfaceId);
  auto& liveRenderer = *liveRendererOwner;
  try {
#if RN_PROFILE_UI_THREAD_RENDERER
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
      if (auto* delegate = getUIManager().getDelegate(); delegate != nullptr) {
        delegate->uiThreadRendererReportUnsupportedInlineAttachment(
            surfaceId, commitSelection.unsupportedAttachmentTag);
        delegate->uiThreadRendererFailSurface(
            surfaceId,
            "Direct renderer commit contains an unsupported inline text attachment");
      }
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
#if RN_PROFILE_UI_THREAD_RENDERER
    // Reconciliation and the root commit are one atomic step now, so this marks
    // the end of both.
    const auto reconcileEnd = std::chrono::steady_clock::now();
#endif
#if RN_PROFILE_UI_THREAD_RENDERER
    const auto profileAfterCommit = std::chrono::steady_clock::now();
#if defined(__ANDROID__)
    const auto profileMs = [](auto from, auto to) {
      return std::chrono::duration<double, std::milli>(to - from).count();
    };
    __android_log_print(
        ANDROID_LOG_INFO,
        "RNXPublishProfile",
        "surface=%d gen=%llu reconciliation=%.3fms rendererCommit=%.3fms",
        static_cast<int>(surfaceId),
        static_cast<unsigned long long>(profileGeneration),
        profileMs(reconcileStart, reconcileEnd),
        profileMs(reconcileEnd, profileAfterCommit));
#endif
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
#if defined(__APPLE__)
    if (profileSyncCommit) {
      os_log(
          OS_LOG_DEFAULT,
          "RNXDirectSyncProfile generation=%llu main=%d work=%.3f",
          static_cast<unsigned long long>(profileGeneration),
          pthread_main_np(),
          UIThreadRendererWorkerProfile::ms(
              syncCommitStart, UIThreadRendererWorkerProfile::Clock::now()));
    }
#endif
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

void UIThreadRendererBinding::drainPendingRootCommits(SurfaceId surfaceId) {
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

std::shared_ptr<UIThreadRendererBinding::NodeRef>
UIThreadRendererBinding::makeNodeRef(
    SurfaceId surfaceId,
    Tag tag,
    std::shared_ptr<DraftNode> draft) {
  return std::make_shared<NodeRef>(
      surfaceId, tag, tokenFor(surfaceId), std::move(draft));
}

std::shared_ptr<UIThreadRendererBinding::SurfaceToken>
UIThreadRendererBinding::tokenFor(SurfaceId surfaceId) {
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

std::shared_ptr<UIThreadRendererBinding::DraftNode>
UIThreadRendererBinding::cloneDraft(
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

bool UIThreadRendererBinding::propsContainTextEffects(const folly::dynamic& value) {
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

void UIThreadRendererBinding::linkPendingPatches(
    DraftNode& draft,
    PropsPatch& patch) {
  auto boundary = draft.propsTail;
  while (boundary && boundary->applied.load(std::memory_order_relaxed)) {
    draft.retainedUnsafeTextProps |= boundary->unsafeText;
    boundary = boundary->previous;
  }
  patch.previous = std::move(boundary);
}

bool UIThreadRendererBinding::isRawTextRef(const NodeRef& ref) const {
  if (ref.draft != nullptr && ref.draft->componentName != nullptr) {
    const auto& name = *ref.draft->componentName;
    return isRawTextName(name);
  }
  const auto renderer = findRenderer(ref.surfaceId);
  return renderer != nullptr && renderer->componentName(ref.tag) == "RCTRawText";
}

std::shared_ptr<UIThreadRenderer> UIThreadRendererBinding::findRenderer(
    SurfaceId surfaceId) const {
  std::lock_guard state(stateMutex_);
  const auto iterator = renderers_.find(surfaceId);
  return iterator == renderers_.end() ? nullptr : iterator->second;
}

#if UI_THREAD_RENDERER_WORKER
bool UIThreadRendererBinding::canMeasureTextOnWorker(
    UIManagerDelegate& delegate) {
  if (UIThreadRendererWorker::kJavaScriptOnUIThread) {
    // Asked only when the answer can change the decision, and not cached:
    // `TextEffectRegistry.hasRegisteredFactories()` changes when a library
    // registers or unregisters a factory.
    return delegate.uiThreadRendererCanMeasureTextOnWorker();
  }
  if (worker_->onUIThread())
    refreshCanMeasureTextOnWorker();
  return canMeasureTextOnWorker_.load() == 1;
}

void UIThreadRendererBinding::refreshCanMeasureTextOnWorker() {
  if (!worker_->onUIThread())
    return;
  if (auto* delegate = getUIManager().getDelegate())
    canMeasureTextOnWorker_ =
        delegate->uiThreadRendererCanMeasureTextOnWorker() ? 1 : 0;
}

std::optional<std::pair<float, float>> UIThreadRendererBinding::readOnUIThread(
    std::function<std::optional<std::pair<float, float>>(UIManagerDelegate&)>
        read,
    std::optional<std::pair<float, float>> fallback) {
  auto* delegate = getUIManager().getDelegate();
  if (delegate == nullptr)
    return fallback;
  if (worker_->onUIThread())
    return read(*delegate);
  auto uiExecutor = delegate->uiThreadRendererGetUIExecutor();
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
        if (auto* delegate = binding->getUIManager().getDelegate()) {
          try {
            value = read(*delegate);
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
#if defined(__ANDROID__)
    __android_log_print(
        ANDROID_LOG_WARN,
        "RNXWorker",
        "live view read waited 5 s for the UI thread");
#endif
    return fallback;
  }
  return reply->value;
}
#endif

UIThreadRenderer* UIThreadRendererBinding::rendererOwning(Tag tag) {
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

std::shared_ptr<UIThreadRendererBinding::NodeRef>
UIThreadRendererBinding::nodeRef(jsi::Runtime& runtime, const jsi::Value& value)
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

bool UIThreadRendererBinding::isLiveNodeRef(const NodeRef& ref) const {
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

std::vector<Tag> UIThreadRendererBinding::tagsOf(
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

jsi::Value UIThreadRendererBinding::get(
    jsi::Runtime& runtime,
    const jsi::PropNameID& name) {
#if RN_PROFILE_UI_THREAD_RENDERER
  // The JS-side cached Fabric host functions use this opt-in marker to measure
  // complete JS→JSI-call elapsed time. It is set only in diagnostic builds.
  runtime.global().setProperty(
      runtime, "__RNX_PROFILE_UI_THREAD_RENDERER", true);
#endif
  const auto method = name.utf8(runtime);

  // React Native's Pressability and other legacy UIManager call sites still
  // measure by React tag through FabricUIManager.measure(). In direct-renderer
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

#if UI_THREAD_RENDERER_WORKER
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
          RNX_HOST_CALL_SCOPE(
              profile, *this, surface, ProfiledHostMethod::CreateNode);
          auto tag = tagFromValue(arguments[0]);
          auto token = tokenFor(surface);
          auto draft = std::make_shared<DraftNode>();
          draft->surfaceId = surface;
          draft->surfaceGeneration = token->generation;
          draft->identity = ++nextDraftIdentity_[surface];
          draft->tag = tag;
          const auto componentName = stringFromValue(rt, arguments[1]);
          draft->componentName =
              std::make_shared<const std::string>(componentName);
          const auto rawText =
              (componentName == "RCTRawText" || componentName == "RawText")
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
          draft->instanceHandle =
              instanceHandleFromValue(rt, arguments[4], arguments[0]);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(surface, tag, std::move(draft)));
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
          RNX_HOST_CALL_SCOPE(
              profile, *this, ref->surfaceId, ProfiledHostMethod::CloneNode);
          auto draft = cloneDraft(ref);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(ref->surfaceId, ref->tag, std::move(draft)));
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
          RNX_HOST_CALL_SCOPE(
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
          }
          linkPendingPatches(*draft, *patch);
          draft->propsTail = std::move(patch);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(ref->surfaceId, ref->tag, std::move(draft)));
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
          RNX_HOST_CALL_SCOPE(
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
              rt, makeNodeRef(ref->surfaceId, ref->tag, std::move(draft)));
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
          RNX_HOST_CALL_SCOPE(
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
          }
          linkPendingPatches(*draft, *patch);
          draft->propsTail = std::move(patch);
          return jsi::Object::createFromHostObject(
              rt, makeNodeRef(ref->surfaceId, ref->tag, std::move(draft)));
        });
  }

  if (method == "createChildSet") {
    return jsi::Function::createFromHostFunction(
        runtime,
        name,
        0,
        [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t)
            -> jsi::Value {
          auto set = std::make_shared<ChildSet>();
#if RN_PROFILE_UI_THREAD_RENDERER
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
#if RN_PROFILE_UI_THREAD_RENDERER
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
            RNX_HOST_CALL_SCOPE(
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
            RNX_HOST_CALL_SCOPE(
                profile,
                *this,
                parent->surfaceId,
                ProfiledHostMethod::AppendChild);
#if UI_THREAD_RENDERER_WORKER
            if (parent->draft->published)
              parent->draft = cloneDraft(parent);
#endif
            auto& draft = *parent->draft;
            if (!draft.childrenExplicit) {
#if UI_THREAD_RENDERER_WORKER
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
#if RN_PROFILE_UI_THREAD_RENDERER
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
#if RN_PROFILE_UI_THREAD_RENDERER
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
#if UI_THREAD_RENDERER_WORKER
          const bool forceSync = forceNextCommitSync_ ||
              (count > 2 && arguments[2].isBool() && arguments[2].getBool());
          forceNextCommitSync_ = false;
          publishWorkerCommit(surface, selectedRoots, forceSync);
#else
          publishRootCommit(surface, selectedRoots, token, generation);
#endif
#if RN_PROFILE_UI_THREAD_RENDERER
          hostProfile.methods[completeRootProfileIndex].totalNanoseconds +=
              static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - completeRootStart)
                      .count());
#if defined(__ANDROID__)
          const auto method =
              [&hostProfile](
                  ProfiledHostMethod value) -> const HostMethodStats& {
            return hostProfile.methods[static_cast<size_t>(value)];
          };
          const auto methodMs = [](const HostMethodStats& stats) {
            return static_cast<double>(stats.totalNanoseconds) / 1.0e6;
          };
          __android_log_print(
              ANDROID_LOG_INFO,
              "RNXHostProfile",
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
#endif
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
#if UI_THREAD_RENDERER_WORKER
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
#if UI_THREAD_RENDERER_WORKER
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
#if UI_THREAD_RENDERER_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "getRelativeLayoutMetrics", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          auto ancestorRef = nodeRef(rt, arguments[1]);

          UIThreadRenderer::Metrics metrics;
          auto* owner = ref == nullptr ? nullptr : rendererOwning(ref->tag);
          const auto ancestorTag =
              ancestorRef == nullptr ? 0 : ancestorRef->tag;
          if (owner == nullptr ||
              !owner->metricsRelativeTo(ref->tag, ancestorTag, metrics)) {
            metrics = UIThreadRenderer::Metrics{};
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
#if UI_THREAD_RENDERER_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "getBoundingClientRect", 2, count);
          auto ref = nodeRef(rt, arguments[0]);
          const bool includeTransform = arguments[1].getBool();

          UIThreadRenderer::Metrics metrics;
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
#if UI_THREAD_RENDERER_WORKER
          worker_->synchronize();
#endif
          requireArguments(rt, "measureInstance", 1, count);
          auto ref = nodeRef(rt, arguments[0]);

          UIThreadRenderer::Metrics metrics;
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
#if UI_THREAD_RENDERER_WORKER
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
#if UI_THREAD_RENDERER_WORKER
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
