/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


// LoomBinding: native props, the animation lane (per-frame animated writes) and layout
// animation frames.

#include "LoomBindingInternals.h"

#if LOOM_RENDERER
namespace facebook::react {

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
        loom::internal::runBeforeFrame(*binding.apiInstance(), surfaceId);
        binding.host()->applyBatch(surfaceId, std::move(batch));
        loom::internal::notifyFrameApplied(*binding.apiInstance(), surfaceId);
      });
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
  // The platform's tag-only entry points (`synchronouslyUpdateViewOnUIThread` and friends) carry no
  // surface: the binding that has the tag mounted takes it.
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    auto* binding = lifetime->binding;
    if (binding != nullptr &&
        binding->recordNativePropsHere(
            std::nullopt, tag, props, knownPaint, viewApplied)) {
      return;
    }
  }
}

void LoomBinding::apiRecordNativeProps(
    SurfaceId surfaceId,
    Tag tag,
    folly::dynamic props,
    bool knownPaint,
    bool viewApplied) {
  recordNativePropsHere(surfaceId, tag, props, knownPaint, viewApplied);
}

bool LoomBinding::recordNativePropsHere(
    std::optional<SurfaceId> surfaceId,
    Tag tag,
    folly::dynamic& props,
    bool knownPaint,
    bool viewApplied) {
  if (replayingNativePropsTag_ == tag && replayingNativeProps_ &&
      props == *replayingNativeProps_) {
    // The replay of an overlay re-entering through the platform's view write.
    return false;
  }
  // The UI thread reaches this while JS may be writing the same maps.
  std::lock_guard state(stateMutex_);
  auto tokenIt = mountedTokens_.find(tag);
  if (tokenIt == mountedTokens_.end() || !tokenIt->second->active) {
    return false;
  }
  auto token = tokenIt->second;
  if (surfaceId.has_value() && token->surfaceId != *surfaceId) {
    return false;
  }
  auto owner = renderers_.find(token->surfaceId);
  if (owner == renderers_.end()) {
    return false;
  }
  // A non-throwing lookup on purpose. An overlay can outlive the identity it
  // was staged for, and the previous `mountedIdentities_.at(tag)` threw
  // `std::out_of_range` out of a JNI frame (`std::terminate`) when the two
  // maps disagreed (design §2, candidate C1). A tag with no identity is
  // simply dropped.
  auto identityIt = mountedIdentities_.find(tag);
  if (identityIt == mountedIdentities_.end()) {
    return false;
  }
  ClassifiedAnimatedProps classified;
  bool isText = owner->second->isTextNode(tag);
  if (knownPaint) {
    if (!isText) {
      classified.paint = std::move(props);
    }
  } else {
    classified = classifyAnimatedProps(props);
    isText = isText || classified.text;
  }
  if (isText) {
    // Font and text keys drive measurement, and any key on a paragraph or raw
    // text node can, which the synchronous lane must not do: the whole payload
    // keeps the ordinary per-call path (design §3.2, "Text").
    stageNativeProps(tag, identityIt->second, token, std::move(props));
    return true;
  }
  // The platform's synchronous update has already put these values on the
  // view, or (`!viewApplied`) the lane writes them itself.
  // Views are written on the UI thread only: off it, a write the platform has not applied is kept
  // for the frame, which writes it on the UI thread (`write.view`).
  writeAnimatedProps(
      token->surfaceId,
      tag,
      identityIt->second,
      std::move(classified),
      /*applyPaintToView=*/!viewApplied && worker_->onUIThread(),
      /*viewAlreadyApplied=*/viewApplied);
  return true;
}

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
    // After the lock and the last use of `write`: a host may run the posted flush at once, and the
    // flush takes this buffer entry.
    state.unlock();
    scheduleAnimationFlush();
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
  scheduleAnimationFlush();
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
    if (auto* binding = lifetime->binding) {
      binding->flushOwnAnimationFrames();
    }
  }
}

void LoomBinding::flushOwnAnimationFrames() {
  // A flush is happening: the next write posts a new one if nothing flushes it first.
  animationFlushPosted_.store(false, std::memory_order_relaxed);
  std::vector<SurfaceId> surfaceIds;
  {
    std::lock_guard state(stateMutex_);
    surfaceIds.reserve(animationFrameBuffer_.size());
    for (const auto& entry : animationFrameBuffer_) {
      surfaceIds.push_back(entry.first);
    }
  }
  for (const auto surfaceId : surfaceIds) {
    flushAnimationFrame(surfaceId);
  }
}

void LoomBinding::scheduleAnimationFlush() {
  if (animationFlushPosted_.exchange(true, std::memory_order_relaxed)) {
    return; // One flush is already on its way.
  }
  auto executor = platformUIExecutor();
  if (!executor) {
    animationFlushPosted_.store(false, std::memory_order_relaxed);
    return;
  }
  executor([weakLifetime = std::weak_ptr<Lifetime>(lifetime_)] {
    if (auto lifetime = weakLifetime.lock()) {
      std::shared_lock gate(lifetime->gate);
      if (auto* binding = lifetime->binding) {
        binding->flushOwnAnimationFrames();
      }
    }
  });
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
  // A paint-only frame reaches the views without a mount batch, so it reports the applied frame to
  // library listeners itself (`onFrameApplied`); a frame with layout does through its batch. Set by
  // compute, read by `applied` (after compute for async jobs).
  auto paintOnlyFrame = std::make_shared<bool>(false);
  job->compute = [this, surfaceId, rendererOwner, frame, paintForView, paintOnlyFrame] {
    std::vector<std::pair<Tag, folly::dynamic>> framePaint;
    std::vector<std::pair<Tag, folly::dynamic>> frameLayout;
    std::vector<std::pair<Tag, folly::dynamic>> frameView;
    if (!frame->take(framePaint, frameLayout, frameView)) {
      return;
    }
    *paintOnlyFrame = frameLayout.empty();
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
    if (*paintOnlyFrame && worker_->onUIThread()) {
      // Computed on the UI thread (a synchronous job): the views were written just above.
      loom::internal::notifyFrameApplied(*apiInstance_, surfaceId);
    }
  };
  job->applied = [this, surfaceId, frame, paintForView, paintOnlyFrame,
                  synchronous] {
    if (paintForView) {
      for (const auto& [tag, paint] : *paintForView) {
        applyAnimatedPaintToView(tag, paint);
      }
    }
    if (!synchronous && *paintOnlyFrame) {
      // Computed on the worker; the views were written just above, on the UI thread.
      loom::internal::notifyFrameApplied(*apiInstance_, surfaceId);
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

} // namespace facebook::react
#endif // LOOM_RENDERER
