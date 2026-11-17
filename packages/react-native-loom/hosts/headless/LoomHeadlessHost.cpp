/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoomHeadlessHost.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace facebook::react {

namespace {

/** The thread that constructed the most recent host: the thread that owns its "views". */
std::atomic<std::thread::id>& uiThreadId() {
  static std::atomic<std::thread::id> id{};
  return id;
}

bool isHeadlessUIThread() {
  return uiThreadId().load() == std::this_thread::get_id();
}

} // namespace

std::optional<Size> LoomHeadlessTextServices::measureText(
    SurfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  const auto characters =
      static_cast<float>(attributedString.getString().size());
  float width = characters * metrics_.characterWidth;
  float lines = characters > 0 ? 1.0f : 0.0f;
  if (std::isfinite(maximumSize.width) && width > maximumSize.width) {
    const auto perLine = std::max(
        1.0f, std::floor(maximumSize.width / metrics_.characterWidth));
    lines = std::ceil(characters / perLine);
    width = perLine * metrics_.characterWidth;
  }
  if (paragraphAttributes.maximumNumberOfLines > 0) {
    lines = std::min(
        lines, static_cast<float>(paragraphAttributes.maximumNumberOfLines));
  }
  float height = lines * metrics_.lineHeight;
  width = std::clamp(width, minimumSize.width, std::max(minimumSize.width, maximumSize.width));
  height = std::clamp(
      height, minimumSize.height, std::max(minimumSize.height, maximumSize.height));
  return Size{width, height};
}

LoomHeadlessHost::LoomHeadlessHost(
    Executor executor,
    LoomHeadlessTextMetrics metrics)
    : executor_(executor), text_(metrics) {
  uiThreadId().store(std::this_thread::get_id());
}

std::shared_ptr<LoomHeadlessHost> LoomHeadlessHost::registerIn(
    const ContextContainer& container,
    Executor executor,
    LoomHeadlessTextMetrics metrics) {
  auto host = std::make_shared<LoomHeadlessHost>(executor, metrics);
  LoomHost::registerIn(container, host);
  return host;
}

LoomPlatformTraits LoomHeadlessHost::traits() const {
  LoomPlatformTraits traits;
  traits.isUIThread = isHeadlessUIThread;
  return traits;
}

std::function<void(std::function<void()>)> LoomHeadlessHost::uiExecutor() {
  if (executor_ == Executor::Inline) {
    return [this](std::function<void()> work) {
      ++uiPosts_;
      work();
    };
  }
  return [this](std::function<void()> work) {
    ++uiPosts_;
    std::lock_guard lock(queueMutex_);
    queue_.push_back(std::move(work));
  };
}

void LoomHeadlessHost::drainUIQueue() {
  for (;;) {
    std::function<void()> work;
    {
      std::lock_guard lock(queueMutex_);
      if (queue_.empty()) {
        return;
      }
      work = std::move(queue_.front());
      queue_.pop_front();
    }
    work();
  }
}

void LoomHeadlessHost::applyBatch(SurfaceId surfaceId, LoomMountBatch batch) {
  std::lock_guard lock(mutex_);
  batches_.push_back({surfaceId, std::move(batch)});
}

void LoomHeadlessHost::failSurface(SurfaceId surfaceId, const std::string& reason) {
  std::lock_guard lock(mutex_);
  failures_.emplace_back(surfaceId, reason);
}

void LoomHeadlessHost::updateViewProps(Tag tag, const folly::dynamic& props) {
  std::lock_guard lock(mutex_);
  viewProps_.push_back({tag, props});
}

void LoomHeadlessHost::dispatchEvent(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& eventName,
    folly::dynamic payload,
    RawEvent::Category,
    bool) {
  std::lock_guard lock(mutex_);
  events_.push_back({surfaceId, tag, eventName, std::move(payload)});
}

void LoomHeadlessHost::updateTextState(const LoomTextStateUpdate& update) {
  std::lock_guard lock(mutex_);
  textStates_.push_back(update);
}

void LoomHeadlessHost::dispatchCommand(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& commandName,
    const folly::dynamic& args) {
  std::lock_guard lock(mutex_);
  commands_.push_back({surfaceId, tag, commandName, args});
}

std::pair<float, float> LoomHeadlessHost::getScrollOffset(SurfaceId, Tag tag) {
  std::lock_guard lock(mutex_);
  const auto found = scrollOffsets_.find(tag);
  return found == scrollOffsets_.end() ? std::pair<float, float>{0, 0}
                                       : found->second;
}

void LoomHeadlessHost::setScrollOffset(Tag tag, float x, float y) {
  std::lock_guard lock(mutex_);
  scrollOffsets_[tag] = {x, y};
}

#define LOOM_HEADLESS_TAKE(Name, Type, Member)  \
  std::vector<Type> LoomHeadlessHost::Name() {  \
    std::lock_guard lock(mutex_);               \
    return std::exchange(Member, {});           \
  }

LOOM_HEADLESS_TAKE(takeBatches, LoomHeadlessBatch, batches_)
LOOM_HEADLESS_TAKE(takeEvents, LoomHeadlessEvent, events_)
LOOM_HEADLESS_TAKE(takeCommands, LoomHeadlessCommand, commands_)
LOOM_HEADLESS_TAKE(takeViewProps, LoomHeadlessViewProps, viewProps_)
LOOM_HEADLESS_TAKE(takeTextStates, LoomTextStateUpdate, textStates_)

std::vector<std::pair<SurfaceId, std::string>> LoomHeadlessHost::takeFailures() {
  std::lock_guard lock(mutex_);
  return std::exchange(failures_, {});
}

#undef LOOM_HEADLESS_TAKE

} // namespace facebook::react
