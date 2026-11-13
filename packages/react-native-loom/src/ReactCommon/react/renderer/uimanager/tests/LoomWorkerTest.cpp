/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */
#include <gtest/gtest.h>
#include <react/renderer/uimanager/LoomWorker.h>
#include "LoomTestTraits.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <stdexcept>

namespace facebook::react {

TEST(LoomWorkerTest, TransfersPacketAndResultInFIFOOrder) {
  LoomWorker worker([] {});
  const auto ui = std::this_thread::get_id();
  std::vector<int> mounted;
  auto effect = worker.defer(std::function<void(int)>([&](int index) {
    EXPECT_EQ(std::this_thread::get_id(), ui);
    mounted.push_back(index);
  }));
  struct Packet {
    int value;
  };
  for (int index = 0; index != 100; ++index) {
    auto packet = std::make_unique<const Packet>(Packet{index});
    const auto* address = packet.get();
    auto job = std::make_unique<LoomWorker::Job>();
    job->compute = [packet = std::move(packet), address, effect, ui] {
      EXPECT_NE(std::this_thread::get_id(), ui);
      EXPECT_EQ(packet.get(), address);
      effect(packet->value);
    };
    worker.submit(std::move(job));
  }
  worker.synchronize();
  ASSERT_EQ(mounted.size(), 100);
  for (int index = 0; index != 100; ++index)
    EXPECT_EQ(mounted[index], index);
}

TEST(LoomWorkerTest, DoesNotAdvanceUntilMountIsAcknowledged) {
  std::promise<void> prepared;
  auto ready = prepared.get_future();
  LoomWorker worker([&] { prepared.set_value(); });
  std::atomic<int> commits{0};
  auto first = std::make_unique<LoomWorker::Job>();
  first->compute = [&] { ++commits; };
  worker.submit(std::move(first));
  ready.wait();
  auto second = std::make_unique<LoomWorker::Job>();
  second->compute = [&] { ++commits; };
  worker.submit(std::move(second));
  EXPECT_EQ(commits.load(), 1);
  // Stop rather than acknowledge: queued computation must not execute.
  worker.shutdown();
  EXPECT_EQ(commits.load(), 1);
}

TEST(LoomWorkerTest, DiscardsInflightStaleGenerationAndQueuedWork) {
  LoomWorker worker([] {});
  auto active = std::make_shared<std::atomic<bool>>(true);
  std::promise<void> started;
  auto ready = started.get_future();
  std::promise<void> resume;
  auto continuation = resume.get_future();
  int mounted = 0;
  auto effect = worker.defer(std::function<void()>([&] { ++mounted; }));
  auto job = std::make_unique<LoomWorker::Job>();
  job->active = [active] { return active->load(); };
  job->compute = [&] {
    started.set_value();
    continuation.wait();
    effect();
  };
  worker.submit(std::move(job));
  ready.wait();
  active->store(false);
  resume.set_value();
  worker.synchronize();
  EXPECT_EQ(mounted, 0);
  auto stale = std::make_unique<LoomWorker::Job>();
  stale->active = [active] { return active->load(); };
  stale->compute = [] { FAIL() << "stale queued generation computed"; };
  worker.submit(std::move(stale));
  worker.synchronize();
}

TEST(
    LoomWorkerTest,
    SynchronousCommitBorrowsSameOwnerAfterPriorMount) {
  LoomWorker worker([] {});
  auto ui = std::this_thread::get_id();
  std::vector<int> steps;
  auto effect = worker.defer(
      std::function<void(int)>([&](int step) { steps.push_back(step); }));
  auto prior = std::make_unique<LoomWorker::Job>();
  prior->compute = [effect] { effect(1); };
  worker.submit(std::move(prior));
  auto sync = std::make_unique<LoomWorker::Job>();
  sync->synchronous = true;
  sync->compute = [&] {
    EXPECT_EQ(std::this_thread::get_id(), ui);
    EXPECT_EQ(steps, (std::vector<int>{1}));
    effect(2);
  };
  worker.submit(std::move(sync));
  worker.synchronize();
  EXPECT_EQ(steps, (std::vector<int>{1, 2}));
}

TEST(LoomWorkerTest, ReentrantUIReadsAndSubmissionsCannotDeadlock) {
  LoomWorker worker([] {});
  std::vector<int> steps;
  auto effect = worker.defer(std::function<void()>([&] {
    steps.push_back(1);
    worker.synchronize();
    auto nested = std::make_unique<LoomWorker::Job>();
    nested->synchronous = true;
    nested->compute = [&] { steps.push_back(2); };
    worker.submit(std::move(nested));
    worker.synchronize();
    EXPECT_EQ(steps, (std::vector<int>{1}));
  }));
  auto job = std::make_unique<LoomWorker::Job>();
  job->compute = [effect] { effect(); };
  worker.submit(std::move(job));
  worker.synchronize();
  EXPECT_EQ(steps, (std::vector<int>{1, 2}));
}

TEST(LoomWorkerTest, MountFailureInvalidatesBeforeNextJobRuns) {
  LoomWorker worker([] {});
  std::atomic<bool> active{true};
  bool computedNext = false;
  auto effect = worker.defer(
      std::function<void()>([] { throw std::runtime_error("partial mount"); }));
  auto job = std::make_unique<LoomWorker::Job>();
  job->active = [&] { return active.load(); };
  job->failed = [&](std::exception_ptr) { active = false; };
  job->compute = [effect] { effect(); };
  worker.submit(std::move(job));
  auto next = std::make_unique<LoomWorker::Job>();
  next->active = [&] { return active.load(); };
  next->compute = [&] { computedNext = true; };
  worker.submit(std::move(next));
  // A failed application stops the surface and consumes the error: `failed`
  // invalidates the token, and rethrowing here would escape the platform
  // dispatch block that called `synchronize()`. The observable contract is the
  // invalidation plus the fact that the queued job never computes, asserted
  // below, not an exception.
  EXPECT_NO_THROW(worker.synchronize());
  EXPECT_FALSE(active.load());
  EXPECT_FALSE(computedNext);
}

TEST(LoomWorkerTest, IdleSynchronousJobRunsOnTheCallingThread) {
  // A forced-sync commit computes on the UI thread either way. On an idle worker
  // it must do so directly, without the worker publishing an empty result and
  // waking this thread to compute it: that round trip is the whole cost P4
  // removes.
  LoomWorker worker([] {});
  const auto caller = std::this_thread::get_id();
  auto computedOn = std::thread::id{};
  bool applied = false;
  auto job = std::make_unique<LoomWorker::Job>();
  job->synchronous = true;
  job->applied = [&] { applied = true; };
  job->compute = [&] { computedOn = std::this_thread::get_id(); };

  EXPECT_TRUE(worker.runIfIdle(job));
  EXPECT_EQ(job, nullptr);
  EXPECT_EQ(computedOn, caller);
  EXPECT_TRUE(applied);
}

TEST(LoomWorkerTest, SynchronousJobQueuesBehindOwnedWork) {
  // Ordering is the worker's job: while it still owns a job, a synchronous job
  // must be refused so the caller queues it behind that work instead of running
  // it out of order.
  std::promise<void> started;
  auto running = started.get_future();
  std::promise<void> release;
  auto released = release.get_future();
  LoomWorker worker([] {});
  auto blocking = std::make_unique<LoomWorker::Job>();
  blocking->compute = [&] {
    started.set_value();
    released.wait();
  };
  worker.submit(std::move(blocking));
  running.wait();

  auto job = std::make_unique<LoomWorker::Job>();
  job->synchronous = true;
  job->compute = [] {};
  EXPECT_FALSE(worker.runIfIdle(job));
  // A refused job stays with the caller, which queues it behind the owned work.
  ASSERT_NE(job, nullptr);
  worker.submit(std::move(job));

  release.set_value();
  worker.synchronize();
}

TEST(LoomWorkerTest, PublicationBookkeepingIsOrderedAndRunsOnUI) {
  // `submitOrderedUIEffect` submits a job whose compute does nothing and whose
  // `applied` callback carries the work. That is only equivalent to blocking in
  // `synchronize()` if the callback runs on the UI thread *after* the effects of
  // every job submitted before it, which this pins down.
  LoomWorker worker([] {});
  const auto ui = std::this_thread::get_id();
  std::vector<std::string> order;

  auto effect = worker.defer(
      std::function<void()>([&] { order.push_back("commit-effect"); }));
  auto commit = std::make_unique<LoomWorker::Job>();
  commit->compute = [effect] { effect(); };
  worker.submit(std::move(commit));

  auto ordered = std::make_unique<LoomWorker::Job>();
  ordered->compute = [] {};
  ordered->applied = [&] {
    EXPECT_EQ(std::this_thread::get_id(), ui);
    order.push_back("ordered-effect");
  };
  worker.submit(std::move(ordered));

  worker.synchronize();
  EXPECT_EQ(order, (std::vector<std::string>{"commit-effect", "ordered-effect"}));
}

TEST(LoomWorkerTest, ShutdownJoinsAndNeverAppliesPendingEffects) {
  int mounted = 0;
  std::weak_ptr<int> lifetime;
  {
    LoomWorker worker([] {});
    auto packet = std::make_shared<int>(42);
    lifetime = packet;
    auto effect = worker.defer(std::function<void()>([&] { ++mounted; }));
    auto job = std::make_unique<LoomWorker::Job>();
    job->compute = [packet = std::move(packet), effect] { effect(); };
    worker.submit(std::move(job));
  }
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(mounted, 0);
}
namespace {

/**
 * A stand-in for the platform UI thread when JavaScript runs elsewhere: the
 * test body plays the JS thread, this thread owns result application and is the
 * only one the worker considers "UI".
 */
class FakeUIThread {
 public:
  FakeUIThread() : thread_([this] { loop(); }) {
    uiId_ = thread_.get_id();
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return started_; });
  }
  ~FakeUIThread() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    thread_.join();
  }
  bool isUIThread() const {
    return std::this_thread::get_id() == uiId_;
  }
  void post(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    ready_.notify_all();
  }

 private:
  void loop() {
    {
      std::lock_guard lock(mutex_);
      started_ = true;
    }
    ready_.notify_all();
    std::unique_lock lock(mutex_);
    for (;;) {
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty())
        return;
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  bool started_{false};
  bool stopping_{false};
  std::thread::id uiId_;
  std::thread thread_;
};

} // namespace

TEST(LoomWorkerTest, OffUIBarrierWaitsForTheUIThreadToApply) {
  FakeUIThread ui;
  LoomWorker* workerPointer = nullptr;
  std::atomic<int> applied{0};
  std::atomic<int> appliedOffUI{0};
  LoomWorker worker(
      [&] { ui.post([&] { workerPointer->applyReady(); }); },
      [&] { return ui.isUIThread(); });
  workerPointer = &worker;
  for (int index = 0; index != 20; ++index) {
    auto job = std::make_unique<LoomWorker::Job>();
    job->compute = [] {};
    job->applied = [&] {
      if (!ui.isUIThread())
        ++appliedOffUI;
      ++applied;
    };
    worker.submit(std::move(job));
  }
  // This thread is "JS": synchronize() must wait instead of applying itself.
  worker.synchronize();
  EXPECT_EQ(applied.load(), 20);
  EXPECT_EQ(appliedOffUI.load(), 0);
}

TEST(LoomWorkerTest, OffUISynchronousJobComputesOnTheUIThread) {
  FakeUIThread ui;
  LoomWorker* workerPointer = nullptr;
  LoomWorker worker(
      [&] { ui.post([&] { workerPointer->applyReady(); }); },
      [&] { return ui.isUIThread(); });
  workerPointer = &worker;
  std::atomic<bool> computedOnUI{false};
  std::atomic<bool> appliedOnUI{false};
  auto job = std::make_unique<LoomWorker::Job>();
  job->synchronous = true;
  job->compute = [&] { computedOnUI = ui.isUIThread(); };
  job->applied = [&] { appliedOnUI = ui.isUIThread(); };
  // The worker is idle, but this is not the UI thread, so it must not borrow it.
  EXPECT_FALSE(worker.runIfIdle(job));
  ASSERT_NE(job, nullptr);
  worker.submit(std::move(job));
  worker.awaitApplied();
  EXPECT_TRUE(computedOnUI.load());
  EXPECT_TRUE(appliedOnUI.load());
}

TEST(LoomWorkerTest, RunIfIdleStillBorrowsTheUIThread) {
  FakeUIThread ui;
  LoomWorker worker([] {}, [&] { return ui.isUIThread(); });
  std::promise<bool> ranOnUI;
  ui.post([&] {
    auto job = std::make_unique<LoomWorker::Job>();
    job->synchronous = true;
    bool computed = false;
    job->compute = [&] { computed = true; };
    const bool taken = worker.runIfIdle(job);
    ranOnUI.set_value(taken && computed);
  });
  EXPECT_TRUE(ranOnUI.get_future().get());
}

TEST(LoomWorkerTest, ApplyReadyIgnoresNonUICallers) {
  FakeUIThread ui;
  LoomWorker worker([] {}, [&] { return ui.isUIThread(); });
  std::atomic<int> applied{0};
  auto job = std::make_unique<LoomWorker::Job>();
  job->compute = [] {};
  job->applied = [&] { ++applied; };
  worker.submit(std::move(job));
  // Give the worker time to publish its result, then try to apply it here.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  worker.applyReady();
  EXPECT_EQ(applied.load(), 0);
  std::promise<void> done;
  ui.post([&] {
    worker.applyReady();
    done.set_value();
  });
  done.get_future().wait();
  EXPECT_EQ(applied.load(), 1);
}

} // namespace facebook::react
