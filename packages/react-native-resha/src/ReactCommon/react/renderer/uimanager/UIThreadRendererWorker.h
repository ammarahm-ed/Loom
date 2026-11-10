/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file.
 */
#pragma once
#include "UIThreadRendererWorkerProfile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
class UIThreadRendererWorker final {
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

  struct Result {
    std::vector<std::function<void()>> effects;
    std::exception_ptr error;
    UIThreadRendererWorkerProfile::Metrics profile;
    UIThreadRendererWorkerProfile::Clock::time_point computeStart{},
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
    // React Native X: the producer estimates this commit as cheaper than a
    // worker hand-off. `tryRunInline` then computes it on the calling thread
    // when the worker is idle.
    bool inlineEligible{false};
    unsigned profileId{0};
    UIThreadRendererWorkerProfile::Clock::time_point submitted{};
  };

  /**
   * Whether the JavaScript runtime is hosted by the UI thread. When it is not
   * (Android with `RN_UI_THREAD` off), JS-thread callers may wait for the UI
   * thread but the UI thread must never wait for JS, and result application is
   * confined to the UI thread. iOS and the host test build always host JS on the
   * thread that applies results.
   */
#if defined(__ANDROID__) && !RN_UI_THREAD
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

  explicit UIThreadRendererWorker(
      std::function<void()> notify,
      std::function<bool()> isUIThread = {})
      : isUIThread_(isUIThread ? std::move(isUIThread) : isPlatformUIThread),
        notify_(std::move(notify)) {
    startThread();
  }
  ~UIThreadRendererWorker() {
    shutdown();
  }
  UIThreadRendererWorker(const UIThreadRendererWorker&) = delete;
  UIThreadRendererWorker& operator=(const UIThreadRendererWorker&) = delete;

  /** True on the thread that owns result application. */
  bool onUIThread() const {
    return isUIThread_();
  }

  void submit(std::unique_ptr<Job> job) {
    if (job->profileCommit && UIThreadRendererWorkerProfile::enabled()) {
      job->submitted = UIThreadRendererWorkerProfile::Clock::now();
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
      if (job->profileCommit && UIThreadRendererWorkerProfile::enabled()) {
        job->submitted = UIThreadRendererWorkerProfile::Clock::now();
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
   * React Native X: computes an asynchronous job on the calling thread instead
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
      if (job->profileCommit && UIThreadRendererWorkerProfile::enabled()) {
        job->submitted = UIThreadRendererWorkerProfile::Clock::now();
        job->profileId = ++profileSequence_;
      }
      running_ = true;
      ++submitted_;
    }
    auto owned = std::move(job);
    auto result = std::make_unique<Result>();
    if (owned->profileId) {
      UIThreadRendererWorkerProfile::metrics() = {};
      result->computeStart = UIThreadRendererWorkerProfile::Clock::now();
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
      result->computeEnd = UIThreadRendererWorkerProfile::Clock::now();
      result->profile = UIThreadRendererWorkerProfile::metrics();
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
        ? UIThreadRendererWorkerProfile::Clock::now()
        : UIThreadRendererWorkerProfile::Clock::time_point{};
    applying_ = true;
    try {
      if (!job->active || job->active()) {
        if (result->error)
          std::rethrow_exception(result->error);
        if (job->applied)
          job->applied();
        if (job->synchronous) {
          if (job->profileId) {
            UIThreadRendererWorkerProfile::metrics() = {};
            result->computeStart = UIThreadRendererWorkerProfile::Clock::now();
            result->computedOnMain = true;
          }
          job->compute();
          if (job->profileId) {
            result->computeEnd = UIThreadRendererWorkerProfile::Clock::now();
            result->profile = UIThreadRendererWorkerProfile::metrics();
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
      acknowledge();
      return;
    }
    applying_ = wasApplying;

    const auto uiEnd = job->profileId
        ? UIThreadRendererWorkerProfile::Clock::now()
        : UIThreadRendererWorkerProfile::Clock::time_point{};
#if defined(__APPLE__)
    if (job->profileId) {
      using P = UIThreadRendererWorkerProfile;
      os_log(
          OS_LOG_DEFAULT,
          "RNXWorkerProfile id=%u sync=%d computeMain=%d uiMain=%d queue=%.3f compute=%.3f delivery=%.3f ui=%.3f prepare=%.3f layout=%.3f diff=%.3f effects=%.3f text=%.3f textCalls=%u",
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
    }
#elif defined(__ANDROID__)
    if (job->profileId) {
      using P = UIThreadRendererWorkerProfile;
      __android_log_print(
          ANDROID_LOG_INFO,
          "RNXWorkerProfile",
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

  /** Capture native effects during worker computation for ordered UI replay. */
  template <typename... Args>
  std::function<void(Args...)> defer(std::function<void(Args...)> operation) {
    if (!operation)
      return {};
    return [operation = std::move(operation)](Args... args) {
      if (!building_) {
        operation(std::forward<Args>(args)...);
        return;
      }
      auto values = std::make_tuple(std::forward<Args>(args)...);
      building_->effects.emplace_back(
          [operation, values = std::move(values)]() mutable {
            std::apply(operation, std::move(values));
          });
    };
  }

 private:
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
          static_cast<UIThreadRendererWorker*>(context)->run();
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
        "RNXWorker",
        "off-UI barrier waited %d ms for the UI thread to apply worker results",
        slice * 2000);
#else
    std::fprintf(
        stderr, "RNXWorker: barrier waited %d ms for the UI thread\n", slice * 2000);
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

      auto result = std::make_unique<Result>();
      if (job->profileId) {
        UIThreadRendererWorkerProfile::metrics() = {};
        result->computeStart = UIThreadRendererWorkerProfile::Clock::now();
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
        result->computeEnd = UIThreadRendererWorkerProfile::Clock::now();
        result->profile = UIThreadRendererWorkerProfile::metrics();
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
