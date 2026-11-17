/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <map>
#include <thread>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#include <react/renderer/runtimescheduler/LoomSettings.h>
#include <limits>

#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/components/root/RootComponentDescriptor.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/loom/Loom.h>
#include <react/renderer/loom/binding/LoomBinding.h>
#include <react/renderer/uimanager/UIManager.h>

#include "../LoomHeadlessHost.h"
#include "LoomAllocationCounter.h"

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

  /** The binding's live scroll-offset read, as the renderer's DOM path calls it. */
  static std::optional<std::pair<float, float>> readScrollOffset(
      LoomBinding& binding,
      SurfaceId surfaceId,
      Tag tag) {
    return binding.readOnUIThread(
        [surfaceId, tag](LoomHost& host)
            -> std::optional<std::pair<float, float>> {
          return host.getScrollOffset(surfaceId, tag);
        },
        std::nullopt);
  }

  static auto tokenFor(LoomBinding& binding, SurfaceId surfaceId) {
    return binding.tokenFor(surfaceId);
  }

  /** Publishes without forcing a synchronous commit: the worker path. */
  static void publishAsync(
      LoomBinding& binding,
      SurfaceId surfaceId,
      const std::shared_ptr<Draft>& root) {
    binding.publishWorkerCommit(
        surfaceId,
        {std::make_shared<LoomBinding::NodeRef>(
            surfaceId, root->tag, binding.tokenFor(surfaceId), root)},
        /*forceSync*/ false);
  }

  /** Publishes a root without children: unmounts everything (worker path). */
  static void publishEmptyAsync(LoomBinding& binding, SurfaceId surfaceId) {
    binding.publishWorkerCommit(surfaceId, {}, /*forceSync*/ false);
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

// A host that keeps live view state under its own lock (liveReadsOnAnyThread)
// answers a read from another thread directly: nothing is posted to the UI
// thread, so a busy UI thread cannot delay it.
TEST_F(LoomHeadlessBindingTest, LiveReadsOnAnyThreadCallTheHostDirectly) {
  auto traits = host_->traits();
  traits.liveReadsOnAnyThread = true;
  LoomPlatformTraits::install(traits);
  host_->setScrollOffset(5, 3, 40);
  const auto postsBefore = host_->uiPostCount();
  std::optional<std::pair<float, float>> value;
  std::thread reader([&] {
    value = LoomBindingTestAccess::readScrollOffset(*binding_, 1, 5);
  });
  reader.join();
  EXPECT_EQ(host_->uiPostCount(), postsBefore);
  EXPECT_EQ(value, (std::optional<std::pair<float, float>>{{3, 40}}));
  LoomPlatformTraits::install(host_->traits());
}

// The default: a read from another thread is answered on the UI thread.
TEST_F(LoomHeadlessBindingTest, LiveReadsGoThroughTheUIThreadByDefault) {
  host_->setScrollOffset(5, 3, 40);
  const auto postsBefore = host_->uiPostCount();
  std::optional<std::pair<float, float>> value;
  std::atomic<bool> done{false};
  std::thread reader([&] {
    value = LoomBindingTestAccess::readScrollOffset(*binding_, 1, 5);
    done = true;
  });
  while (!done) {
    host_->drainUIQueue();
    std::this_thread::yield();
  }
  reader.join();
  EXPECT_EQ(host_->uiPostCount(), postsBefore + 1);
  EXPECT_EQ(value, (std::optional<std::pair<float, float>>{{3, 40}}));
}

/** Frames by tag, from Layout operations and from inserts that carry one. */
struct MountedFrames {
  std::map<Tag, std::array<float, 4>> frames;
  size_t layoutOperations{0};
  size_t insetOperations{0};
  size_t operations{0};
};

MountedFrames mountedFrames(LoomHeadlessHost& host) {
  host.drainUIQueue();
  MountedFrames result;
  for (const auto& recorded : host.takeBatches()) {
    for (const auto& operation : recorded.batch.operations) {
      ++result.operations;
      if (const auto* layout = std::get_if<DirectLayoutOperation>(&operation)) {
        ++result.layoutOperations;
        result.frames[layout->tag] = {layout->x, layout->y, layout->width, layout->height};
      } else if (const auto* insert = std::get_if<DirectInsertOperation>(&operation);
                 insert != nullptr && insert->hasLayout) {
        result.frames[insert->tag] = {insert->x, insert->y, insert->width, insert->height};
      } else if (
          std::holds_alternative<DirectPaddingOperation>(operation) ||
          std::holds_alternative<DirectOverflowInsetOperation>(operation)) {
        ++result.insetOperations;
      }
    }
  }
  return result;
}

// `insertsCarryInitialLayout` moves a new view's first frame onto its insert and
// drops its zero insets; the frames themselves are exactly the default path's.
TEST_F(LoomHeadlessBindingTest, InsertsCarryInitialLayoutWhenTheHostOptsIn) {
  using Access = LoomBindingTestAccess;
  const auto mount = [&](SurfaceId surface, Tag base) {
    LoomBinding::setSurfaceConstraints(surface, 300, 300, 300, 300, false, 0, 0);
    const auto token = Access::tokenFor(*binding_, surface);
    auto outer = Access::draft(
        surface, token->generation, base + 1, base + 1, "View",
        folly::dynamic::object("padding", 7.0)("flexDirection", "row")(
            "collapsable", false));
    for (Tag index = 0; index < 3; ++index) {
      auto child = Access::draft(
          surface, token->generation, base + 10 + index, base + 10 + index, "View",
          folly::dynamic::object("width", 20.0 + index)("height", 10.0)(
              "margin", 2.0)("collapsable", false));
      outer->children->push_back(child);
    }
    Access::publishAsync(*binding_, surface, outer);
    LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
    return mountedFrames(*host_);
  };
  const auto byDefault = mount(31, 3100);
  auto traits = host_->traits();
  traits.insertsCarryInitialLayout = true;
  LoomPlatformTraits::install(traits);
  const auto carried = mount(41, 4100);
  LoomPlatformTraits::install(host_->traits());

  EXPECT_EQ(byDefault.layoutOperations, 4u);
  EXPECT_EQ(byDefault.insetOperations, 8u);
  EXPECT_EQ(carried.layoutOperations, 0u);
  // Only the outer view's padding is non-zero.
  EXPECT_EQ(carried.insetOperations, 1u);
  EXPECT_EQ(carried.operations, 4u + 4u + 1u);
  ASSERT_EQ(carried.frames.size(), byDefault.frames.size());
  for (const auto& [tag, frame] : byDefault.frames) {
    const auto found = carried.frames.find(tag - 3100 + 4100);
    ASSERT_NE(found, carried.frames.end());
    EXPECT_EQ(found->second, frame);
  }
  EXPECT_TRUE(host_->takeFailures().empty());
}

/** Mounts 2 > 3 > 4 > {5, 6} on `surface`, then commits 2 without children. */
std::vector<std::pair<Tag, Tag>> removesOfSubtreeUnmount(
    LoomHeadlessHost& host,
    LoomBinding& binding,
    UIManager& uiManager,
    SurfaceId surface) {
  using Access = LoomBindingTestAccess;
  LoomBinding::setSurfaceConstraints(surface, 300, 300, 300, 300, false, 0, 0);
  const auto token = Access::tokenFor(binding, surface);
  const Tag base = surface * 100;
  const auto view = [&](Tag tag) {
    return Access::draft(
        surface, token->generation, base + tag, base + tag, "View",
        folly::dynamic::object("width", 10.0)("height", 10.0)("collapsable", false));
  };
  auto outer = view(2);
  auto a = view(3);
  auto b = view(4);
  b->children->push_back(view(5));
  b->children->push_back(view(6));
  a->children->push_back(b);
  outer->children->push_back(a);
  Access::publishAsync(binding, surface, outer);
  LoomBinding::synchronizeWorkerForUIManager(uiManager);
  host.drainUIQueue();
  host.takeBatches();

  auto emptied = std::make_shared<Access::Draft>(*outer);
  emptied->published = false;
  emptied->workerCommitted = false;
  emptied->frozenCommit = 0;
  emptied->freezeFacts = {};
  emptied->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
  emptied->childrenExplicit = true;
  Access::publishAsync(binding, surface, emptied);
  LoomBinding::synchronizeWorkerForUIManager(uiManager);
  host.drainUIQueue();
  std::vector<std::pair<Tag, Tag>> removes;
  size_t deletes = 0;
  for (const auto& recorded : host.takeBatches()) {
    for (const auto& operation : recorded.batch.operations) {
      if (const auto* remove = std::get_if<DirectRemoveOperation>(&operation)) {
        removes.emplace_back(remove->parentTag - base, remove->tag - base);
      } else if (std::holds_alternative<DirectDeleteOperation>(operation)) {
        ++deletes;
      }
    }
  }
  EXPECT_EQ(deletes, 4u);
  std::sort(removes.begin(), removes.end());
  return removes;
}

// By default an unmount removes every edge of the subtree, as Fabric does.
// `removesSubtreeRootOnly` keeps only the subtree root's remove.
TEST_F(LoomHeadlessBindingTest, SubtreeRootOnlyRemovesAreOptIn) {
  EXPECT_EQ(
      removesOfSubtreeUnmount(*host_, *binding_, *uiManager_, 51),
      (std::vector<std::pair<Tag, Tag>>{{2, 3}, {3, 4}, {4, 5}, {4, 6}}));
  auto traits = host_->traits();
  traits.removesSubtreeRootOnly = true;
  LoomPlatformTraits::install(traits);
  EXPECT_EQ(
      removesOfSubtreeUnmount(*host_, *binding_, *uiManager_, 61),
      (std::vector<std::pair<Tag, Tag>>{{2, 3}}));
  LoomPlatformTraits::install(host_->traits());
  EXPECT_TRUE(host_->takeFailures().empty());
}

/** Bytes the process has allocated, or 0 where it cannot be read. */
size_t allocatedBytes() {
#if defined(__APPLE__)
  malloc_statistics_t stats{};
  malloc_zone_statistics(nullptr, &stats);
  return stats.size_in_use;
#else
  return 0;
#endif
}

/**
 * A probe, not a check: mounts a grid of cells (View > Paragraph > RawText)
 * through the binding's worker path, with and without early creates, and prints
 * mount operations per kind, the commit time and the bytes held per node.
 */
TEST_F(LoomHeadlessBindingTest, GridMountProbe) {
  using Access = LoomBindingTestAccess;
  constexpr int kCells = 2000;
  for (const int mode : {0, 1, 2}) {
    // 0: default; 1: early creates; 2: early creates, inserts carry layout.
    const bool earlyCreate = mode != 0;
    LoomSettings::set(LoomSettingId::EarlyCreate, earlyCreate ? 1 : 0);
    auto traits = host_->traits();
    traits.insertsCarryInitialLayout = mode == 2;
    LoomPlatformTraits::install(traits);
    const SurfaceId surface = 11 + mode * 10;
    LoomBinding::setSurfaceConstraints(surface, 1000, 100000, 1000, 100000, false, 0, 0);
    const auto token = Access::tokenFor(*binding_, surface);
    const auto bytesBefore = allocatedBytes();
    Tag nextTag = surface * 100000 + 2;
    uint64_t identity = surface * 100000;
    auto grid = Access::draft(
        surface, token->generation, ++identity, nextTag++, "View",
        folly::dynamic::object("flexDirection", "row")("flexWrap", "wrap")(
            "collapsable", false));
    for (int cell = 0; cell < kCells; ++cell) {
      auto view = Access::draft(
          surface, token->generation, ++identity, nextTag++, "View",
          folly::dynamic::object("width", 48.0)("height", 24.0)(
              "margin", 1.0)("borderWidth", 1.0)("backgroundColor", 0xffeeeeee)(
              "collapsable", false));
      auto paragraph = Access::draft(
          surface, token->generation, ++identity, nextTag++, "Paragraph",
          folly::dynamic::object("fontSize", 12.0)("color", 0xff000000)(
              "numberOfLines", 1));
      auto text = std::make_shared<Access::Draft>();
      text->surfaceId = surface;
      text->surfaceGeneration = token->generation;
      text->identity = ++identity;
      text->tag = nextTag++;
      text->componentName = std::make_shared<const std::string>("RawText");
      text->roles = LoomPlatformTraits::current().components.classify("RawText");
      text->initialRawText =
          std::make_shared<const std::string>("cell " + std::to_string(cell));
      text->hasInitialProps = true;
      paragraph->children->push_back(text);
      view->children->push_back(paragraph);
      grid->children->push_back(view);
    }
    const auto bytesDrafts = allocatedBytes();
    const auto start = std::chrono::steady_clock::now();
    Access::publishAsync(*binding_, surface, grid);
    LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
    host_->drainUIQueue();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    std::array<size_t, 8> counts{};
    size_t batches = 0;
    for (const auto& recorded : host_->takeBatches()) {
      ++batches;
      for (const auto& operation : recorded.batch.operations) {
        ++counts[operation.index()];
      }
    }
    const auto bytesMounted = allocatedBytes();
    const auto nodes = static_cast<size_t>(nextTag - (surface * 100000 + 2));
    size_t total = 0;
    for (const auto count : counts) {
      total += count;
    }
    printf(
        "[probe] grid mount, %d cells, %zu nodes, mode=%d: %zu batches, "
        "%zu ops (create %zu, update %zu, insert %zu, remove %zu, layout %zu, "
        "delete %zu, padding %zu, overflowInset %zu), commit %lld us, "
        "drafts %zu B/node, mounted %zu B/node\n",
        kCells, nodes, mode, batches, total, counts[0], counts[1],
        counts[2], counts[3], counts[4], counts[5], counts[6], counts[7],
        static_cast<long long>(micros),
        (bytesDrafts - bytesBefore) / nodes,
        (bytesMounted - bytesDrafts) / nodes);
    // A relayout of every cell: each View gets a new version with a width patch.
    const auto clone = [](const std::shared_ptr<Access::Draft>& source) {
      auto next = std::make_shared<Access::Draft>(*source);
      next->published = false;
      next->workerCommitted = false;
      next->frozenCommit = 0;
      next->freezeFacts = {};
      return next;
    };
    auto nextGrid = clone(grid);
    nextGrid->children =
        std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
    for (const auto& view : *grid->children) {
      auto next = clone(view);
      auto patch = std::make_shared<Access::Patch>();
      patch->previous = view->propsTail;
      patch->props = folly::dynamic::object("width", 46.0);
      next->propsTail = patch;
      nextGrid->children->push_back(next);
    }
    const auto updateStart = std::chrono::steady_clock::now();
    Access::publishAsync(*binding_, surface, nextGrid);
    LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
    host_->drainUIQueue();
    const auto updateMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - updateStart)
            .count();
    size_t updateOps = 0;
    for (const auto& recorded : host_->takeBatches()) {
      updateOps += recorded.batch.operations.size();
    }
    printf(
        "[probe] grid relayout, mode=%d: %zu ops, commit %lld us\n",
        mode, updateOps, static_cast<long long>(updateMicros));
    EXPECT_TRUE(host_->takeFailures().empty());
  }
  LoomPlatformTraits::install(host_->traits());
  LoomSettings::resetForTesting();
}

/**
 * A probe, not a check: three cycles of mounting a grid of cells (View >
 * Paragraph > RawText, LOOM_PROBE_CELLS of them, 5000 by default), relaying
 * out every cell and unmounting the grid. Prints, per node, the allocations and
 * the bytes each phase leaves allocated, the bytes still held after the unmount
 * while the JavaScript side still holds its handles and after it drops them
 * (a GC), and the commit times. The held bytes must not grow across cycles.
 */
TEST_F(LoomHeadlessBindingTest, GridLifecycleProbe) {
  using Access = LoomBindingTestAccess;
  using loomtest::allocationCount;
  int cells = 5000;
  if (const char* value = std::getenv("LOOM_PROBE_CELLS")) {
    cells = std::max(1, std::atoi(value));
  }
  const SurfaceId surface = 91;
  LoomBinding::setSurfaceConstraints(surface, 1000, 1000000, 1000, 1000000, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, surface);
  // The headless host records what it is sent; drop the records so they are
  // not counted as held by the core.
  const auto settle = [&] {
    LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
    host_->drainUIQueue();
    host_->takeBatches();
    host_->takeTextStates();
    host_->takeViewProps();
  };
  const auto clone = [](const std::shared_ptr<Access::Draft>& source) {
    auto next = std::make_shared<Access::Draft>(*source);
    next->published = false;
    next->workerCommitted = false;
    next->frozenCommit = 0;
    next->freezeFacts = {};
    return next;
  };
  const auto microsSince = [](auto start) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  };
  const auto perNode = [](long long bytes, size_t nodes) {
    return static_cast<double>(bytes) / static_cast<double>(nodes);
  };
  // Warm the process (first-use statics, the worker) on a small mount.
  {
    auto warm = Access::draft(
        surface, token->generation, 1, 2, "View",
        folly::dynamic::object("width", 10.0)("collapsable", false));
    Access::publishAsync(*binding_, surface, warm);
    settle();
    Access::publishEmptyAsync(*binding_, surface);
    settle();
  }
  const auto baseline = static_cast<long long>(allocatedBytes());
  Tag nextTag = 10;
  uint64_t identity = 1000;
  for (int cycle = 0; cycle < 3; ++cycle) {
    const auto cycleStart = static_cast<long long>(allocatedBytes());
    auto grid = Access::draft(
        surface, token->generation, ++identity, nextTag++, "View",
        folly::dynamic::object("flexDirection", "row")("flexWrap", "wrap")(
            "collapsable", false));
    for (int cell = 0; cell < cells; ++cell) {
      auto view = Access::draft(
          surface, token->generation, ++identity, nextTag++, "View",
          folly::dynamic::object("width", 48.0)("height", 24.0)(
              "margin", 1.0)("borderWidth", 1.0)("backgroundColor", 0xffeeeeee)(
              "collapsable", false));
      auto paragraph = Access::draft(
          surface, token->generation, ++identity, nextTag++, "Paragraph",
          folly::dynamic::object("fontSize", 12.0)("color", 0xff000000)(
              "numberOfLines", 1));
      auto text = std::make_shared<Access::Draft>();
      text->surfaceId = surface;
      text->surfaceGeneration = token->generation;
      text->identity = ++identity;
      text->tag = nextTag++;
      text->componentName = std::make_shared<const std::string>("RawText");
      text->roles = LoomPlatformTraits::current().components.classify("RawText");
      text->initialRawText =
          std::make_shared<const std::string>("cell " + std::to_string(cell));
      text->hasInitialProps = true;
      paragraph->children->push_back(text);
      view->children->push_back(paragraph);
      grid->children->push_back(view);
    }
    const size_t nodes = 1 + static_cast<size_t>(cells) * 3;
    const auto afterDrafts = static_cast<long long>(allocatedBytes());

    auto allocations = allocationCount();
    loomtest::startAllocationSites();
    auto start = std::chrono::steady_clock::now();
    Access::publishAsync(*binding_, surface, grid);
    settle();
    const auto mountMicros = microsSince(start);
    loomtest::printAllocationSites("mount");
    const auto mountAllocations = allocationCount() - allocations;
    const auto afterMount = static_cast<long long>(allocatedBytes());

    auto nextGrid = clone(grid);
    nextGrid->children =
        std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();
    for (const auto& view : *grid->children) {
      auto next = clone(view);
      auto patch = std::make_shared<Access::Patch>();
      patch->previous = view->propsTail;
      patch->props = folly::dynamic::object("width", 46.0);
      next->propsTail = patch;
      nextGrid->children->push_back(next);
    }
    allocations = allocationCount();
    start = std::chrono::steady_clock::now();
    Access::publishAsync(*binding_, surface, nextGrid);
    settle();
    const auto relayoutMicros = microsSince(start);
    const auto relayoutAllocations = allocationCount() - allocations;

    allocations = allocationCount();
    start = std::chrono::steady_clock::now();
    Access::publishEmptyAsync(*binding_, surface);
    settle();
    const auto unmountMicros = microsSince(start);
    const auto unmountAllocations = allocationCount() - allocations;
    // JavaScript still holds its handles here: only what the core holds for
    // them, beyond the drafts the test built, is a cost of the core.
    const auto heldWithHandles =
        static_cast<long long>(allocatedBytes()) - afterDrafts;
    grid.reset();
    nextGrid.reset();
    const auto heldAfterGC =
        static_cast<long long>(allocatedBytes()) - cycleStart;
    loomtest::stopAllocationSites();
    loomtest::printAllocationSites("cycle, after unmount and GC", true);

    printf(
        "[probe] lifecycle cycle %d, %d cells, %zu nodes: "
        "mount %lld us %.1f allocs/node %.0f B/node held; "
        "relayout %lld us %.1f allocs/node; "
        "unmount %lld us %.1f allocs/node; "
        "after unmount %.0f B/node held with JS handles, %.0f B/node after GC, "
        "%lld B above the first cycle's start\n",
        cycle, cells, nodes, mountMicros,
        static_cast<double>(mountAllocations) / nodes,
        perNode(afterMount - afterDrafts, nodes), relayoutMicros,
        static_cast<double>(relayoutAllocations) / nodes, unmountMicros,
        static_cast<double>(unmountAllocations) / nodes,
        perNode(heldWithHandles, nodes), perNode(heldAfterGC, nodes),
        static_cast<long long>(allocatedBytes()) - baseline);
    EXPECT_TRUE(host_->takeFailures().empty());
  }
}

} // namespace
} // namespace facebook::react
