/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <react/renderer/runtimescheduler/LoomSettings.h>

namespace facebook::react {
namespace {

using Id = LoomSettingId;
using Result = LoomSettings::WriteResult;

class LoomSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    LoomSettings::resetForTesting();
  }
  void TearDown() override {
    LoomSettings::resetForTesting();
  }
};

TEST_F(LoomSettingsTest, UnconfiguredDefaults) {
  EXPECT_TRUE(LoomSettings::enabled(Id::AsyncText));
  EXPECT_TRUE(LoomSettings::enabled(Id::AsyncTextResize));
  EXPECT_FALSE(LoomSettings::enabled(Id::EarlyCreate));
  EXPECT_FALSE(LoomSettings::enabled(Id::ParallelText));
  EXPECT_TRUE(LoomSettings::enabled(Id::JankReporter));
  EXPECT_EQ(LoomSettings::get(Id::JankBudgetMs), 50);
  EXPECT_FALSE(LoomSettings::enabled(Id::ProfileWorker));
}

TEST_F(LoomSettingsTest, PlatformDefaults) {
  LoomSettings::configurePlatform(kLoomPlatformIOS);
  EXPECT_TRUE(LoomSettings::enabled(Id::EarlyCreate));
  EXPECT_TRUE(LoomSettings::enabled(Id::ParallelText));
  LoomSettings::configurePlatform(kLoomPlatformAndroid);
  EXPECT_TRUE(LoomSettings::enabled(Id::EarlyCreate));
  EXPECT_FALSE(LoomSettings::enabled(Id::ParallelText));
  LoomSettings::configurePlatform(kLoomPlatformDesktop);
  EXPECT_TRUE(LoomSettings::enabled(Id::EarlyCreate));
  EXPECT_TRUE(LoomSettings::enabled(Id::ParallelText));
  EXPECT_EQ(LoomSettings::write(Id::ParallelText, 0), Result::Ok);
  EXPECT_FALSE(LoomSettings::enabled(Id::ParallelText));
  // iOS text-drawing switches are not the desktop's.
  EXPECT_EQ(LoomSettings::write(Id::AsyncText, 0), Result::NotApplicable);
}

TEST_F(LoomSettingsTest, WriteThenRead) {
  EXPECT_EQ(LoomSettings::write(Id::AsyncText, 0), Result::Ok);
  EXPECT_FALSE(LoomSettings::enabled(Id::AsyncText));
  EXPECT_EQ(LoomSettings::write(Id::JankBudgetMs, 30), Result::Ok);
  EXPECT_EQ(LoomSettings::get(Id::JankBudgetMs), 30);
  EXPECT_EQ(LoomSettings::write(Id::JankBudgetMs, -5), Result::Ok);
  EXPECT_EQ(LoomSettings::get(Id::JankBudgetMs), 1); // clamped to the minimum
  EXPECT_EQ(LoomSettings::write(Id::ParallelText, 7), Result::Ok);
  EXPECT_EQ(LoomSettings::get(Id::ParallelText), 1); // booleans normalise
}

TEST_F(LoomSettingsTest, ReadOnlyRejectsWrites) {
  LoomSettings::setBuildInfo(false, true);
  EXPECT_EQ(LoomSettings::write(Id::JsOnMainThread, 1), Result::ReadOnly);
  EXPECT_EQ(LoomSettings::write(Id::Worker, 0), Result::ReadOnly);
  EXPECT_FALSE(LoomSettings::enabled(Id::JsOnMainThread));
  EXPECT_TRUE(LoomSettings::enabled(Id::Worker));
}

TEST_F(LoomSettingsTest, OtherPlatformWriteIsNoOp) {
  LoomSettings::configurePlatform(kLoomPlatformAndroid);
  EXPECT_EQ(LoomSettings::write(Id::AsyncText, 0), Result::NotApplicable);
  EXPECT_TRUE(LoomSettings::enabled(Id::AsyncText));
  EXPECT_EQ(LoomSettings::valueForJS(Id::AsyncText), 1);
  LoomSettings::set(Id::AsyncText, 0); // hosts may still set it
  EXPECT_EQ(LoomSettings::valueForJS(Id::AsyncText), 1); // JS sees the default
}

TEST_F(LoomSettingsTest, FindByName) {
  EXPECT_EQ(LoomSettings::find("jankBudgetMs"), Id::JankBudgetMs);
  EXPECT_EQ(LoomSettings::find("nope"), Id::Count);
}

TEST_F(LoomSettingsTest, ListenerSeesWrites) {
  static int seenValue = 0;
  LoomSettings::setChangeListener([](Id, int value) { seenValue = value; });
  LoomSettings::write(Id::JankBudgetMs, 42);
  EXPECT_EQ(seenValue, 42);
}

} // namespace
} // namespace facebook::react
