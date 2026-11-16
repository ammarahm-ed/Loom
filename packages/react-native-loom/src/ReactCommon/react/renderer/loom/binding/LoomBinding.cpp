/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


// LoomBinding: construction and teardown, surfaces (constraints, scale, release, the
// per-surface renderer), component state, and the UI-thread helpers.
// The rest of the class lives beside this file:
//   LoomBindingJSI.cpp        the nativeFabricUIManager host object (get)
//   LoomBindingCommit.cpp     worker jobs, commit publishing and reconciliation
//   LoomBindingAnimation.cpp  native props, the animation lane, layout animation frames
//   LoomBindingNodes.cpp      node references, drafts and Fabric compat props

#include "LoomBindingInternals.h"

#if LOOM_RENDERER
namespace facebook::react {

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
  // The library API's per-instance state lives in the same container, so `loom::Runtime::from` with
  // the container (before JavaScript) and with the JS runtime (after) reach the same state.
  apiInstance_ = loom::internal::Instance::in(*getUIManager().getContextContainer());
  apiInstance_->attach(lifetime_);
  // registerIn installed the host's traits. If this side still sees the defaults, the
  // process holds two copies of the traits (a build that duplicated shared state per
  // module): every thread would count as the UI thread and view work would run off it.
  if (!LoomPlatformTraits::isInstalled()) {
    LOG(FATAL) << "Loom: LoomHost was registered but its LoomPlatformTraits are not visible "
                  "to the renderer; the build duplicated process-wide state across modules.";
  }
  text_ = host_->textServices();
  installLoomFabricBridge();
  // Power-of-two bucket counts keep these on mask indexing (see LoomTagHash).
  mountedIdentities_.reserve(1024);
  mountedTokens_.reserve(1024);
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
#if LOOM_JS_ON_MAIN
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
  // Unreachable first: no static callback (registry) or library handle (API instance) can enter
  // once these return, and the gate waits for the ones already inside. Only then stop the worker,
  // so nothing can submit to a worker that is shutting down.
  apiInstance_->detach(lifetime_.get());
  {
    std::lock_guard lock(registryMutex());
    liveBindings().erase(this);
  }
  {
    std::unique_lock lock(lifetime_->gate);
    lifetime_->binding = nullptr;
  }
  for (auto& [surface, token] : surfaceTokens_)
    token->active = false;
  worker_->shutdown();
#if LOOM_JS_ON_MAIN
  FrameCallbackScope::afterFrameCallbacks() = {};
#endif
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
}

void LoomBinding::submitOrderedUIEffect(
    SurfaceId surfaceId,
    std::function<void()> effect) {
  auto job = std::make_unique<LoomWorker::Job>();
  // `run()` always invokes `compute`, so it cannot be left empty; the work
  // itself belongs on the UI thread, after the in-flight result has been
  // applied, which is where `applied` runs.
  job->compute = [] {};
  job->applied = std::move(effect);
  submitWorkerJob(tokenFor(surfaceId), std::move(job));
}

void LoomBinding::synchronizeWorkerForUIManager(
    UIManager& manager) {
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    if (auto* binding = lifetime->binding;
        binding && &binding->getUIManager() == &manager) {
      binding->worker_->synchronize();
    }
  }
}

void LoomBinding::shutdownWorkerForUIManager(UIManager& manager) {
  for (const auto& lifetime : bindingSnapshot()) {
    std::shared_lock lock(lifetime->gate);
    if (auto* binding = lifetime->binding;
        binding && &binding->getUIManager() == &manager) {
      for (auto& [surface, token] : binding->surfaceTokens_)
        token->active = false;
      binding->worker_->shutdown();
    }
  }
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

void LoomBinding::apiForEachSurface(
    const std::function<void(SurfaceId)>& visit) {
  std::vector<SurfaceId> surfaceIds;
  {
    std::lock_guard state(stateMutex_);
    surfaceIds.reserve(renderers_.size());
    for (const auto& entry : renderers_) {
      surfaceIds.push_back(entry.first);
    }
  }
  for (const auto surfaceId : surfaceIds) {
    visit(surfaceId);
  }
}

std::optional<SurfaceId> LoomBinding::apiSurfaceOf(Tag tag) {
  std::lock_guard state(stateMutex_);
  // The mounted map is the UI's view of the tree; a token names its surface.
  if (auto token = mountedTokens_.find(tag);
      token != mountedTokens_.end() && token->second->active) {
    return token->second->surfaceId;
  }
  return std::nullopt;
}

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
  // Always queued, never run on the calling thread: `unstable_Immediate`
  // updates arrive from inside UIKit layout and from mounting, where applying
  // a batch synchronously would re-enter the batch applier.
  auto job = std::make_unique<LoomWorker::Job>();
  job->compute = [renderer, update] { renderer->applyComponentState(update); };
  submitWorkerJob(token, std::move(job));
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
  // Only this surface's own tags: O(its nodes), not O(every mounted node).
  if (auto mounted = mountedTagsBySurface_.find(surfaceId);
      mounted != mountedTagsBySurface_.end()) {
    for (const auto& entry : mounted->second) {
      mountedIdentities_.erase(entry.first);
      mountedTokens_.erase(entry.first);
    }
    mountedTagsBySurface_.erase(mounted);
  }
  nativePropsOverlays_.erase(surfaceId);
  submittedIdentities_.erase(surfaceId);
  // Animation fast lane state is per surface: a frame still being collected, the
  // single queued animation job and its adaptive cost history all die with it.
  animationFrameBuffer_.erase(surfaceId);
  pendingAnimationFrames_.erase(surfaceId);
  animationLaneStats_.erase(surfaceId);
  // `surfaceGenerations_` is kept on purpose: a restarted surface must not reuse a
  // generation an in-flight job of the old one still carries.
  ++surfaceGenerations_[surfaceId];
#if LOOM_PROFILE
  profileCommitGeneration_.erase(surfaceId);
  profileHostCalls_.erase(surfaceId);
#endif
  renderers_.erase(surfaceId);
  pendingRootCommits_.erase(surfaceId);
  pendingDocumentInstanceHandles_.erase(surfaceId);
  documentInstanceHandles_.erase(surfaceId);
  LoomEventManager::clearSurface(surfaceId);
  loom::internal::releaseSurface(*apiInstance_, surfaceId);
}

void LoomBinding::setMountedLocked(
    Tag tag,
    uint64_t identity,
    const std::shared_ptr<SurfaceToken>& token) {
  auto& mountedToken = mountedTokens_[tag];
  if (mountedToken != nullptr && mountedToken->surfaceId != token->surfaceId) {
    // The tag moved to another surface (a surface restarted under a new id).
    if (auto previous = mountedTagsBySurface_.find(mountedToken->surfaceId);
        previous != mountedTagsBySurface_.end()) {
      previous->second.erase(tag);
    }
  }
  mountedToken = token;
  mountedIdentities_[tag] = identity;
  mountedTagsBySurface_[token->surfaceId][tag] = true;
}

void LoomBinding::eraseMountedLocked(Tag tag) {
  auto mountedToken = mountedTokens_.find(tag);
  if (mountedToken == mountedTokens_.end()) {
    return;
  }
  const auto surfaceId = mountedToken->second->surfaceId;
  if (auto mounted = mountedTagsBySurface_.find(surfaceId);
      mounted != mountedTagsBySurface_.end()) {
    mounted->second.erase(tag);
  }
  if (auto overlays = nativePropsOverlays_.find(surfaceId);
      overlays != nativePropsOverlays_.end()) {
    overlays->second.erase(tag);
  }
  mountedTokens_.erase(mountedToken);
  mountedIdentities_.erase(tag);
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
    // `beforeNextFrame` callbacks of this surface, and only this surface, run right before its batch.
    loom::internal::runBeforeFrame(*apiInstance_, surface);
    // Native-props overlays are replayed only onto views this batch touched: a
    // create or a props/layout update can reset what a native write set, any
    // other view still shows it. So the replay costs O(touched), not O(overlays),
    // and a batch for a surface without overlays collects nothing.
    std::vector<Tag> touched;
    bool hasOverlays = false;
    {
      std::lock_guard state(stateMutex_);
      auto surfaceOverlays = nativePropsOverlays_.find(surface);
      hasOverlays = surfaceOverlays != nativePropsOverlays_.end() &&
          !surfaceOverlays->second.empty();
    }
    if (hasOverlays) {
      for (const auto& operation : batch.operations) {
        if (const auto* create = std::get_if<DirectCreateOperation>(&operation)) {
          touched.push_back(create->tag);
        } else if (const auto* update = std::get_if<DirectUpdatePropsOperation>(&operation)) {
          touched.push_back(update->tag);
        } else if (const auto* layout = std::get_if<DirectLayoutOperation>(&operation)) {
          touched.push_back(layout->tag);
        }
      }
    }
    host_->applyBatch(surface, std::move(batch));
    loom::internal::notifyFrameApplied(*apiInstance_, surface);
    for (const auto tag : touched) {
      // Looked up per tag rather than collected up front: applying native props
      // can emit events and reenter this map, which may stage a nested delta.
      folly::dynamic props;
      {
        std::lock_guard state(stateMutex_);
        auto surfaceOverlays = nativePropsOverlays_.find(surface);
        if (surfaceOverlays == nativePropsOverlays_.end())
          break;
        auto it = surfaceOverlays->second.find(tag);
        if (it == surfaceOverlays->second.end()) {
          continue;
        }
        // `find`, not `operator[]`: a lookup must never insert an entry.
        auto mounted = mountedIdentities_.find(tag);
        if (mounted == mountedIdentities_.end() ||
            mounted->second != it->second.identity) {
          continue;
        }
        props = it->second.props;
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

std::shared_ptr<LoomRenderer> LoomBinding::findRenderer(
    SurfaceId surfaceId) const {
  std::lock_guard state(stateMutex_);
  const auto iterator = renderers_.find(surfaceId);
  return iterator == renderers_.end() ? nullptr : iterator->second;
}

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

} // namespace facebook::react
#endif // LOOM_RENDERER
