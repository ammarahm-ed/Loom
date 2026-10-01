/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file.
 */
#pragma once
#include "UIThreadRendererWorkerProfile.h"

#include <condition_variable>
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
    unsigned profileId{0};
    UIThreadRendererWorkerProfile::Clock::time_point submitted{};
  };

  explicit UIThreadRendererWorker(std::function<void()> notify)
      : notify_(std::move(notify)) {
    startThread();
  }
  ~UIThreadRendererWorker() {
    shutdown();
  }
  UIThreadRendererWorker(const UIThreadRendererWorker&) = delete;
  UIThreadRendererWorker& operator=(const UIThreadRendererWorker&) = delete;

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
    if (!job || !job->synchronous)
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
      completed_ = std::move(job);
      result_ = std::make_unique<Result>();
    }
    applyReady();
    return true;
  }

  /** UI only. Acknowledge only after every result effect has finished. */
  void applyReady() {
    if (applying_)
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

  /** Drain worker results on the UI thread until prior work is acknowledged. */
  void synchronize() {
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

  void acknowledge() {
    {
      std::lock_guard lock(mutex_);
      running_ = false;
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
  bool applying_{false}; // UI only.
  unsigned profileSequence_{0};
  std::function<void()> notify_;
  std::thread thread_;
#if defined(__APPLE__)
  pthread_t workerThread_{0};
#endif
};
} // namespace facebook::react
