/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RuntimeScheduler_Modern.h"

#include <ReactCommon/RuntimeExecutorSyncUIThreadUtils.h>
#include <cxxreact/TraceSection.h>
#include <jsinspector-modern/tracing/EventLoopReporter.h>
#include <react/performance/timeline/PerformanceEntryReporter.h>
#include <react/renderer/consistency/ScopedShadowTreeRevisionLock.h>
#include <react/timing/primitives.h>
#include <react/utils/OnScopeExit.h>
#include "JankReporter.h"
#include "LoomJavaScriptThread.h"
#include "RuntimeSchedulerFrameCallbackScope.h"

#include <atomic>
#include <optional>
#include <thread>

namespace facebook::react {

namespace {
HighResDuration getResolvedTimeoutForIdleTask(HighResDuration customTimeout) {
  return customTimeout <
          timeoutForSchedulerPriority(SchedulerPriority::IdlePriority)
      ? timeoutForSchedulerPriority(SchedulerPriority::LowPriority) +
          customTimeout
      : customTimeout;
}

int64_t durationToMilliseconds(HighResDuration duration) {
  return duration.toNanoseconds() / static_cast<int64_t>(1e6);
}

// Main mode (JavaScript on the UI thread) only; see `jsOnUIThread_`.
// How long non-urgent work may run in one UI-thread message before React is asked to yield.
// A 60 Hz frame is 16.7 ms.
constexpr HighResDuration kLoopSliceYield = HighResDuration::fromMilliseconds(5);
constexpr HighResDuration kLoopSliceMinimum = HighResDuration::fromMilliseconds(1);

/*
 * Loom: the runtime executing on this thread right now, if any.
 *
 * Upstream set the equivalent flag only inside `executeNowOnTheSameThread`'s own
 * callback, because the runtime always ran on a dedicated JS queue thread and a
 * synchronous request therefore always arrived from a *different* thread. Loom
 * runs the runtime on the UI thread, so a synchronous request can arrive from
 * the runtime thread itself; upstream's post-and-park algorithm in
 * `executeSynchronouslyOnSameThread_CAN_DEADLOCK` deadlocks there, because the
 * capture block it posts to the runtime queue can never run while this thread
 * blocks waiting for it.
 *
 * Publishing the runtime for the whole duration of an event loop tick routes
 * such requests to the direct-execution path upstream already has for
 * re-entrant calls.
 */
thread_local jsi::Runtime* currentRuntimeOnThread = nullptr;

/*
 * The thread that owns the runtime, recorded the first time the runtime
 * executes on it.
 *
 * `currentRuntimeOnThread` is scoped to a single event loop tick, so between
 * ticks there is no way to tell whether the calling thread is the runtime's
 * own thread. That distinction decides whether a synchronous request may park
 * waiting for the runtime queue (safe from another thread) or not (a deadlock
 * on the runtime's own thread).
 */
std::atomic<std::thread::id> runtimeThreadId{};

/** True when the calling thread is the thread the runtime executes on. */
bool isOnRuntimeThread() noexcept {
  return runtimeThreadId.load(std::memory_order_acquire) ==
      std::this_thread::get_id();
}

/*
 * Loom: the runtime this thread last executed, remembered beyond the
 * tick that observed it.
 *
 * `currentRuntimeOnThread` is scoped to a single tick, so a synchronous request
 * that arrives *between* ticks on the runtime thread has no runtime to execute
 * against. Remembering it (the runtime outlives the ticks that run on it, and a
 * reload publishes its replacement here the first time it ticks) lets that
 * request run directly instead of posting to a queue only this thread drains.
 * The pointer is deliberately not restored on scope exit: the thread keeps
 * owning the runtime after the tick ends.
 */
thread_local jsi::Runtime* runtimeForThread = nullptr;

/** Background mode: the runtime a synchronous request is running on this thread, if any. */
constinit thread_local jsi::Runtime* backgroundRuntimeOnThread = nullptr;

/** Publishes `runtime` as the current thread's runtime, restoring the previous one. */
class CurrentRuntimeScope final {
 public:
  explicit CurrentRuntimeScope(jsi::Runtime& runtime) noexcept
      : previous_(currentRuntimeOnThread) {
    currentRuntimeOnThread = &runtime;
    runtimeForThread = &runtime;
    runtimeThreadId.store(std::this_thread::get_id(), std::memory_order_release);
  }

  ~CurrentRuntimeScope() noexcept {
    currentRuntimeOnThread = previous_;
  }

  CurrentRuntimeScope(const CurrentRuntimeScope&) = delete;
  CurrentRuntimeScope& operator=(const CurrentRuntimeScope&) = delete;

 private:
  jsi::Runtime* previous_;
};

} // namespace

#pragma mark - Public

RuntimeScheduler_Modern::RuntimeScheduler_Modern(
    RuntimeExecutor runtimeExecutor,
    std::function<HighResTimeStamp()> now,
    RuntimeSchedulerTaskErrorHandler onTaskError)
    : runtimeExecutor_(std::move(runtimeExecutor)),
      now_(std::move(now)),
      onTaskError_(std::move(onTaskError)),
      jsOnUIThread_(loomJavaScriptOnUIThread()) {}

void RuntimeScheduler_Modern::scheduleWork(RawCallback&& callback) noexcept {
  TraceSection s("RuntimeScheduler::scheduleWork");
  scheduleTask(SchedulerPriority::ImmediatePriority, std::move(callback));
}

void RuntimeScheduler_Modern::scheduleTask(const std::function<void()>& task) {
  scheduleIdleTask([task](jsi::Runtime& /*runtime*/) { task(); });
}

uint64_t RuntimeScheduler_Modern::registerTaskQueueSource() {
  // It's fine to wrap around, as it's impossible to hold so many live task
  // queue sources in practice.
  return nextTaskQueueSourceId_.fetch_add(1) + 1;
}

void RuntimeScheduler_Modern::unregisterTaskQueueSource(uint64_t /*sourceId*/) {
  // For now, we don't need to do unregistering. The reason is that the event
  // loop of the runtime scheduler doesn't need to be blocked on the task
  // producer of IEventLoopControl. When the event loop ends, we just ignore
  // all queueing tasks.
}

std::shared_ptr<Task> RuntimeScheduler_Modern::scheduleTask(
    SchedulerPriority priority,
    jsi::Function&& callback) noexcept {
  auto expirationTime = now_() + timeoutForSchedulerPriority(priority);
  auto task =
      std::make_shared<Task>(priority, std::move(callback), expirationTime);

  scheduleTask(task);

  return task;
}

std::shared_ptr<Task> RuntimeScheduler_Modern::scheduleTask(
    SchedulerPriority priority,
    RawCallback&& callback) noexcept {
  auto expirationTime = now_() + timeoutForSchedulerPriority(priority);
  auto task =
      std::make_shared<Task>(priority, std::move(callback), expirationTime);

  scheduleTask(task);

  return task;
}

std::shared_ptr<Task> RuntimeScheduler_Modern::scheduleIdleTask(
    jsi::Function&& callback,
    HighResDuration customTimeout) noexcept {
  TraceSection s(
      "RuntimeScheduler::scheduleIdleTask",
      "customTimeout",
      durationToMilliseconds(customTimeout),
      "callbackType",
      "jsi::Function");

  auto timeout = getResolvedTimeoutForIdleTask(customTimeout);
  auto expirationTime = now_() + timeout;
  auto task = std::make_shared<Task>(
      SchedulerPriority::IdlePriority, std::move(callback), expirationTime);

  scheduleTask(task);

  return task;
}

std::shared_ptr<Task> RuntimeScheduler_Modern::scheduleIdleTask(
    RawCallback&& callback,
    HighResDuration customTimeout) noexcept {
  TraceSection s(
      "RuntimeScheduler::scheduleIdleTask",
      "customTimeout",
      durationToMilliseconds(customTimeout),
      "callbackType",
      "RawCallback");

  auto expirationTime = now_() + getResolvedTimeoutForIdleTask(customTimeout);
  auto task = std::make_shared<Task>(
      SchedulerPriority::IdlePriority, std::move(callback), expirationTime);

  scheduleTask(task);

  return task;
}

bool RuntimeScheduler_Modern::getShouldYield() noexcept {
  markYieldingOpportunity(now_());

  std::shared_lock lock(schedulingMutex_);

  if (syncTaskRequests_ > 0 ||
      (!taskQueue_.empty() && taskQueue_.top().get() != currentTask_)) {
    return true;
  }
  // Loom: RN gives React the native scheduler, which until now only
  // asked a transition render to yield for a higher-priority task, never for
  // time. With JavaScript on the UI thread, a large transition then holds the
  // message queue (and so the next frame) until it is finished. Ask non-urgent work to
  // yield once it has used its slice. Urgent work (immediate and user-blocking
  // priorities, synchronous requests) is never asked to.
  if (jsOnUIThread_ && currentTask_ != nullptr &&
      currentPriority_ >= SchedulerPriority::NormalPriority) {
    LOOM_MODE_COUNT(schedulerMainPaths);
    if ((now_() - loopSliceStart_) >= kLoopSliceYield) {
      timeSliceYielded_ = true;
      return true;
    }
    // The deadline rule needs a millisecond of progress first, so a slice that starts right
    // before a vsync still renders something instead of yielding forever.
    if ((now_() - loopSliceStart_) >= kLoopSliceMinimum &&
        FrameClock::nextVsyncIsNear()) {
      timeSliceYielded_ = true;
      yieldedForFrame_ = true;
      return true;
    }
  }
  return false;
}

void RuntimeScheduler_Modern::cancelTask(Task& task) noexcept {
  task.callback.reset();
}

SchedulerPriority RuntimeScheduler_Modern::getCurrentPriorityLevel()
    const noexcept {
  return currentPriority_;
}

HighResTimeStamp RuntimeScheduler_Modern::now() const noexcept {
  return now_();
}

void RuntimeScheduler_Modern::executeNowOnTheSameThread(
    RawCallback&& callback) {
  TraceSection s("RuntimeScheduler::executeNowOnTheSameThread");

  auto currentTime = now_();
  auto priority = SchedulerPriority::ImmediatePriority;
  auto expirationTime = currentTime + timeoutForSchedulerPriority(priority);
  Task task{priority, std::move(callback), expirationTime};

  if (jsOnUIThread_) {
  if (currentRuntimeOnThread != nullptr) {
    // Protecting against re-entry into `executeNowOnTheSameThread` from within
    // `executeNowOnTheSameThread`. Without accounting for re-rentry, a deadlock
    // will occur when trying to gain access to the runtime.
    //
    // Loom: on the single-thread execution model this branch is also taken by
    // ordinary synchronous requests issued from JavaScript running on the
    // runtime thread, not only by genuinely nested ones. See
    // `currentRuntimeOnThread`. Such a request must run here either way: its
    // caller needs the result before it returns, and the queue it would park on
    // is drained by this very thread, so posting to it can never complete. The
    // task keeps its ImmediatePriority, so it is not deprioritized; what it does
    // skip is waiting for other pending tasks of equal priority.
    return executeTask(*currentRuntimeOnThread, task, true);
  }
  if (isOnRuntimeThread()) {
    // Between ticks on the runtime's own thread. This is the case
    // `executeSynchronouslyOnSameThread_CAN_DEADLOCK` cannot serve: it posts the
    // work to the runtime queue and parks, but under the single-thread model
    // this thread is the one that would have to run it. Execute directly
    // instead. If no runtime has been observed on this thread yet there is
    // nothing to execute against, so fall through to the posted path.
    if (auto* runtime = runtimeForThread; runtime != nullptr) {
      // A full event loop tick, not a bare task: the work JavaScript queues
      // while handling the request (React schedules a discrete event's render
      // as a microtask) must run before this returns. A bare task left it
      // waiting for whatever task the loop ran next, e.g. a Pressable timer
      // 130 ms later, which defeats delivering the event synchronously.
      runEventLoopTick(*runtime, task);
      bool shouldScheduleEventLoop = false;
      {
        std::unique_lock lock(schedulingMutex_);
        if (!taskQueue_.empty() && !isEventLoopScheduled_) {
          isEventLoopScheduled_ = true;
          shouldScheduleEventLoop = true;
        }
      }
      if (shouldScheduleEventLoop) {
        scheduleEventLoop();
      }
      return;
    }
  }
  } else {
    if (backgroundRuntimeOnThread != nullptr) {
      // Protecting against re-entry into `executeNowOnTheSameThread` from within
      // `executeNowOnTheSameThread`. Without accounting for re-rentry, a deadlock
      // will occur when trying to gain access to the runtime.
      return executeTask(*backgroundRuntimeOnThread, task, true);
    }
  }

  syncTaskRequests_++;
  executeSynchronouslyOnSameThread_CAN_DEADLOCK(
      runtimeExecutor_, [&](jsi::Runtime& runtime) mutable {
        TraceSection s2("RuntimeScheduler::executeNowOnTheSameThread callback");

        syncTaskRequests_--;
        if (jsOnUIThread_) {
          CurrentRuntimeScope currentRuntimeScope(runtime);
          runEventLoopTick(runtime, task);
        } else {
          backgroundRuntimeOnThread = &runtime;
          runEventLoopTick(runtime, task);
          backgroundRuntimeOnThread = nullptr;
        }
      });

  bool shouldScheduleEventLoop = false;

  {
    // Unique access because we might write to `isEventLoopScheduled_`.
    std::unique_lock lock(schedulingMutex_);

    // We only need to schedule the event loop if there any remaining tasks
    // in the queue.
    if (!taskQueue_.empty() && !isEventLoopScheduled_) {
      isEventLoopScheduled_ = true;
      shouldScheduleEventLoop = true;
    }
  }

  if (shouldScheduleEventLoop) {
    scheduleEventLoop();
  }
}

void RuntimeScheduler_Modern::callExpiredTasks(jsi::Runtime& runtime) {
  // No-op in the event loop implementation.
}

void RuntimeScheduler_Modern::scheduleRenderingUpdate(
    SurfaceId surfaceId,
    RuntimeSchedulerRenderingUpdate&& renderingUpdate) {
  TraceSection s("RuntimeScheduler::scheduleRenderingUpdate");

  surfaceIdsWithPendingRenderingUpdates_.insert(surfaceId);
  pendingRenderingUpdates_.push(renderingUpdate);
}

void RuntimeScheduler_Modern::setRenderingUpdateListener(
    std::function<void()> listener) {
  renderingUpdateListener_ = std::move(listener);
}

void RuntimeScheduler_Modern::setShadowTreeRevisionConsistencyManager(
    ShadowTreeRevisionConsistencyManager*
        shadowTreeRevisionConsistencyManager) {
  shadowTreeRevisionConsistencyManager_ = shadowTreeRevisionConsistencyManager;
}

void RuntimeScheduler_Modern::setPerformanceEntryReporter(
    PerformanceEntryReporter* performanceEntryReporter) {
  performanceEntryReporter_ = performanceEntryReporter;
}

void RuntimeScheduler_Modern::setEventTimingDelegate(
    RuntimeSchedulerEventTimingDelegate* eventTimingDelegate) {
  eventTimingDelegate_ = eventTimingDelegate;
}

void RuntimeScheduler_Modern::setIntersectionObserverDelegate(
    RuntimeSchedulerIntersectionObserverDelegate*
        intersectionObserverDelegate) {
  intersectionObserverDelegate_ = intersectionObserverDelegate;
}

void RuntimeScheduler_Modern::setResizeObserverDelegate(
    RuntimeSchedulerResizeObserverDelegate* resizeObserverDelegate) {
  resizeObserverDelegate_ = resizeObserverDelegate;
}

#pragma mark - Private

void RuntimeScheduler_Modern::scheduleTask(std::shared_ptr<Task> task) {
  TraceSection s(
      "RuntimeScheduler::scheduleTask",
      "priority",
      serialize(task->priority),
      "id",
      task->id);

  bool shouldScheduleEventLoop = false;

  {
    std::unique_lock lock(schedulingMutex_);

    // We only need to schedule the event loop if the task we're about to
    // schedule is the only one in the queue.
    // Otherwise, we don't need to schedule it because there's another one
    // running already that will pick up the new task.
    if (taskQueue_.empty() && !isEventLoopScheduled_) {
      isEventLoopScheduled_ = true;
      shouldScheduleEventLoop = true;
    }

    // Loom: a task scheduled by requestAnimationFrame work is such work too.
    if (AnimationFrameWork::active()) {
      animationFrameTasks_.insert(task.get());
    }
    taskQueue_.push(std::move(task));
  }

  if (shouldScheduleEventLoop) {
    scheduleEventLoop();
  }
}

void RuntimeScheduler_Modern::scheduleEventLoop() {
  // Loom (main mode only): during the host's animation phase the loop is run by
  // the FrameCallbackScope when it closes (still before this frame is
  // drawn) instead of through a posted message, which can queue behind the
  // frame and push the resulting commit a frame later.
  if (auto* scope = jsOnUIThread_ ? FrameCallbackScope::current() : nullptr;
      scope != nullptr && currentRuntimeOnThread == nullptr &&
      runtimeForThread != nullptr) {
    auto* runtime = runtimeForThread;
    scope->kick = [this, runtime] { runEventLoop(*runtime); };
    return;
  }
  runtimeExecutor_([this](jsi::Runtime& runtime) { runEventLoop(runtime); });
}

void RuntimeScheduler_Modern::runEventLoop(jsi::Runtime& runtime) {
  TraceSection s("RuntimeScheduler::runEventLoop");

  auto previousPriority = currentPriority_;

  // `selectTask` must be called unconditionaly to ensure that
  // `isEventLoopScheduled_` is set to false and the event loop resume
  // correctly if a synchronous task is scheduled.
  // Unit test normalTaskYieldsToSynchronousAccessAndResumes covers this
  // scenario.
  bool isContinuation = false;
  if (jsOnUIThread_) {
    loopSliceStart_ = now_();
    timeSliceYielded_ = false;
    yieldedForFrame_ = false;
    isContinuation = continuationPending_;
    continuationPending_ = false;
  }
  auto topPriorityTask = selectTask();
  // A continuation that would start inside the frame's reserved time does not start: it would
  // only run a millisecond and yield again, holding the UI thread in front of the frame callback.
  // Only when continuations are held past the vsync: with an immediate re-post this check would
  // spin, re-posting itself until the vsync passes whenever nothing holds it behind the frame.
  if (jsOnUIThread_ && FrameClock::kDeferContinuationPastVsync && isContinuation && topPriorityTask &&
      topPriorityTask->priority >= SchedulerPriority::NormalPriority &&
      FrameClock::nextVsyncIsNear()) {
    {
      std::unique_lock lock(schedulingMutex_);
      isEventLoopScheduled_ = true;
    }
    continuationPending_ = true;
    std::function<void()> repost = [this] {
      runtimeExecutor_([this](jsi::Runtime& rt) { runEventLoop(rt); });
    };
    if (!FrameClock::postAfterNextFrame(repost)) {
      repost();
    }
    return;
  }
  while (topPriorityTask && syncTaskRequests_ == 0) {
    runEventLoopTick(runtime, *topPriorityTask);
    topPriorityTask = selectTask();
    // Loom (main mode only): upstream drains the whole queue inside one message, which
    // was fine on a dedicated JS thread. Here it keeps a pending frame waiting
    // until every queued task (a React render split into continuations, say) is
    // done. After a slice, hand the UI thread back by re-posting the loop; the frame
    // callback or input then runs before the next slice. Only non-urgent tasks
    // are deferred, and a task is never split.
    if (jsOnUIThread_ && topPriorityTask && timeSliceYielded_ &&
        topPriorityTask->priority >= SchedulerPriority::NormalPriority) {
      timeSliceYielded_ = false;
      {
        std::unique_lock lock(schedulingMutex_);
        isEventLoopScheduled_ = true;
      }
      continuationPending_ = true;
      std::function<void()> repost = [this] {
        runtimeExecutor_([this](jsi::Runtime& rt) { runEventLoop(rt); });
      };
      // A slice that ran out of time continues at once (the frame is not close). One that
      // yielded for the frame waits until after it: an immediate re-post would be dequeued
      // ahead of the vsync's frame callback and delay it.
      if (!(yieldedForFrame_ && FrameClock::postAfterNextFrame(repost))) {
        repost();
      }
      break;
    }
  }
  currentPriority_ = previousPriority;
}

std::shared_ptr<Task> RuntimeScheduler_Modern::selectTask() {
  // We need a unique lock here because we'll also remove executed tasks from
  // the top of the queue.
  std::unique_lock lock(schedulingMutex_);

  // It's safe to reset the flag here, as its access is also synchronized with
  // the access to the task queue.
  isEventLoopScheduled_ = false;

  // Skip executed tasks
  while (!taskQueue_.empty() && !taskQueue_.top()->callback) {
    animationFrameTasks_.erase(taskQueue_.top().get());
    taskQueue_.pop();
  }
  if (taskQueue_.empty()) {
    animationFrameTasks_.clear();
  }

  if (!taskQueue_.empty()) {
    return taskQueue_.top();
  }

  return nullptr;
}

void RuntimeScheduler_Modern::runEventLoopTick(
    jsi::Runtime& runtime,
    Task& task) {
  TraceSection s("RuntimeScheduler::runEventLoopTick");
  jsinspector_modern::tracing::EventLoopReporter performanceReporter(
      jsinspector_modern::tracing::EventLoopPhase::Task);

  ScopedShadowTreeRevisionLock revisionLock(
      shadowTreeRevisionConsistencyManager_);

  // Loom (main mode only): publish the runtime for the duration of the tick so that synchronous
  // access requested by user code running on this thread executes directly,
  // instead of posting to the very queue this thread is already running.
  std::optional<CurrentRuntimeScope> currentRuntimeScope;
  if (jsOnUIThread_) {
    LOOM_MODE_COUNT(schedulerMainPaths);
    currentRuntimeScope.emplace(runtime);
  }

  currentTask_ = &task;
  currentPriority_ = task.priority;

#if RN_JANK_REPORTER
  // Loom (debug builds only): a report about an earlier slow task is
  // logged here, at the start of a later one, never in the middle of the slow
  // task itself.
  jank::Reporter::instance().flushPending(runtime);
  jank::Reporter::instance().beginTask();
#endif

  auto taskStartTime = now_();
  lastYieldingOpportunity_ = taskStartTime;
  longestPeriodWithoutYieldingOpportunity_ = HighResDuration::zero();

  // Loom: a requestAnimationFrame callback run outside a task (a host that delivers
  // timers itself) still marks the first task after it, which holds the
  // microtasks it queued.
  {
    std::shared_lock lock(schedulingMutex_);
    if (animationFrameTasks_.count(&task) != 0) {
      AnimationFrameWork::set(true);
    }
  }

  auto didUserCallbackTimeout = task.expirationTime <= taskStartTime;
  executeTask(runtime, task, didUserCallbackTimeout);
#if RN_JANK_REPORTER
  const auto jankTaskEnd = now_();
#endif

  // "Perform a microtask checkpoint" step.
  performMicrotaskCheckpoint(runtime);

  auto taskEndTime = now_();
  markYieldingOpportunity(taskEndTime);
  reportLongTasks(task, taskStartTime, taskEndTime);

  // "Update the rendering" step.
  updateRendering(runtime, taskEndTime);

#if RN_JANK_REPORTER
  {
    const auto renderEnd = now_();
    jank::TaskTimes times;
    times.taskMs = (jankTaskEnd - taskStartTime).toNanoseconds() / 1e6;
    times.microtaskMs = (taskEndTime - jankTaskEnd).toNanoseconds() / 1e6;
    times.renderingMs = (renderEnd - taskEndTime).toNanoseconds() / 1e6;
    jank::Reporter::instance().endTask(times);
  }
#endif

  AnimationFrameWork::set(false);
  currentTask_ = nullptr;
}

/**
 * This is partially equivalent to the "Update the rendering" step in the Web
 * event loop. See
 * https://html.spec.whatwg.org/multipage/webappapis.html#update-the-rendering.
 */
void RuntimeScheduler_Modern::updateRendering(
    jsi::Runtime& runtime,
    HighResTimeStamp taskEndTime) {
  TraceSection s("RuntimeScheduler::updateRendering");

  // This is the integration of the Event Timing API in the Event Loop.
  // See https://w3c.github.io/event-timing/#sec-modifications-HTML
  const auto eventTimingDelegate = eventTimingDelegate_.load();
  if (eventTimingDelegate != nullptr) {
    eventTimingDelegate->dispatchPendingEventTimingEntries(
        taskEndTime, surfaceIdsWithPendingRenderingUpdates_);
  }

  // This is the integration of the Resize Observer API in the Event Loop. See
  // https://w3c.github.io/csswg-drafts/resize-observer/#broadcast-resize-notifications-h
  // The delegate runs the spec's depth-bounded gather/broadcast loop
  // internally; this call site stays a single invocation per tick.
  if (resizeObserverDelegate_ != nullptr) {
    // This delivers the observations to JS synchronously, so it is outside the
    // error boundary of `executeTask`. Report the error without going through
    // `handleTaskError`: we are already inside the "update the rendering" step,
    // so clearing the queues here would drop the pending rendering updates this
    // step is about to drain, and a throwing observer callback would abort the
    // remaining steps below.
    try {
      resizeObserverDelegate_->runResizeObservations(runtime);
    } catch (jsi::JSError& error) {
      onTaskError_(runtime, error);
    } catch (std::exception& ex) {
      jsi::JSError error(
          runtime, std::string("Non-JS exception: ") + ex.what());
      onTaskError_(runtime, error);
    }
  }

  // This is the integration of the Intersection Observer API in the Event Loop.
  // See
  if (intersectionObserverDelegate_ != nullptr) {
    intersectionObserverDelegate_->updateIntersectionObservations(
        surfaceIdsWithPendingRenderingUpdates_);
  }

  surfaceIdsWithPendingRenderingUpdates_.clear();

  while (!pendingRenderingUpdates_.empty()) {
    auto& pendingRenderingUpdate = pendingRenderingUpdates_.front();
    if (pendingRenderingUpdate != nullptr) {
      pendingRenderingUpdate();
    }
    pendingRenderingUpdates_.pop();
  }

  // Loom (animation fast lane): the rendering-update phase is over, so this
  // frame's animated writes are complete. Folding them here makes every JS task
  // that animated something a flush point, which is what keeps a JS-driven
  // animation on the UI thread instead of one worker job per prop write. A
  // failure must not take the task loop down with it: the flush reports its own
  // surface failures.
  if (renderingUpdateListener_ != nullptr) {
    try {
      renderingUpdateListener_();
    } catch (...) {
    }
  }
}

void RuntimeScheduler_Modern::executeTask(
    jsi::Runtime& runtime,
    Task& task,
    bool didUserCallbackTimeout) {
  TraceSection s(
      "RuntimeScheduler::executeTask",
      "id",
      task.id,
      "priority",
      serialize(task.priority),
      "didUserCallbackTimeout",
      didUserCallbackTimeout);

  try {
    auto result = task.execute(runtime, didUserCallbackTimeout);

    if (result.isObject() && result.getObject(runtime).isFunction(runtime)) {
      // If the task returned a continuation callback, we re-assign it to the
      // task and keep the task in the queue.
      task.callback = result.getObject(runtime).getFunction(runtime);
    }
  } catch (jsi::JSError& error) {
    handleTaskError(runtime, error);
  } catch (std::exception& ex) {
    jsi::JSError error(runtime, std::string("Non-JS exception: ") + ex.what());
    handleTaskError(runtime, error);
  }
}

/**
 * This is partially equivalent to the "Perform a microtask checkpoint" step in
 * the Web event loop. See
 * https://html.spec.whatwg.org/multipage/webappapis.html#perform-a-microtask-checkpoint.
 *
 * Iterates on \c drainMicrotasks until it completes or hits the retries bound.
 */
void RuntimeScheduler_Modern::performMicrotaskCheckpoint(
    jsi::Runtime& runtime) {
  if (performingMicrotaskCheckpoint_) {
    return;
  }

  TraceSection s("RuntimeScheduler::performMicrotaskCheckpoint");
  jsinspector_modern::tracing::EventLoopReporter performanceReporter(
      jsinspector_modern::tracing::EventLoopPhase::Microtasks);

  performingMicrotaskCheckpoint_ = true;
  OnScopeExit restoreFlag([&]() { performingMicrotaskCheckpoint_ = false; });

  uint8_t retries = 0;
  // A heuristic number to guard infinite or absurd numbers of retries.
  const static unsigned int kRetriesBound = 255;

  while (retries < kRetriesBound) {
    try {
      // The default behavior of \c drainMicrotasks is unbounded execution.
      // We may want to make it bounded in the future.
      if (runtime.drainMicrotasks()) {
        break;
      }
    } catch (jsi::JSError& error) {
      handleTaskError(runtime, error);
    } catch (std::exception& ex) {
      jsi::JSError error(
          runtime, std::string("Non-JS exception: ") + ex.what());
      handleTaskError(runtime, error);
    }
    retries++;
  }

  if (retries == kRetriesBound) {
    throw std::runtime_error("Hits microtasks retries bound.");
  }
}

void RuntimeScheduler_Modern::reportLongTasks(
    const Task& /*task*/,
    HighResTimeStamp startTime,
    HighResTimeStamp endTime) {
  auto reporter = performanceEntryReporter_;
  if (reporter == nullptr) {
    return;
  }

#if RN_JANK_REPORTER
  // Loom (debug): DevTools' long-task entries use the jank budget so
  // the performance panel and the console report agree.
  const auto longTaskThreshold = HighResDuration::fromMilliseconds(
      jank::Reporter::instance().enabled()
          ? jank::Reporter::instance().budgetMs()
          : 50);
  if (longestPeriodWithoutYieldingOpportunity_ >= longTaskThreshold ||
      (endTime - startTime) >= longTaskThreshold) {
#else
  if (longestPeriodWithoutYieldingOpportunity_ >=
      LONG_TASK_DURATION_THRESHOLD) {
#endif
    auto duration = endTime - startTime;
    reporter->reportLongTask(startTime, duration);
  }
}

void RuntimeScheduler_Modern::handleTaskError(
    jsi::Runtime& runtime,
    jsi::JSError& error) {
  clearQueues();

  onTaskError_(runtime, error);
}

void RuntimeScheduler_Modern::clearQueues() {
  // Destroyed after the lock is released: task destructors may schedule work.
  decltype(taskQueue_) drainedTasks;
  {
    std::unique_lock lock(schedulingMutex_);
    drainedTasks.swap(taskQueue_);
    isEventLoopScheduled_ = false;
    animationFrameTasks_.clear();
  }

  pendingRenderingUpdates_ = {};
  surfaceIdsWithPendingRenderingUpdates_.clear();
}

void RuntimeScheduler_Modern::markYieldingOpportunity(
    HighResTimeStamp currentTime) {
  auto currentPeriod = currentTime - lastYieldingOpportunity_;
  if (currentPeriod > longestPeriodWithoutYieldingOpportunity_) {
    longestPeriodWithoutYieldingOpportunity_ = currentPeriod;
  }
  lastYieldingOpportunity_ = currentTime;
}

} // namespace facebook::react
