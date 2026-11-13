/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <react/renderer/uimanager/LoomRenderer.h>

#include "../LoomHeadlessHost.h"

namespace facebook::react {
namespace {

AttributedString textOf(const std::string& text) {
  AttributedString string;
  AttributedString::Fragment fragment;
  fragment.string = text;
  string.appendFragment(std::move(fragment));
  return string;
}

TEST(LoomHeadlessHostTest, TextWrapsAtCharacterGranularity) {
  LoomHeadlessTextServices text({.characterWidth = 10, .lineHeight = 20});
  const auto size = text.measureText(
      1, textOf("abcdefghij"), ParagraphAttributes{}, Size{0, 0}, Size{40, 1000});
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(size->width, 40);
  EXPECT_EQ(size->height, 60);
}

TEST(LoomHeadlessHostTest, MaximumNumberOfLinesCapsHeight) {
  LoomHeadlessTextServices text({.characterWidth = 10, .lineHeight = 20});
  ParagraphAttributes attributes;
  attributes.maximumNumberOfLines = 2;
  const auto size = text.measureText(
      1, textOf("abcdefghij"), attributes, Size{0, 0}, Size{40, 1000});
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(size->height, 40);
}

TEST(LoomHeadlessHostTest, QueuedExecutorRunsOnDrain) {
  LoomHeadlessHost host(LoomHeadlessHost::Executor::Queued);
  int ran = 0;
  host.uiExecutor()([&ran] { ++ran; });
  EXPECT_EQ(ran, 0);
  host.drainUIQueue();
  EXPECT_EQ(ran, 1);
}

TEST(LoomHeadlessHostTest, InlineExecutorRunsImmediately) {
  LoomHeadlessHost host;
  int ran = 0;
  host.uiExecutor()([&ran] { ++ran; });
  EXPECT_EQ(ran, 1);
}

// The core drives the host through `LoomRenderer::NativeOperations`; this is
// what `LoomBinding` does with a registered host, without a UIManager.
TEST(LoomHeadlessHostTest, RendererBatchReachesTheHost) {
  LoomHeadlessHost host;
  LoomRenderer::NativeOperations operations;
  operations.applyBatch = [&host](SurfaceId surface, LoomMountBatch batch) {
    host.applyBatch(surface, std::move(batch));
  };
  operations.registerEventTarget = [](SurfaceId, Tag, InstanceHandle::Shared) {};
  operations.unregisterEventTarget = [](SurfaceId, Tag) {};

  LoomRenderer renderer(1, std::move(operations));
  renderer.setConstraints(200, 200, 200, 200);
  renderer.createNode(1, "Root", folly::dynamic::object());
  renderer.createNode(
      2,
      "View",
      folly::dynamic::object("width", 100.0)("height", 50.0)(
          "backgroundColor", "red"));
  renderer.setChildren(1, {2});
  renderer.completeRoot(1, {2});

  auto batches = host.takeBatches();
  ASSERT_FALSE(batches.empty());
  bool created = false;
  bool laidOut = false;
  for (const auto& recorded : batches) {
    for (const auto& operation : recorded.batch.operations) {
      if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
          create != nullptr && create->tag == 2) {
        created = true;
        EXPECT_EQ(create->componentName, "View");
      }
      if (const auto* layout = std::get_if<DirectLayoutOperation>(&operation);
          layout != nullptr && layout->tag == 2) {
        laidOut = true;
        EXPECT_EQ(layout->width, 100);
        EXPECT_EQ(layout->height, 50);
      }
    }
  }
  EXPECT_TRUE(created);
  EXPECT_TRUE(laidOut);
}

} // namespace
} // namespace facebook::react
