/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/components/root/RootComponentDescriptor.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/loom/Loom.h>
#include <react/renderer/loom/binding/LoomBinding.h>
#include <react/renderer/uimanager/UIManager.h>

#include "../LoomHeadlessHost.h"

namespace facebook::react {

// Reaches the binding's publication path, which the host API normally drives.
struct LoomBindingTestAccess {
  using Draft = LoomBinding::DraftNode;
  using Patch = LoomBinding::PropsPatch;

  static std::shared_ptr<Draft> draft(
      SurfaceId surfaceId,
      uint64_t generation,
      uint64_t identity,
      Tag tag,
      std::string componentName,
      folly::dynamic props) {
    auto node = std::make_shared<Draft>();
    node->surfaceId = surfaceId;
    node->surfaceGeneration = generation;
    node->identity = identity;
    node->tag = tag;
    node->componentName =
        std::make_shared<const std::string>(std::move(componentName));
    node->initialProps =
        std::make_shared<const folly::dynamic>(std::move(props));
    node->hasInitialProps = true;
    node->roles = LoomPlatformTraits::current().components.classify(
        *node->componentName);
    return node;
  }

  /** The platform's tag-only native-props entry, with the lane writing the view itself. */
  static void recordNativeProps(Tag tag, folly::dynamic props) {
    LoomBinding::recordNativePropsImpl(
        tag, std::move(props), /*knownPaint*/ false, /*viewApplied*/ false);
  }

  static auto tokenFor(LoomBinding& binding, SurfaceId surfaceId) {
    return binding.tokenFor(surfaceId);
  }

  static void publish(
      LoomBinding& binding,
      SurfaceId surfaceId,
      const std::shared_ptr<Draft>& root,
      uint64_t generation) {
    binding.publishWorkerCommit(
        surfaceId,
        {std::make_shared<LoomBinding::NodeRef>(
            surfaceId, root->tag, binding.tokenFor(surfaceId), root)},
        /*forceSync*/ true);
    (void)generation;
  }
};

namespace {

/**
 * The core with a host registered in the context container and no
 * `UIManagerDelegate` at all: the binding must reach every service through
 * the host.
 */
class LoomHeadlessBindingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    contextContainer_ = std::make_shared<ContextContainer>();
    host_ = LoomHeadlessHost::registerIn(*contextContainer_);

    ComponentDescriptorProviderRegistry providerRegistry{};
    auto registry = providerRegistry.createComponentDescriptorRegistry(
        ComponentDescriptorParameters{
            .eventDispatcher = EventDispatcher::Shared{},
            .contextContainer = contextContainer_,
            .flavor = nullptr});
    providerRegistry.add(
        concreteComponentDescriptorProvider<RootComponentDescriptor>());
    providerRegistry.add(
        concreteComponentDescriptorProvider<ViewComponentDescriptor>());

    RuntimeExecutor runtimeExecutor =
        [](std::function<void(jsi::Runtime&)>&&) {};
    uiManager_ =
        std::make_shared<UIManager>(runtimeExecutor, contextContainer_);
    uiManager_->setComponentDescriptorRegistry(std::move(registry));
    // Deliberately no setDelegate().
    binding_ = std::make_shared<LoomBinding>(uiManager_);
  }

  void TearDown() override {
    binding_.reset();
  }

  bool viewGotOpacity(Tag tag, double opacity) {
    host_->drainUIQueue();
    bool written = false;
    for (const auto& update : host_->takeViewProps()) {
      if (update.tag == tag && update.props.isObject() && update.props.count("opacity") != 0) {
        written = written || update.props["opacity"].asDouble() == opacity;
      }
    }
    return written;
  }

  /** Commits one View (tag 2) under surface 1's root and applies it on the (test) UI thread. */
  void mountOneView() {
    using Access = LoomBindingTestAccess;
    LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
    const auto token = Access::tokenFor(*binding_, 1);
    auto child = Access::draft(
        1, token->generation, 11, 2, "View",
        folly::dynamic::object("width", 100.0)("height", 50.0)("opacity", 1.0));
    child->children =
        std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
    Access::publish(*binding_, 1, child, token->generation);
    LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
    host_->drainUIQueue();
    host_->takeBatches();
  }

  std::shared_ptr<ContextContainer> contextContainer_;
  std::shared_ptr<LoomHeadlessHost> host_;
  std::shared_ptr<UIManager> uiManager_;
  std::shared_ptr<LoomBinding> binding_;
};

TEST_F(LoomHeadlessBindingTest, CommitReachesTheRegisteredHost) {
  using Access = LoomBindingTestAccess;
  ASSERT_EQ(uiManager_->getDelegate(), nullptr);

  LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, 1);
  // The root's children are published; the surface root itself is the host's.
  auto child = Access::draft(
      1,
      token->generation,
      11,
      2,
      "View",
      folly::dynamic::object("width", 100.0)("height", 50.0)(
          "backgroundColor", "red"));
  child->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();

  Access::publish(*binding_, 1, child, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  host_->drainUIQueue();

  bool created = false;
  for (const auto& recorded : host_->takeBatches()) {
    EXPECT_EQ(recorded.surfaceId, 1);
    for (const auto& operation : recorded.batch.operations) {
      if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
          create != nullptr && create->tag == 2) {
        created = true;
        EXPECT_EQ(create->componentName, "View");
      }
    }
  }
  EXPECT_TRUE(created);
  EXPECT_TRUE(host_->takeFailures().empty());
}

/** The last display type the host was given for `tag`, or -1. */
int lastDisplayType(LoomHeadlessHost& host, Tag tag) {
  host.drainUIQueue();
  int displayType = -1;
  for (const auto& recorded : host.takeBatches()) {
    for (const auto& operation : recorded.batch.operations) {
      if (const auto* layout = std::get_if<DirectLayoutOperation>(&operation);
          layout != nullptr && layout->tag == tag) {
        displayType = layout->displayType;
      }
    }
  }
  return displayType;
}

// Activity, Suspense and react-freeze hide a subtree by committing a hidden
// clone of each host node, and show it again by committing the node's own
// version. Showing must restore that version's props: the hidden state is not
// a props patch that outlives the clone.
TEST_F(LoomHeadlessBindingTest, CommittingTheSourceVersionAgainShowsAHiddenClone) {
  using Access = LoomBindingTestAccess;
  LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, 1);
  auto visible = Access::draft(
      1, token->generation, 11, 2, "View",
      folly::dynamic::object("width", 100.0)("height", 50.0)(
          "collapsable", false));
  visible->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
  Access::publish(*binding_, 1, visible, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  EXPECT_EQ(lastDisplayType(*host_, 2), 1);

  auto hidden = std::make_shared<Access::Draft>(*visible);
  hidden->published = false;
  hidden->workerCommitted = false;
  hidden->frozenCommit = 0;
  hidden->freezeFacts = {};
  hidden->offscreenHidden = true;
  Access::publish(*binding_, 1, hidden, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  EXPECT_EQ(lastDisplayType(*host_, 2), 0);

  Access::publish(*binding_, 1, visible, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  EXPECT_EQ(lastDisplayType(*host_, 2), 1);
  EXPECT_TRUE(host_->takeFailures().empty());
}

// Showing a hidden clone again by committing its source version is not a
// paint change, even when the version carries only a paint patch: the
// paint-only shortcut must not keep the node hidden.
TEST_F(LoomHeadlessBindingTest, ShowingAHiddenCloneWithAPaintPatchShowsIt) {
  using Access = LoomBindingTestAccess;
  LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, 1);
  auto visible = Access::draft(
      1, token->generation, 11, 2, "View",
      folly::dynamic::object("width", 100.0)("height", 50.0)(
          "backgroundColor", "red")("collapsable", false));
  visible->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
  Access::publish(*binding_, 1, visible, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  EXPECT_EQ(lastDisplayType(*host_, 2), 1);

  auto hidden = std::make_shared<Access::Draft>(*visible);
  hidden->published = false;
  hidden->workerCommitted = false;
  hidden->frozenCommit = 0;
  hidden->freezeFacts = {};
  hidden->offscreenHidden = true;
  Access::publish(*binding_, 1, hidden, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  EXPECT_EQ(lastDisplayType(*host_, 2), 0);

  auto shown = std::make_shared<Access::Draft>(*visible);
  shown->published = false;
  shown->workerCommitted = false;
  shown->frozenCommit = 0;
  shown->freezeFacts = {};
  auto patch = std::make_shared<Access::Patch>();
  patch->previous = visible->propsTail;
  patch->props = folly::dynamic::object("backgroundColor", "blue");
  shown->propsTail = patch;
  Access::publish(*binding_, 1, shown, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  EXPECT_EQ(lastDisplayType(*host_, 2), 1);
  EXPECT_TRUE(host_->takeFailures().empty());
}

// A forced-sync commit must not mark its versions committed before it has
// reconciled them: otherwise a parent whose new version gained a child looks
// unchanged and the reconcile keeps the live (childless) children instead.
TEST_F(LoomHeadlessBindingTest, AForcedSyncCommitKeepsAChildAddedToAMountedParent) {
  using Access = LoomBindingTestAccess;
  LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, 1);
  auto parent = Access::draft(
      1, token->generation, 11, 2, "View",
      folly::dynamic::object("width", 100.0)("height", 100.0)(
          "collapsable", false));
  parent->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
  parent->childrenExplicit = true;
  Access::publish(*binding_, 1, parent, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  host_->drainUIQueue();
  host_->takeBatches();

  auto child = Access::draft(
      1, token->generation, 12, 3, "View",
      folly::dynamic::object("width", 10.0)("height", 10.0)(
          "collapsable", false));
  child->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
  auto withChild = std::make_shared<Access::Draft>(*parent);
  withChild->published = false;
  withChild->workerCommitted = false;
  withChild->frozenCommit = 0;
  withChild->freezeFacts = {};
  withChild->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>(
          std::vector<std::shared_ptr<Access::Draft>>{child});
  Access::publish(*binding_, 1, withChild, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  host_->drainUIQueue();
  bool created = false;
  for (const auto& recorded : host_->takeBatches()) {
    for (const auto& operation : recorded.batch.operations) {
      if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
          create != nullptr && create->tag == 3) {
        created = true;
      }
    }
  }
  EXPECT_TRUE(created);
  EXPECT_TRUE(host_->takeFailures().empty());
}

// A surface that sizes to its content (no bounded maximum) reports the size
// its root was laid out at, never its unbounded constraints.
TEST_F(LoomHeadlessBindingTest, AContentSizedSurfaceReportsItsLaidOutSize) {
  using Access = LoomBindingTestAccess;
  const float unbounded = std::numeric_limits<float>::infinity();
  LoomBinding::setSurfaceConstraints(1, 0, 0, unbounded, unbounded, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, 1);
  auto child = Access::draft(
      1, token->generation, 11, 2, "View",
      folly::dynamic::object("width", 100.0)("height", 50.0)(
          "collapsable", false));
  child->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
  Access::publish(*binding_, 1, child, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  host_->drainUIQueue();

  auto runtime = loom::Runtime::from(*contextContainer_);
  ASSERT_TRUE(runtime.has_value());
  const auto size = runtime->latest().surfaceSize(1);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(size->width, 100);
  EXPECT_EQ(size->height, 50);
  EXPECT_TRUE(host_->takeFailures().empty());
}

// Regression: the binding registry was a function-local static in a header's anonymous namespace,
// so LoomBindingAnimation.cpp had its own, always empty, copy and dropped every native-props write.
TEST_F(LoomHeadlessBindingTest, NativePropsFromThePlatformReachTheBinding) {
  mountOneView();
  LoomBindingTestAccess::recordNativeProps(2, folly::dynamic::object("opacity", 0.5));
  EXPECT_TRUE(viewGotOpacity(2, 0.5));
}

TEST_F(LoomHeadlessBindingTest, RuntimeIsPerInstanceAndReadsAreExplicit) {
  mountOneView();
  auto runtime = loom::Runtime::from(*contextContainer_);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->surfaceOf(2), std::optional<SurfaceId>{1});
  EXPECT_EQ(runtime->surfaceOf(999), std::nullopt);
  // The test thread is the UI thread. latest() always answers; synced() answers only when JavaScript
  // runs on the UI thread (then the UI thread is the JS thread), and never waits otherwise.
  EXPECT_EQ(runtime->latest().hasNode(1, 2), std::optional<bool>{true});
  EXPECT_EQ(
      runtime->synced().hasNode(1, 2),
      LoomWorker::javaScriptOnUIThread() ? std::optional<bool>{true} : std::nullopt);
  EXPECT_EQ(runtime->synced().surfaceOf(2), LoomWorker::javaScriptOnUIThread() ? std::optional<SurfaceId>{1} : std::nullopt);
  auto layout = runtime->latest().layout(1, 2);
  ASSERT_TRUE(layout.has_value());
  EXPECT_EQ(layout->frame.size.width, 100);
  // The native lane needs the surface now; the wrong surface is ignored.
  runtime->updateNativeProps(5, 2, folly::dynamic::object("opacity", 0.25));
  EXPECT_FALSE(viewGotOpacity(2, 0.25));
  runtime->updateNativeProps(1, 2, folly::dynamic::object("opacity", 0.25));
  EXPECT_TRUE(viewGotOpacity(2, 0.25));

  // A second React instance (its own container, host and binding) sees none of this one's nodes.
  auto otherContainer = std::make_shared<ContextContainer>();
  LoomHeadlessHost::registerIn(*otherContainer);
  RuntimeExecutor runtimeExecutor = [](std::function<void(jsi::Runtime&)>&&) {};
  auto otherManager = std::make_shared<UIManager>(runtimeExecutor, otherContainer);
  auto otherBinding = std::make_shared<LoomBinding>(otherManager);
  auto other = loom::Runtime::from(*otherContainer);
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->surfaceOf(2), std::nullopt);
  EXPECT_EQ(other->latest().hasNode(1, 2), std::nullopt);
}

TEST_F(LoomHeadlessBindingTest, BeforeNextFrameRunsOnlyForItsSurface) {
  auto runtime = loom::Runtime::from(*contextContainer_);
  ASSERT_TRUE(runtime.has_value());
  int ranForSurface1 = 0;
  int ranForSurface7 = 0;
  runtime->beforeNextFrame(1, [&] { ++ranForSurface1; });
  runtime->beforeNextFrame(7, [&] { ++ranForSurface7; });
  mountOneView();
  EXPECT_EQ(ranForSurface1, 1);
  EXPECT_EQ(ranForSurface7, 0);
  // Released with its surface: nothing is left behind.
  LoomBinding::releaseSurface(7);
}

TEST_F(LoomHeadlessBindingTest, HandlesOutliveTheirInstanceSafely) {
  auto runtime = loom::Runtime::from(*contextContainer_);
  ASSERT_TRUE(runtime.has_value());
  auto subscription = runtime->onFrameApplied([](const loom::FrameInfo&) {});
  binding_.reset();
  EXPECT_EQ(runtime->surfaceOf(2), std::nullopt);
  EXPECT_EQ(runtime->latest().hasNode(1, 2), std::nullopt);
  subscription.cancel();
}

TEST_F(LoomHeadlessBindingTest, ReleasingASurfaceForgetsItsNodesAndOverlays) {
  mountOneView();
  auto runtime = loom::Runtime::from(*contextContainer_);
  ASSERT_TRUE(runtime.has_value());
  runtime->updateNativeProps(1, 2, folly::dynamic::object("opacity", 0.5));
  ASSERT_TRUE(viewGotOpacity(2, 0.5));
  LoomBinding::releaseSurface(1);
  host_->drainUIQueue();
  EXPECT_EQ(runtime->surfaceOf(2), std::nullopt);
  // The overlay died with the surface: a platform write for the old tag reaches nobody.
  LoomBindingTestAccess::recordNativeProps(2, folly::dynamic::object("opacity", 0.75));
  EXPECT_FALSE(viewGotOpacity(2, 0.75));
}

// The library animation lane: nothing but Loom writes a library's bag to the view, and a paint-only
// frame (no mount batch) still reports itself to onFrameApplied.
TEST_F(LoomHeadlessBindingTest, LibraryAnimatedPaintReachesTheViewAndReportsAFrame) {
  mountOneView();
  auto runtime = loom::Runtime::from(*contextContainer_);
  ASSERT_TRUE(runtime.has_value());
  int frames = 0;
  auto subscription = runtime->onFrameApplied([&](const loom::FrameInfo& info) {
    if (info.surfaceId == 1) {
      ++frames;
    }
  });
  loom::AnimatedPropBag bag;
  bag.props.push_back(std::make_unique<AnimatedProp<Float>>(OPACITY, Float{0.3}));
  runtime->updateAnimatedProps(1, 2, std::move(bag));
  LoomBinding::flushAnimationFrames();
  host_->drainUIQueue();
  bool written = false;
  for (const auto& update : host_->takeViewProps()) {
    if (update.tag == 2 && update.props.isObject() && update.props.count("opacity") != 0) {
      written = written || std::abs(update.props["opacity"].asDouble() - 0.3) < 1e-6;
    }
  }
  EXPECT_TRUE(written);
  EXPECT_GE(frames, 1);
}

} // namespace
} // namespace facebook::react
