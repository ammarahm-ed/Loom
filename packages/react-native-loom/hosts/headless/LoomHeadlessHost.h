/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <folly/dynamic.h>
#include <react/renderer/loom/host/LoomHost.h>
#include <react/renderer/loom/text/LoomTextServices.h>

namespace facebook::react {

/** A batch the core handed to `applyBatch`, with the surface it was for. */
struct LoomHeadlessBatch {
  SurfaceId surfaceId{0};
  LoomMountBatch batch;
};

struct LoomHeadlessEvent {
  SurfaceId surfaceId{0};
  Tag tag{0};
  std::string name;
  folly::dynamic payload;
};

struct LoomHeadlessCommand {
  SurfaceId surfaceId{0};
  Tag tag{0};
  std::string name;
  folly::dynamic args;
};

struct LoomHeadlessViewProps {
  Tag tag{0};
  folly::dynamic props;
};

/**
 * Text metrics for the fake text engine: every character is `characterWidth`
 * wide and every line `lineHeight` tall, in DIP.
 */
struct LoomHeadlessTextMetrics {
  float characterWidth{8};
  float lineHeight{16};
};

/**
 * Deterministic text measurement with no font engine: a string wraps at
 * character granularity inside the maximum width, up to
 * `maximumNumberOfLines`, and the result is clamped into the constraints.
 * Measurement is pure, so it is safe on any thread.
 */
class LoomHeadlessTextServices final : public LoomTextServices {
 public:
  explicit LoomHeadlessTextServices(LoomHeadlessTextMetrics metrics = {})
      : metrics_(metrics) {}

  std::optional<Size> measureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;

  bool canMeasureOnWorker() override {
    return true;
  }

 private:
  LoomHeadlessTextMetrics metrics_;
};

/**
 * A `LoomHost` with no views and no platform code, for tests and as the
 * starting point for a new platform. It uses the core's generic defaults
 * (`platform/cxx`): Fabric component names, full props on update, no prepared
 * text layout, no parallel measurement, logging to stderr.
 *
 *   - Output is recorded. `applyBatch`, `failSurface`, `dispatchEvent`,
 *     `dispatchCommand`, `updateViewProps` and `updateTextState` append to
 *     lists read with `takeBatches()` and friends. Everything else is a no-op.
 *   - Text is measured by `LoomHeadlessTextServices`.
 *   - Work posted through `uiExecutor()` runs inline on the calling thread
 *     (`Executor::Inline`, the default), or is queued until `drainUIQueue()`
 *     on the UI thread (`Executor::Queued`). Use `Queued` when the core runs
 *     its worker, so results are applied on the thread that owns this host,
 *     which is the thread that constructed it.
 */
class LoomHeadlessHost final : public LoomHost {
 public:
  enum class Executor { Inline, Queued };

  explicit LoomHeadlessHost(
      Executor executor = Executor::Inline,
      LoomHeadlessTextMetrics metrics = {});

  /** Registers a new host in `container` (`LoomHost::registerIn`) and returns it. */
  static std::shared_ptr<LoomHeadlessHost> registerIn(
      const ContextContainer& container,
      Executor executor = Executor::Inline,
      LoomHeadlessTextMetrics metrics = {});

  // LoomHost
  LoomPlatformTraits traits() const override;
  std::function<void(std::function<void()>)> uiExecutor() override;
  void applyBatch(SurfaceId, LoomMountBatch) override;
  void failSurface(SurfaceId, const std::string& reason) override;
  void updateViewProps(Tag, const folly::dynamic& props) override;
  void dispatchEvent(
      SurfaceId,
      Tag,
      const std::string& eventName,
      folly::dynamic payload,
      RawEvent::Category,
      bool unique) override;
  void updateTextState(const LoomTextStateUpdate&) override;
  void dispatchCommand(
      SurfaceId,
      Tag,
      const std::string& commandName,
      const folly::dynamic& args) override;
  std::pair<float, float> getScrollOffset(SurfaceId, Tag) override;
  LoomTextServices* textServices() override {
    return &text_;
  }

  // Observation. Each `take` returns what was recorded since the last call.
  std::vector<LoomHeadlessBatch> takeBatches();
  std::vector<std::pair<SurfaceId, std::string>> takeFailures();
  std::vector<LoomHeadlessEvent> takeEvents();
  std::vector<LoomHeadlessCommand> takeCommands();
  std::vector<LoomHeadlessViewProps> takeViewProps();
  std::vector<LoomTextStateUpdate> takeTextStates();

  /** The scroll position `getScrollOffset` reports for a view. */
  void setScrollOffset(Tag, float x, float y);

  /** `Executor::Queued` only: runs queued work, in order, on the calling thread. */
  void drainUIQueue();

 private:
  const Executor executor_;
  LoomHeadlessTextServices text_;

  std::mutex mutex_;
  std::vector<LoomHeadlessBatch> batches_;
  std::vector<std::pair<SurfaceId, std::string>> failures_;
  std::vector<LoomHeadlessEvent> events_;
  std::vector<LoomHeadlessCommand> commands_;
  std::vector<LoomHeadlessViewProps> viewProps_;
  std::vector<LoomTextStateUpdate> textStates_;
  std::unordered_map<Tag, std::pair<float, float>> scrollOffsets_;

  std::mutex queueMutex_;
  std::deque<std::function<void()>> queue_;
};

} // namespace facebook::react
