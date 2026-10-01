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
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/renderer/consistency/ScopedShadowTreeRevisionLock.h>
#include <react/timing/primitives.h>
#include <react/utils/OnScopeExit.h>

#include <atomic>
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

#if RN_UI_THREAD
/*
 * React Native X: the runtime executing on this thread right now, if any.
 *
 * Upstream set the equivalent flag only inside `executeNowOnTheSameThread`'s own
 * callback, because the runtime always ran on a dedicated JS queue thread and a
 * synchronous request therefore always arrived from a *different* thread. React Native X
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
 * React Native X: the runtime this thread last executed, remembered beyond the
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
#endif // RN_UI_THREAD

} // namespace

#pragma mark - Public

RuntimeScheduler_Modern::RuntimeScheduler_Modern(
    RuntimeExecutor runtimeExecutor,
    std::function<HighResTimeStamp()> now,
    RuntimeSchedulerTaskErrorHandler onTaskError)
    : runtimeExecutor_(std::move(runtimeExecutor)),
      now_(std::move(now)),
      onTaskError_(std::move(onTaskError)) {}

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

  return syncTaskRequests_ > 0 ||
      (!taskQueue_.empty() && taskQueue_.top().get() != currentTask_);
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

#if RN_UI_THREAD
  if (currentRuntimeOnThread != nullptr) {
    // Protecting against re-entry into `executeNowOnTheSameThread` from within
    // `executeNowOnTheSameThread`. Without accounting for re-rentry, a deadlock
    // will occur when trying to gain access to the runtime.
    //
    // React Native X: on the single-thread execution model this branch is also taken by
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
      return executeTask(*runtime, task, true);
    }
  }
#else
  static thread_local jsi::Runtime* runtimePtr = nullptr;

  if (runtimePtr != nullptr) {
    // Protecting against re-entry into `executeNowOnTheSameThread` from within
    // `executeNowOnTheSameThread`. Without accounting for re-rentry, a deadlock
    // will occur when trying to gain access to the runtime.
    return executeTask(*runtimePtr, task, true);
  }
#endif // RN_UI_THREAD

  syncTaskRequests_++;
  executeSynchronouslyOnSameThread_CAN_DEADLOCK(
      runtimeExecutor_, [&](jsi::Runtime& runtime) mutable {
        TraceSection s2("RuntimeScheduler::executeNowOnTheSameThread callback");

        syncTaskRequests_--;
#if RN_UI_THREAD
        CurrentRuntimeScope currentRuntimeScope(runtime);
        runEventLoopTick(runtime, task);
#else
        runtimePtr = &runtime;
        runEventLoopTick(runtime, task);
        runtimePtr = nullptr;
#endif // RN_UI_THREAD
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

    taskQueue_.push(std::move(task));
  }

  if (shouldScheduleEventLoop) {
    scheduleEventLoop();
  }
}

void RuntimeScheduler_Modern::scheduleEventLoop() {
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
  auto topPriorityTask = selectTask();
  while (topPriorityTask && syncTaskRequests_ == 0) {
    runEventLoopTick(runtime, *topPriorityTask);
    topPriorityTask = selectTask();
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
    taskQueue_.pop();
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

#if RN_UI_THREAD
  // React Native X: publish the runtime for the duration of the tick so that synchronous
  // access requested by user code running on this thread executes directly,
  // instead of posting to the very queue this thread is already running.
  CurrentRuntimeScope currentRuntimeScope(runtime);
#endif // RN_UI_THREAD

  currentTask_ = &task;
  currentPriority_ = task.priority;

  auto taskStartTime = now_();
  lastYieldingOpportunity_ = taskStartTime;
  longestPeriodWithoutYieldingOpportunity_ = HighResDuration::zero();

  auto didUserCallbackTimeout = task.expirationTime <= taskStartTime;
  executeTask(runtime, task, didUserCallbackTimeout);

  // "Perform a microtask checkpoint" step.
  performMicrotaskCheckpoint(runtime);

  auto taskEndTime = now_();
  markYieldingOpportunity(taskEndTime);
  reportLongTasks(task, taskStartTime, taskEndTime);

  // "Update the rendering" step.
  updateRendering(taskEndTime);

  currentTask_ = nullptr;
}

/**
 * This is partially equivalent to the "Update the rendering" step in the Web
 * event loop. See
 * https://html.spec.whatwg.org/multipage/webappapis.html#update-the-rendering.
 */
void RuntimeScheduler_Modern::updateRendering(HighResTimeStamp taskEndTime) {
  TraceSection s("RuntimeScheduler::updateRendering");

  // This is the integration of the Event Timing API in the Event Loop.
  // See https://w3c.github.io/event-timing/#sec-modifications-HTML
  const auto eventTimingDelegate = eventTimingDelegate_.load();
  if (eventTimingDelegate != nullptr) {
    eventTimingDelegate->dispatchPendingEventTimingEntries(
        taskEndTime, surfaceIdsWithPendingRenderingUpdates_);
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
    jsi::JSError error(runtime, std::string("Non-js exception: ") + ex.what());
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
          runtime, std::string("Non-js exception: ") + ex.what());
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

  if (longestPeriodWithoutYieldingOpportunity_ >=
      LONG_TASK_DURATION_THRESHOLD) {
    auto duration = endTime - startTime;
    reporter->reportLongTask(startTime, duration);
  }
}

void RuntimeScheduler_Modern::handleTaskError(
    jsi::Runtime& runtime,
    jsi::JSError& error) {
  if (ReactNativeFeatureFlags::enableRuntimeSchedulerQueueClearingOnError()) {
    clearQueues();
  }

  onTaskError_(runtime, error);
}

void RuntimeScheduler_Modern::clearQueues() {
  {
    std::unique_lock lock(schedulingMutex_);
    taskQueue_ = {};
    isEventLoopScheduled_ = false;
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
