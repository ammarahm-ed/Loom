/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file.
 */
#pragma once
#include "LoomWorkerProfile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace facebook::react {

/** One computation owner. It waits for each UI result to be acknowledged
 * before starting the next commit. */
class LoomWorker final {
 public:
  class Task {
    struct Callable {
      virtual ~Callable() = default;
      virtual void invoke() = 0;
    };
    template <typename F>
    struct Holder final : Callable {
      F function;
      explicit Holder(F value) : function(std::move(value)) {}
      void invoke() override {
        function();
      }
    };
    std::unique_ptr<Callable> callable_;

   public:
    Task() = default;
    template <
        typename F,
        typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Task>>>
    Task(F function)
        : callable_(std::make_unique<Holder<F>>(std::move(function))) {}
    Task(Task&&) = default;
    Task& operator=(Task&&) = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    void operator()() {
      callable_->invoke();
    }
    explicit operator bool() const {
      return callable_ != nullptr;
    }
  };

  /**
   * A move-only `void()` callable that stores its target inline.
   *
   * Every effect a job records (a state update, a mount batch, an event target)
   * used to be a `std::function` wrapping a lambda that held a tuple of the
   * call's arguments, which is one heap block per effect on top of the copies in
   * the arguments themselves. The inline buffer is sized from the largest common
   * capture (a shared operation pointer plus `LoomMountBatch` or
   * `LoomTextStateUpdate`, about 250 bytes); anything larger falls back to the
   * heap, so correctness never depends on the size.
   */
  class Effect {
   public:
    static constexpr size_t kInlineBytes = 256;

    Effect() noexcept = default;
    template <
        typename F,
        typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Effect>>>
    explicit Effect(F function) {
      if constexpr (
          sizeof(F) <= kInlineBytes &&
          alignof(F) <= alignof(std::max_align_t) &&
          std::is_nothrow_move_constructible_v<F>) {
        new (storage_) F(std::move(function));
        ops_ = &inlineOps<F>;
      } else {
        auto* heap = new F(std::move(function));
        std::memcpy(storage_, &heap, sizeof(heap));
        ops_ = &heapOps<F>;
      }
    }
    Effect(Effect&& other) noexcept {
      moveFrom(other);
    }
    Effect& operator=(Effect&& other) noexcept {
      if (this != &other) {
        reset();
        moveFrom(other);
      }
      return *this;
    }
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    ~Effect() {
      reset();
    }
    void operator()() {
      ops_->invoke(storage_);
    }
    explicit operator bool() const {
      return ops_ != nullptr;
    }

   private:
    struct Ops {
      void (*invoke)(void*);
      void (*relocate)(void* from, void* to) noexcept;
      void (*destroy)(void*) noexcept;
    };
    template <typename F>
    static inline constexpr Ops inlineOps{
        [](void* p) { (*static_cast<F*>(p))(); },
        [](void* from, void* to) noexcept {
          new (to) F(std::move(*static_cast<F*>(from)));
          static_cast<F*>(from)->~F();
        },
        [](void* p) noexcept { static_cast<F*>(p)->~F(); }};
    template <typename F>
    static inline constexpr Ops heapOps{
        [](void* p) { (**static_cast<F**>(p))(); },
        [](void* from, void* to) noexcept {
          std::memcpy(to, from, sizeof(void*));
        },
        [](void* p) noexcept { delete *static_cast<F**>(p); }};

    void moveFrom(Effect& other) noexcept {
      ops_ = other.ops_;
      if (ops_ != nullptr) {
        ops_->relocate(other.storage_, storage_);
        other.ops_ = nullptr;
      }
    }
    void reset() noexcept {
      if (ops_ != nullptr) {
        ops_->destroy(storage_);
        ops_ = nullptr;
      }
    }

    alignas(std::max_align_t) unsigned char storage_[kInlineBytes];
    const Ops* ops_{nullptr};
  };

  struct Result {
    std::vector<Effect> effects;
    std::exception_ptr error;
    LoomWorkerProfile::Metrics profile;
    LoomWorkerProfile::Clock::time_point computeStart{},
        computeEnd{};
    bool computedOnMain{false};
  };

  struct Job {
    Task compute;
    std::function<bool()> active;
    std::function<void()> applied;
    std::function<void(std::exception_ptr)> failed;
    bool synchronous{false};
    bool profileCommit{false};
    // Loom: the producer estimates this commit as cheaper than a
    // worker hand-off. `tryRunInline` then computes it on the calling thread
    // when the worker is idle.
    bool inlineEligible{false};
    unsigned profileId{0};
    LoomWorkerProfile::Clock::time_point submitted{};
  };

  /**
   * Whether the JavaScript runtime is hosted by the UI thread. When it is not
   * (Android, or iOS with `LOOM_JS_ON_MAIN` defined to 0), JS-thread callers may wait for the UI
   * thread but the UI thread must never wait for JS, and result application is
   * confined to the UI thread. iOS and the host test build always host JS on the
   * thread that applies results.
   */
#if (defined(__ANDROID__) && !LOOM_JS_ON_MAIN) || \
    (defined(__APPLE__) && defined(LOOM_JS_ON_MAIN) && !LOOM_JS_ON_MAIN)
  static constexpr bool kJavaScriptOnUIThread = false;
#else
  static constexpr bool kJavaScriptOnUIThread = true;
#endif

  /** The platform's notion of the thread that applies results. */
  static bool isPlatformUIThread() {
#if defined(__ANDROID__)
    return static_cast<pid_t>(::syscall(SYS_gettid)) == ::getpid();
#elif defined(__APPLE__)
    return pthread_main_np() != 0;
#else
    return true;
#endif
  }

  explicit LoomWorker(
      std::function<void()> notify,
      std::function<bool()> isUIThread = {})
      : isUIThread_(isUIThread ? std::move(isUIThread) : isPlatformUIThread),
        notify_(std::move(notify)) {
    startThread();
  }
  ~LoomWorker() {
    shutdown();
  }
  LoomWorker(const LoomWorker&) = delete;
  LoomWorker& operator=(const LoomWorker&) = delete;

  /** True on the thread that owns result application. */
  bool onUIThread() const {
    return isUIThread_();
  }

  void submit(std::unique_ptr<Job> job) {
    if (job->profileCommit && LoomWorkerProfile::enabled()) {
      job->submitted = LoomWorkerProfile::Clock::now();
      job->profileId = ++profileSequence_;
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      jobs_.push_back(std::move(job));
      ++submitted_;
    }
    wake_.notify_all();
  }

  /**
   * UI only. Runs a synchronous job on the calling thread when this worker is
   * quiescent, so a forced-sync commit does not pay a worker round trip (the
   * worker would otherwise publish an empty result, wake the UI thread, and the
   * UI thread would compute it anyway).
   *
   * Returns false when the worker still owns work, in which case the caller must
   * submit the job as usual: ordering against an in-flight or queued job is the
   * worker's job, not the caller's. The job is taken only when this returns
   * true; otherwise the caller still owns it.
   */
  bool runIfIdle(std::unique_ptr<Job>& job) {
    // A job borrowed off the UI thread would apply its effects there. Queueing
    // it instead lets the worker hand it to the UI thread like any other result.
    if (!job || !job->synchronous || !onUIThread())
      return false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || running_ || result_ || completed_ || !jobs_.empty())
        return false;
      // Publish exactly the state `run()` leaves behind for a synchronous job:
      // it skips `compute` and hands the job to the UI thread. From here on this
      // is the ordinary apply path, including the acknowledgment, so the worker
      // stays paused until the result has been applied.
      if (job->profileCommit && LoomWorkerProfile::enabled()) {
        job->submitted = LoomWorkerProfile::Clock::now();
        job->profileId = ++profileSequence_;
      }
      running_ = true;
      ++submitted_;
      completed_ = std::move(job);
      result_ = std::make_unique<Result>();
    }
    applyReady();
    return true;
  }

  /**
   * Loom: computes an asynchronous job on the calling thread instead
   * of waking the worker, for commits small enough that the cross-thread
   * hand-off (two wakeups plus cache-cold data) costs more than the work.
   *
   * Only taken when the worker is completely quiescent, the same condition
   * `runIfIdle` requires, so a job can never overtake queued or unapplied work
   * and results still apply in submission order. The worker stays marked
   * running until the result is acknowledged, so later submissions queue
   * behind this one exactly as they would behind a worker-computed job.
   *
   * On the UI thread the result is applied before this returns, which is what
   * lets a requestAnimationFrame commit reach the frame it was made in. Off the
   * UI thread (JS-thread mode) only the apply is posted to it. Returns false
   * without touching the job when the worker is busy.
   */
  bool tryRunInline(std::unique_ptr<Job>& job) {
    if (!job || job->synchronous || !job->inlineEligible)
      return false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || running_ || result_ || completed_ || !jobs_.empty())
        return false;
      if (job->profileCommit && LoomWorkerProfile::enabled()) {
        job->submitted = LoomWorkerProfile::Clock::now();
        job->profileId = ++profileSequence_;
      }
      running_ = true;
      ++submitted_;
    }
    auto owned = std::move(job);
    auto result = newResult();
    if (owned->profileId) {
      LoomWorkerProfile::metrics() = {};
      result->computeStart = LoomWorkerProfile::Clock::now();
      result->computedOnMain = onUIThread();
    }
    // Effects recorded by `defer` belong to this result, as on the worker.
    // Restored afterwards because a re-entrant UI apply may itself compute.
    auto* const previousBuilding = building_;
    building_ = result.get();
    try {
      if (!owned->active || owned->active())
        owned->compute();
    } catch (...) {
      result->error = std::current_exception();
    }
    building_ = previousBuilding;
    if (owned->profileId) {
      result->computeEnd = LoomWorkerProfile::Clock::now();
      result->profile = LoomWorkerProfile::metrics();
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return true;
      completed_ = std::move(owned);
      result_ = std::move(result);
    }
    wake_.notify_all();
    if (onUIThread() && !applying_)
      applyReady();
    else
      notify_();
    return true;
  }

  /** UI only. Acknowledge only after every result effect has finished. */
  void applyReady() {
    // Applying runs view effects, so it is the UI thread's alone.
    if (applying_ || !onUIThread())
      return;
    // Early effects belong to a job that is still computing or to the result
    // below; either way they come first.
    drainEarly();
    std::unique_ptr<Result> result;
    std::unique_ptr<Job> job;
    {
      std::lock_guard lock(mutex_);
      if (!result_)
        return;
      result = std::move(result_);
      job = std::move(completed_);
    }

    const bool wasApplying = applying_;
    const auto uiStart = job->profileId
        ? LoomWorkerProfile::Clock::now()
        : LoomWorkerProfile::Clock::time_point{};
    applying_ = true;
    try {
      if (!job->active || job->active()) {
        if (result->error)
          std::rethrow_exception(result->error);
        if (job->applied)
          job->applied();
        if (job->synchronous) {
          if (job->profileId) {
            LoomWorkerProfile::metrics() = {};
            result->computeStart = LoomWorkerProfile::Clock::now();
            result->computedOnMain = true;
          }
          job->compute();
          if (job->profileId) {
            result->computeEnd = LoomWorkerProfile::Clock::now();
            result->profile = LoomWorkerProfile::metrics();
          }
        }
        for (auto& effect : result->effects) {
          if (job->active && !job->active())
            break;
          effect();
        }
      }
    } catch (...) {
      // The failure path is the only error contract: `failed` stops the surface.
      // Rethrowing here would escape the platform dispatch block (a
      // dispatch_async block on iOS, a JNativeRunnable on Android) and terminate
      // the process, so the error is consumed instead.
      const auto error = std::current_exception();
      try {
        if (job->failed)
          job->failed(error);
      } catch (...) {
      }
      applying_ = wasApplying;
      recycleEffects(*result);
      acknowledge();
      return;
    }
    applying_ = wasApplying;
    recycleEffects(*result);

    const auto uiEnd = job->profileId
        ? LoomWorkerProfile::Clock::now()
        : LoomWorkerProfile::Clock::time_point{};
#if defined(__APPLE__)
    if (job->profileId) {
      using P = LoomWorkerProfile;
      os_log(
          OS_LOG_DEFAULT,
          "LoomWorkerProfile id=%u sync=%d computeMain=%d uiMain=%d queue=%.3f compute=%.3f delivery=%.3f ui=%.3f prepare=%.3f layout=%.3f diff=%.3f effects=%.3f text=%.3f textCalls=%u",
          job->profileId,
          job->synchronous,
          result->computedOnMain,
          pthread_main_np(),
          P::ms(job->submitted, result->computeStart),
          P::ms(result->computeStart, result->computeEnd),
          job->synchronous ? 0.0 : P::ms(result->computeEnd, uiStart),
          P::ms(uiStart, uiEnd),
          result->profile.prepare,
          result->profile.layout,
          result->profile.diff,
          result->profile.effects,
          result->profile.text,
          result->profile.textCalls);
      // The bench harness reads the device console, which has stderr only.
      std::fprintf(
          stderr,
          "LoomWorkerProfile id=%u queue=%.3f compute=%.3f delivery=%.3f ui=%.3f prepare=%.3f layout=%.3f diff=%.3f text=%.3f textCalls=%u\n",
          job->profileId,
          P::ms(job->submitted, result->computeStart),
          P::ms(result->computeStart, result->computeEnd),
          job->synchronous ? 0.0 : P::ms(result->computeEnd, uiStart),
          P::ms(uiStart, uiEnd),
          result->profile.prepare,
          result->profile.layout,
          result->profile.diff,
          result->profile.text,
          result->profile.textCalls);
    }
#elif defined(__ANDROID__)
    if (job->profileId) {
      using P = LoomWorkerProfile;
      __android_log_print(
          ANDROID_LOG_INFO,
          "LoomWorkerProfile",
          "id=%u sync=%d computeMain=%d uiMain=%d queue=%.3fms compute=%.3fms delivery=%.3fms ui=%.3fms prepare=%.3fms layout=%.3fms diff=%.3fms effects=%.3fms text=%.3fms textCalls=%u",
          job->profileId,
          job->synchronous,
          result->computedOnMain,
          static_cast<pid_t>(::syscall(SYS_gettid)) == ::getpid(),
          P::ms(job->submitted, result->computeStart),
          P::ms(result->computeStart, result->computeEnd),
          job->synchronous ? 0.0 : P::ms(result->computeEnd, uiStart),
          P::ms(uiStart, uiEnd),
          result->profile.prepare,
          result->profile.layout,
          result->profile.diff,
          result->profile.effects,
          result->profile.text,
          result->profile.textCalls);
    }
#endif
    acknowledge();
  }

  /**
   * UI only. Like `synchronize`, but gives up after \p budget instead of
   * waiting for a slow worker commit. Used right before a frame's traversal:
   * a large commit that is not ready then lands in the next frame (the worker
   * posts its result to the UI thread when it finishes) instead of holding
   * this frame hostage. Results that are ready are applied either way.
   */
  void synchronizeFor(std::chrono::microseconds budget) {
    if (!onUIThread()) {
      return;
    }
    if (applying_)
      return;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        const bool ready = wake_.wait_until(lock, deadline, [this] {
          return stopping_ || result_ || (!running_ && jobs_.empty());
        });
        if (!ready || stopping_ || (!result_ && !running_ && jobs_.empty()))
          return;
      }
      applyReady();
    }
  }

  /** Drain worker results on the UI thread until prior work is acknowledged. */
  void synchronize() {
    if (!onUIThread()) {
      // Another thread cannot apply results; it waits for the UI thread to.
      awaitApplied();
      return;
    }
    if (applying_)
      return;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || result_ || (!running_ && jobs_.empty());
        });
        if (stopping_ || (!result_ && !running_ && jobs_.empty()))
          return;
      }
      applyReady();
    }
  }

  /**
   * Any thread except the UI thread. Blocks until the UI thread has applied
   * every job submitted before this call, so a reader sees a renderer that
   * includes all of them. Only JS-to-UI waits are allowed: the UI thread never
   * waits on the caller, which is why this must not be used with a lock the UI
   * thread needs held. A watchdog logs and gives up rather than deadlocking.
   */
  void awaitApplied() {
    using namespace std::chrono_literals;
    constexpr auto kSlice = 2000ms;
    constexpr int kMaxSlices = 3;
    std::unique_lock lock(mutex_);
    const auto target = submitted_;
    for (int slice = 0; slice < kMaxSlices; ++slice) {
      if (wake_.wait_for(lock, kSlice, [this, target] {
            return stopping_ || acknowledged_ >= target;
          }))
        return;
      logStall(slice + 1);
    }
  }

  bool isApplying() const {
    return applying_;
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      jobs_.clear();
    }
    wake_.notify_all();
#if defined(__APPLE__)
    if (workerThread_ != 0) {
      pthread_join(workerThread_, nullptr);
      workerThread_ = 0;
    }
#endif
    if (thread_.joinable())
      thread_.join();
    result_.reset();
    completed_.reset();
  }

  /**
   * Like `defer`, but while a job computes on the worker the effect is handed to
   * the UI thread at once instead of with the job's result, so the UI thread can
   * work on it while the worker finishes the job. Early effects are applied in
   * order and always before the result of the job that produced them (and of
   * every later job): `applyReady` drains them first. Computed anywhere else
   * (inline on the UI thread, or outside a job) this is exactly `defer`.
   */
  template <typename... Args>
  std::function<void(Args...)> early(std::function<void(Args...)> operation) {
    if (!operation)
      return {};
    // One shared copy of the operation serves the deferred path, every early
    // effect and the wrapper itself; effects hold a pointer, never a copy.
    auto shared = std::make_shared<std::function<void(Args...)>>(
        std::move(operation));
    auto deferred = deferShared(shared);
    return [this, shared, deferred = std::move(deferred)](Args... args) {
      if (!building_ || onUIThread()) {
        deferred(std::forward<Args>(args)...);
        return;
      }
      Effect effect(
          [shared, values = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(*shared, std::move(values));
          });
      {
        std::lock_guard lock(mutex_);
        if (stopping_)
          return;
        earlyEffects_.emplace_back(std::move(effect));
      }
      notify_();
    };
  }

  /** UI only. Runs the early effects posted so far, in order. */
  void drainEarly() {
    if (!onUIThread())
      return;
    for (;;) {
      std::vector<Effect> effects;
      {
        std::lock_guard lock(mutex_);
        if (earlyEffects_.empty())
          return;
        effects.swap(earlyEffects_);
      }
      const bool wasApplying = applying_;
      applying_ = true;
      for (auto& effect : effects) {
        try {
          effect();
        } catch (...) {
          // As for result effects, a failure must not escape the platform's
          // dispatch block; the job's own result reports the surface failure.
        }
      }
      applying_ = wasApplying;
    }
  }

  /**
   * Capture native effects during worker computation for ordered UI replay.
   * Arguments the caller passes as temporaries are moved into the effect.
   */
  template <typename... Args>
  std::function<void(Args...)> defer(std::function<void(Args...)> operation) {
    if (!operation)
      return {};
    return deferShared(std::make_shared<std::function<void(Args...)>>(
        std::move(operation)));
  }

 private:
  template <typename... Args>
  std::function<void(Args...)> deferShared(
      std::shared_ptr<std::function<void(Args...)>> shared) {
    return [shared = std::move(shared)](Args... args) {
      if (!building_) {
        (*shared)(std::forward<Args>(args)...);
        return;
      }
      building_->effects.emplace_back(Effect(
          [shared, values = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(*shared, std::move(values));
          }));
    };
  }

  /** A result whose effects vector reuses the capacity of an earlier one. */
  std::unique_ptr<Result> newResult() {
    auto result = std::make_unique<Result>();
    std::lock_guard lock(mutex_);
    result->effects = std::move(spareEffects_);
    return result;
  }

  /**
   * Destroys an applied result's effects and keeps the vector for the next job,
   * unless it is far larger than what recent jobs needed (the same idea as
   * `LoomRenderer::trimScratch`): a burst of effects must not pin its peak.
   */
  void recycleEffects(Result& result) {
    auto effects = std::move(result.effects);
    const size_t used = effects.size();
    effects.clear();
    std::lock_guard lock(mutex_);
    effectPeak_ = std::max(used, effectPeak_ - effectPeak_ / 8);
    if (effects.capacity() <= 4 * std::max<size_t>(effectPeak_, 16)) {
      spareEffects_ = std::move(effects);
    } else {
      spareEffects_ = std::vector<Effect>();
    }
  }

#if defined(__APPLE__)
  // `std::thread` inherits the 512 KB platform default on Apple platforms. This
  // thread runs Yoga plus several recursive `std::function` tree walks
  // (prepare, selectDraft, captureRemovedMountedEdges, overflow and the identity
  // collect), so it is created explicitly with a stack at least as large as the
  // JS thread's, which React Native doubles under RCT_DEBUG.
  static constexpr size_t kWorkerStackSize = 2 * 1024 * 1024;
#endif

  void startThread() {
#if defined(__APPLE__)
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kWorkerStackSize);
    const auto created = pthread_create(
        &workerThread_,
        &attributes,
        [](void* context) -> void* {
          // Named so profilers and the bench probe can tell it apart.
          pthread_setname_np("com.loom.worker");
          static_cast<LoomWorker*>(context)->run();
          return nullptr;
        },
        this);
    pthread_attr_destroy(&attributes);
    if (created == 0) {
      return;
    }
    workerThread_ = 0;
#endif
    // Everything else, and the fallback when the explicit stack cannot be
    // created, uses the platform default stack.
    thread_ = std::thread([this] {
      run();
    });
  }

  void logStall(int slice) {
#if defined(__ANDROID__)
    __android_log_print(
        ANDROID_LOG_WARN,
        "LoomWorker",
        "off-UI barrier waited %d ms for the UI thread to apply worker results",
        slice * 2000);
#else
    std::fprintf(
        stderr, "LoomWorker: barrier waited %d ms for the UI thread\n", slice * 2000);
#endif
  }

  void acknowledge() {
    {
      std::lock_guard lock(mutex_);
      running_ = false;
      ++acknowledged_;
    }
    wake_.notify_all();
  }

  void run() {
    for (;;) {
      std::unique_ptr<Job> job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || (!running_ && !jobs_.empty());
        });
        if (stopping_)
          return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
        running_ = true;
      }

      auto result = newResult();
      if (job->profileId) {
        LoomWorkerProfile::metrics() = {};
        result->computeStart = LoomWorkerProfile::Clock::now();
#if defined(__APPLE__)
        result->computedOnMain = pthread_main_np();
#elif defined(__ANDROID__)
        result->computedOnMain =
            static_cast<pid_t>(::syscall(SYS_gettid)) == ::getpid();
#endif
      }
      building_ = result.get();
      try {
        if (!job->synchronous && (!job->active || job->active()))
          job->compute();
      } catch (...) {
        result->error = std::current_exception();
      }
      building_ = nullptr;
      if (job->profileId) {
        result->computeEnd = LoomWorkerProfile::Clock::now();
        result->profile = LoomWorkerProfile::metrics();
      }

      {
        std::lock_guard lock(mutex_);
        if (stopping_)
          return;
        completed_ = std::move(job);
        result_ = std::move(result);
      }
      wake_.notify_all();
      notify_();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::unique_ptr<Job>> jobs_;
  std::unique_ptr<Job> completed_;
  std::unique_ptr<Result> result_;
  std::vector<Effect> earlyEffects_;
  std::vector<Effect> spareEffects_; // guarded by mutex_
  size_t effectPeak_{0}; // guarded by mutex_
  static inline thread_local Result* building_{nullptr};
  bool running_{false};
  bool stopping_{false};
  bool applying_{false}; // UI only: every access is behind `onUIThread()`.
  // Jobs accepted and jobs fully applied. Results are applied in submission
  // order, so `acknowledged_ >= n` means the first n jobs are on screen.
  uint64_t submitted_{0};
  uint64_t acknowledged_{0};
  std::function<bool()> isUIThread_;
  unsigned profileSequence_{0};
  std::function<void()> notify_;
  std::thread thread_;
#if defined(__APPLE__)
  pthread_t workerThread_{0};
#endif
};
} // namespace facebook::react
