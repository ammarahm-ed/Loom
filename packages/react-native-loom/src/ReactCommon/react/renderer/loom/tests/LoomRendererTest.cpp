/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/components/root/RootComponentDescriptor.h>
#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/core/ComponentDescriptor.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/renderer/uimanager/UIManager.h>
#include "LoomTestTraits.h"
#include "TestUIManagerDelegate.h"

#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/featureflags/ReactNativeFeatureFlagsDefaults.h>
#include <react/renderer/animationbackend/AnimatedProps.h>
#include <react/renderer/loom/core/LoomRenderer.h>
#include <react/renderer/loom/binding/LoomBinding.h>
#include <react/renderer/loom/worker/LoomWorker.h>

namespace facebook::react {

namespace {

LoomRenderer::NativeOperations testOperations() {
  LoomRenderer::NativeOperations operations;
  operations.applyBatch = [](SurfaceId, LoomMountBatch) {};
  operations.registerEventTarget = [](SurfaceId, Tag, InstanceHandle::Shared) {
  };
  operations.unregisterEventTarget = [](SurfaceId, Tag) {};
  return operations;
}

folly::dynamic encodeForTest(const LoomMountBatch& batch) {
  auto encoded = folly::dynamic::array();
  size_t instructionCount = 0;
  size_t objectCount = 0;
  for (const auto& operation : batch.operations) {
    std::visit(
        [&](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, DirectCreateOperation>) {
            instructionCount += 3;
            objectCount += 2;
            encoded.push_back(0);
            encoded.push_back(static_cast<int>(typed.tag));
            encoded.push_back(typed.layoutable ? 1 : 0);
            encoded.push_back(typed.componentName);
            encoded.push_back(typed.props);
          } else if constexpr (std::is_same_v<T, DirectUpdatePropsOperation>) {
            instructionCount += 2;
            objectCount += 1;
            encoded.push_back(1);
            encoded.push_back(static_cast<int>(typed.tag));
            encoded.push_back(typed.props);
          } else if constexpr (
              std::is_same_v<T, DirectInsertOperation> ||
              std::is_same_v<T, DirectRemoveOperation>) {
            instructionCount += 4;
            encoded.push_back(std::is_same_v<T, DirectInsertOperation> ? 2 : 3);
            encoded.push_back(static_cast<int>(typed.parentTag));
            encoded.push_back(static_cast<int>(typed.tag));
            encoded.push_back(static_cast<int>(typed.index));
          } else if constexpr (std::is_same_v<T, DirectLayoutOperation>) {
            instructionCount += 9;
            encoded.push_back(4);
            encoded.push_back(static_cast<int>(typed.tag));
            encoded.push_back(static_cast<int>(typed.parentTag));
            encoded.push_back(typed.displayType);
            encoded.push_back(typed.layoutDirection);
            encoded.push_back(typed.x);
            encoded.push_back(typed.y);
            encoded.push_back(typed.width);
            encoded.push_back(typed.height);
          } else if constexpr (std::is_same_v<T, DirectDeleteOperation>) {
            instructionCount += 2;
            encoded.push_back(5);
            encoded.push_back(static_cast<int>(typed.tag));
          } else {
            instructionCount += 6;
            encoded.push_back(
                std::is_same_v<T, DirectPaddingOperation> ? 6 : 7);
            encoded.push_back(static_cast<int>(typed.tag));
            for (const auto inset : typed.insets)
              encoded.push_back(inset);
          }
        },
        operation);
  }
  EXPECT_EQ(instructionCount, batch.instructionCount);
  EXPECT_EQ(objectCount, batch.objectCount);
  return encoded;
}

folly::dynamic sizedProps(float width) {
  // Most existing tests exercise host-view mounting explicitly. Flattening is
  // covered separately using the production default (collapsable=true).
  return folly::dynamic::object("width", width)("height", 20.0)(
      "collapsable", false);
}

void makeRoot(
    LoomRenderer& renderer,
    float width = 200,
    float height = 200) {
  renderer.setConstraints(width, height, width, height);
  renderer.createNode(1, "Root", folly::dynamic::object());
}

std::vector<std::tuple<int, int, int, int>> structureOperations(
    const folly::dynamic& operations) {
  std::vector<std::tuple<int, int, int, int>> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    switch (operation) {
      case 0: // Create: 4 operands.
        index += 4;
        break;
      case 1: // UpdateProps: 2 operands.
        index += 2;
        break;
      case 2: // Insert
      case 3: { // Remove
        const auto parent = static_cast<int>(operations[index++].asInt());
        const auto child = static_cast<int>(operations[index++].asInt());
        const auto childIndex = static_cast<int>(operations[index++].asInt());
        result.emplace_back(operation, parent, child, childIndex);
        break;
      }
      case 4: // Layout: 8 operands.
        index += 8;
        break;
      case 5: // Delete: 1 operand.
        index += 1;
        break;
      case 6: // Padding
      case 7: // Overflow inset
        index += 5;
        break;
      default:
        ADD_FAILURE() << "Unknown direct operation " << operation;
        return result;
    }
    if (index > operations.size()) {
      ADD_FAILURE() << "Direct operation overruns its instruction batch";
      return result;
    }
  }
  return result;
}

// The sequence of direct-operation kinds in a batch, decoded with the same
// operand widths the other helpers use. Lets a test assert on which kinds a
// batch contains without depending on the tags or props inside it.
std::vector<int> operationKinds(const folly::dynamic& operations) {
  std::vector<int> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    result.push_back(operation);
    switch (operation) {
      case 0: // Create
        index += 4;
        break;
      case 1: // UpdateProps
        index += 2;
        break;
      case 2: // Insert
      case 3: // Remove
        index += 3;
        break;
      case 4: // Layout
        index += 8;
        break;
      case 5: // Delete
        index += 1;
        break;
      case 6: // Padding
      case 7: // Overflow inset
        index += 5;
        break;
      default:
        ADD_FAILURE() << "Unknown direct operation " << operation;
        return result;
    }
  }
  return result;
}

std::vector<Tag> deletedTags(const folly::dynamic& operations) {
  std::vector<Tag> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    switch (operation) {
      case 0:
        index += 4;
        break;
      case 1:
        index += 2;
        break;
      case 2:
      case 3:
        index += 3;
        break;
      case 4:
        index += 8;
        break;
      case 5:
        result.push_back(static_cast<Tag>(operations[index++].asInt()));
        break;
      case 6:
      case 7:
        index += 5;
        break;
      default:
        ADD_FAILURE() << "Unknown direct operation " << operation;
        return result;
    }
  }
  return result;
}

std::vector<Tag> createdTags(const folly::dynamic& operations) {
  std::vector<Tag> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    switch (operation) {
      case 0:
        result.push_back(static_cast<Tag>(operations[index].asInt()));
        index += 4;
        break;
      case 1:
        index += 2;
        break;
      case 2:
      case 3:
        index += 3;
        break;
      case 4:
        index += 8;
        break;
      case 5:
        index += 1;
        break;
      case 6:
      case 7:
        index += 5;
        break;
      default:
        ADD_FAILURE() << "Unknown direct operation " << operation;
        return result;
    }
  }
  return result;
}

std::vector<folly::dynamic> updatedProps(const folly::dynamic& operations) {
  std::vector<folly::dynamic> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    switch (operation) {
      case 0:
        index += 4;
        break;
      case 1:
        index++; // tag
        result.push_back(operations[index++]);
        break;
      case 2:
      case 3:
        index += 3;
        break;
      case 4:
        index += 8;
        break;
      case 5:
        index++;
        break;
      case 6:
      case 7:
        index += 5;
        break;
      default:
        ADD_FAILURE() << "Unknown direct operation " << operation;
        return result;
    }
  }
  return result;
}

std::vector<std::pair<Tag, Tag>> layoutOperations(
    const folly::dynamic& operations) {
  std::vector<std::pair<Tag, Tag>> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    switch (operation) {
      case 0:
        index += 4;
        break;
      case 1:
        index += 2;
        break;
      case 2:
      case 3:
        index += 3;
        break;
      case 4: {
        const auto tag = static_cast<Tag>(operations[index++].asInt());
        const auto parent = static_cast<Tag>(operations[index++].asInt());
        result.emplace_back(tag, parent);
        index += 6;
        break;
      }
      case 5:
        index += 1;
        break;
      case 6:
      case 7:
        index += 5;
        break;
      default:
        ADD_FAILURE() << "Unknown direct operation " << operation;
        return result;
    }
    if (index > operations.size()) {
      ADD_FAILURE() << "Direct operation overruns its instruction batch";
      return result;
    }
  }
  return result;
}

std::vector<std::pair<Tag, std::array<float, 4>>> overflowOperations(
    const folly::dynamic& operations) {
  std::vector<std::pair<Tag, std::array<float, 4>>> result;
  for (size_t index = 0; index < operations.size();) {
    const auto operation = static_cast<int>(operations[index++].asInt());
    if (operation == 7) {
      const auto tag = static_cast<Tag>(operations[index++].asInt());
      std::array<float, 4> insets{};
      for (auto& inset : insets)
        inset = operations[index++].asDouble();
      result.emplace_back(tag, insets);
    } else if (operation == 0) {
      index += 4;
    } else if (operation == 1 || operation == 5) {
      index += operation == 1 ? 2 : 1;
    } else if (operation == 2 || operation == 3) {
      index += 3;
    } else if (operation == 4) {
      index += 8;
    } else if (operation == 6) {
      index += 5;
    } else {
      ADD_FAILURE() << "Unknown direct operation " << operation;
      break;
    }
  }
  return result;
}

} // namespace

TEST(
    LoomRendererTest,
    RetainedNodeWithUnchangedGeometryDoesNotReemitLayout) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  // `cloneNode` is a no-op by contract (same props, same children), so a second
  // commit of the unchanged node is the same statement - and is the path the
  // reconciler actually takes.
  renderer.completeRoot(1, {2});

  EXPECT_TRUE(layoutOperations(batch).empty());
}

TEST(LoomRendererTest, LayoutConformanceUsesAndroidViewHostName) {
  LoomMountBatch batch;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = std::move(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2, "LayoutConformance", folly::dynamic::object("mode", "strict"));
  renderer.completeRoot(1, {2});

  const auto created = std::find_if(
      batch.operations.begin(),
      batch.operations.end(),
      [](const auto& operation) {
        return std::holds_alternative<DirectCreateOperation>(operation);
      });
  ASSERT_NE(created, batch.operations.end());
  EXPECT_EQ(std::get<DirectCreateOperation>(*created).componentName, "RCTView");
}

TEST(
    LoomRendererTest,
    LayoutOnlyViewFlattensChildrenAndPreservesLayoutEvent) {
  LoomMountBatch batch;
  std::vector<Tag> layoutEvents;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = std::move(next);
  };
  operations.dispatchEvent = [&layoutEvents](
                                 SurfaceId,
                                 Tag tag,
                                 const std::string&,
                                 folly::dynamic,
                                 RawEvent::Category,
                                 bool) { layoutEvents.push_back(tag); };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 100.0)("height", 100.0)("left", 20.0)(
          "top", 30.0)("position", "absolute")("padding", 10.0)(
          "onLayout", true));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  bool wrapperIsVirtual = false;
  bool childInsertedAtRoot = false;
  bool childHasFlattenedFrame = false;
  for (const auto& operation : batch.operations) {
    if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
        create != nullptr && create->tag == 2) {
      wrapperIsVirtual = !create->layoutable && create->props.empty();
    } else if (const auto* insert =
                   std::get_if<DirectInsertOperation>(&operation);
               insert != nullptr && insert->tag == 3) {
      childInsertedAtRoot = insert->parentTag == 1;
    } else if (const auto* layout =
                   std::get_if<DirectLayoutOperation>(&operation);
               layout != nullptr && layout->tag == 3) {
      childHasFlattenedFrame = layout->parentTag == 1 &&
          std::abs(layout->x - 30.0f) < 0.01f &&
          std::abs(layout->y - 40.0f) < 0.01f;
    }
  }
  EXPECT_TRUE(wrapperIsVirtual);
  EXPECT_TRUE(childInsertedAtRoot);
  EXPECT_TRUE(childHasFlattenedFrame);
  EXPECT_EQ(layoutEvents, (std::vector<Tag>{2}));
}

TEST(LoomRendererTest, NativeVisibleViewPropPreventsFlattening) {
  LoomMountBatch batch;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = std::move(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 100.0)("height", 100.0)(
          "backgroundColor", 0xFF00FF00));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  bool wrapperIsReal = false;
  bool childInsertedAtWrapper = false;
  for (const auto& operation : batch.operations) {
    if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
        create != nullptr && create->tag == 2) {
      wrapperIsReal = create->layoutable;
    } else if (const auto* insert =
                   std::get_if<DirectInsertOperation>(&operation);
               insert != nullptr && insert->tag == 3) {
      childInsertedAtWrapper = insert->parentTag == 2;
    }
  }
  EXPECT_TRUE(wrapperIsReal);
  EXPECT_TRUE(childInsertedAtWrapper);
}

TEST(LoomRendererTest, PaintPatchCannotLeaveLayoutOnlyNodeUnmaterialized) {
  LoomMountBatch batch;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = std::move(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2, "RCTView", folly::dynamic::object("width", 100.0)("height", 100.0));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_FALSE(renderer.tryCommitPaintOnlyProps(
      {{2, folly::dynamic::object("backgroundColor", 0xFF00FF00)}}));

  renderer.cloneNodeWithNewProps(
      2, folly::dynamic::object("backgroundColor", 0xFF00FF00));
  renderer.completeRoot(1, {2});
  bool wrapperIsReal = false;
  for (const auto& operation : batch.operations) {
    if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
        create != nullptr && create->tag == 2) {
      wrapperIsReal = create->layoutable;
    }
  }
  EXPECT_TRUE(wrapperIsReal);
}

TEST(LoomRendererTest, RetainedNodeWithChangedGeometryEmitsLayout) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  renderer.cloneNodeWithNewProps(2, folly::dynamic::object("width", 70.0));
  renderer.completeRoot(1, {2});

  EXPECT_EQ(
      layoutOperations(batch), (std::vector<std::pair<Tag, Tag>>{{2, 1}}));
}

TEST(LoomRendererTest, RetainedNodeMoveEmitsLayoutWhenParentChangesOnly) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.createNode(3, "RCTView", sizedProps(20));
  renderer.createNode(4, "RCTView", sizedProps(100));
  renderer.setIdentity(2, 2);
  renderer.setIdentity(3, 3);
  renderer.setIdentity(4, 4);
  renderer.setChildren(2, {3});
  renderer.setChildren(1, {2, 4});
  renderer.completeRoot(1, {2, 4});

  batch = folly::dynamic::array();
  renderer.captureRemovedMountedEdges({{1, 0}, {2, 2}, {3, 3}, {4, 4}});
  renderer.setChildren(2, {});
  renderer.setChildren(4, {3});
  renderer.setChildren(1, {2, 4});
  renderer.completeRoot(1, {2, 4});

  const auto layouts = layoutOperations(batch);
  EXPECT_EQ(
      std::count(layouts.begin(), layouts.end(), std::make_pair(3, 4)), 1);
}

TEST(LoomRendererTest, NewHostNodeEmitsInitialLayoutAndOnLayoutEvent) {
  folly::dynamic batch = folly::dynamic::array();
  std::vector<std::pair<Tag, std::string>> events;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  operations.dispatchEvent = [&events](
                                 SurfaceId,
                                 Tag tag,
                                 const std::string& type,
                                 folly::dynamic,
                                 RawEvent::Category,
                                 bool) { events.emplace_back(tag, type); };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  auto props = sizedProps(30);
  props["onLayout"] = true;
  renderer.createNode(2, "RCTView", props);
  renderer.completeRoot(1, {2});

  EXPECT_EQ(
      layoutOperations(batch), (std::vector<std::pair<Tag, Tag>>{{2, 1}}));
  EXPECT_EQ(
      events, (std::vector<std::pair<Tag, std::string>>{{2, "topLayout"}}));
}

namespace {
/** Installs a prop-diff policy for one test and restores the previous traits. */
struct ScopedPropDiffPolicy {
  explicit ScopedPropDiffPolicy(bool (*policy)(const std::string&))
      : saved_(LoomPlatformTraits::current()) {
    auto traits = saved_;
    traits.sendsPropDiffs = policy;
    LoomPlatformTraits::install(std::move(traits));
  }
  ~ScopedPropDiffPolicy() {
    LoomPlatformTraits::install(saved_);
  }
  LoomPlatformTraits saved_;
};

/** Diffs for the core view managers only; every other component gets full props. */
bool coreViewsSendPropDiffs(const std::string& name) {
  return name == "RCTView" || name == "RCTImageView" ||
      name == "RCTScrollView" || name == "AndroidHorizontalScrollView";
}
} // namespace

TEST(LoomRendererTest, PaintOnlyPropsCommitSkipsLayoutAndKeepsFullProps) {
  const ScopedPropDiffPolicy policy(coreViewsSendPropDiffs);
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "LoomTestLibraryView", sizedProps(100));
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  const auto accepted = renderer.tryCommitPaintOnlyProps(
      {{2,
        folly::dynamic::object(
            "style", folly::dynamic::object("backgroundColor", 0xff00ff))}});

  ASSERT_TRUE(accepted);
  EXPECT_EQ(batch.size(), 3);
  EXPECT_EQ(batch[0].asInt(), 1);
  EXPECT_EQ(batch[1].asInt(), 2);
  EXPECT_EQ(batch[2]["width"].asDouble(), 100.0);
  EXPECT_EQ(batch[2]["height"].asDouble(), 20.0);
  EXPECT_EQ(batch[2]["backgroundColor"].asInt(), 0xff00ff);
}

TEST(LoomRendererTest, PaintOnlyPropsRejectLayoutAndTextChanges) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.completeRoot(1, {2});

  EXPECT_FALSE(renderer.tryCommitPaintOnlyProps(
      {{2,
        folly::dynamic::object(
            "style", folly::dynamic::object("width", 80.0))}}));
  EXPECT_FALSE(renderer.tryCommitPaintOnlyProps(
      {{2, folly::dynamic::object("accessibilityLabel", "changed")}}));
}

// Include the upstream AnimatedProps header above as a compile-time regression
// check: the lane type must coexist with the backend type in the same TU.
TEST(LoomRendererTest, AnimationLaneClassifiesNativeDriverTransform) {
  AnimatedProps backendProps;
  EXPECT_TRUE(backendProps.props.empty());
  auto transform =
      folly::dynamic::array(folly::dynamic::object("translateX", 0));
  const std::vector<folly::dynamic> payloads{
           folly::dynamic::object("transform", transform),
           folly::dynamic::object(
               "style", folly::dynamic::object("transform", transform))};
  for (const auto& payload : payloads) {
    auto classified = classifyAnimatedProps(payload);
    ASSERT_TRUE(classified.paint.isObject());
    ASSERT_TRUE(classified.layout.isObject());
    EXPECT_EQ(classified.paint.at("transform"), transform);
    EXPECT_TRUE(classified.layout.empty());
    EXPECT_FALSE(classified.text);
  }
}

TEST(LoomRendererTest, AnimationLaneSplitsPaintLayoutAndText) {
  auto classified = classifyAnimatedProps(
      folly::dynamic::object("opacity", 0.5)("width", 40)("zIndex", 2)(
          "fontSize", 16));
  folly::dynamic expectedPaint = folly::dynamic::object("opacity", 0.5);
  folly::dynamic expectedLayout =
      folly::dynamic::object("width", 40)("zIndex", 2)("fontSize", 16);
  EXPECT_EQ(classified.paint, expectedPaint);
  EXPECT_EQ(classified.layout, expectedLayout);
  EXPECT_TRUE(classified.text);
  EXPECT_TRUE(classifyAnimatedProps(nullptr).empty());
  EXPECT_TRUE(classifyAnimatedProps(folly::dynamic::array()).empty());
  auto clearedStyle =
      classifyAnimatedProps(folly::dynamic::object("style", nullptr));
  EXPECT_TRUE(clearedStyle.paint.isObject());
  EXPECT_TRUE(clearedStyle.layout.isObject());
  auto nonStringKey = classifyAnimatedProps(folly::dynamic::object(42, true));
  EXPECT_EQ(nonStringKey.layout.at(42), true);
}

TEST(LoomRendererTest, NativeAnimatedPatchShapesUseThePaintOnlyPath) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.completeRoot(1, {2});

  // Native Animated sends a nested style object, not a flattened prop. Both keys
  // it animates have to be on the paint-only allow-list, otherwise every
  // animated frame on every node silently falls back to a full commit - which is
  // the regression this pins.
  EXPECT_TRUE(renderer.tryCommitPaintOnlyProps(
      {{2,
        folly::dynamic::object(
            "style", folly::dynamic::object("opacity", 0.5))}}));
  EXPECT_TRUE(renderer.tryCommitPaintOnlyProps(
      {{2,
        folly::dynamic::object(
            "style", folly::dynamic::object("backgroundColor", 0xff00ff))}}));

  // A patch that can affect layout or text is not animatable this way.
  EXPECT_FALSE(renderer.tryCommitPaintOnlyProps(
      {{2, folly::dynamic::object("style", folly::dynamic::object("width", 20.0))}}));
}

/*
 * `setNativeProps` consults the paint-only path first and falls back to a full
 * commit. The fast path merges props exactly as `patchProps` does and skips only
 * work that is a no-op for the props it accepts, so the emitted batch and the
 * resulting node state are identical either way - this test pins that observable
 * contract (merged props, no structure operations, layout patches still
 * layouting) rather than the path taken, which is not visible from a batch.
 */
TEST(LoomRendererTest, PaintOnlySetNativePropsMergesPropsWithoutStructuralWork) {
  const ScopedPropDiffPolicy policy(coreViewsSendPropDiffs);
  std::vector<folly::dynamic> batches;
  auto operations = testOperations();
  operations.applyBatch = [&batches](SurfaceId, LoomMountBatch next) {
    batches.push_back(encodeForTest(next));
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "LoomTestLibraryView", sizedProps(100));
  renderer.completeRoot(1, {2});

  batches.clear();
  renderer.setNativeProps(
      2,
      folly::dynamic::object(
          "style", folly::dynamic::object("opacity", 0.5)));

  ASSERT_EQ(batches.size(), 1);
  EXPECT_EQ(operationKinds(batches[0]), (std::vector<int>{1}));
  EXPECT_EQ(batches[0][2]["opacity"].asDouble(), 0.5);
  // The patch merges onto the existing props rather than replacing them, so the
  // host view keeps its layout size even though no layout ran.
  EXPECT_EQ(batches[0][2]["width"].asDouble(), 100.0);

  // A layout change cannot use the fast path, so it must still commit fully.
  batches.clear();
  renderer.setNativeProps(2, folly::dynamic::object("width", 20.0));

  ASSERT_EQ(batches.size(), 1);
  const auto kinds = operationKinds(batches[0]);
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), 4), kinds.end())
      << "a layout-affecting setNativeProps must still emit a Layout operation";
}

TEST(LoomRendererTest, NestedOverflowInsetsIncludeDescendantBounds) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(40));
  auto childProps = sizedProps(80);
  childProps["left"] = 10.0;
  renderer.createNode(3, "RCTView", childProps);
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  const auto overflow = overflowOperations(batch);
  const auto rootContainer =
      std::find_if(overflow.begin(), overflow.end(), [](const auto& entry) {
        return entry.first == 2;
      });
  ASSERT_NE(rootContainer, overflow.end());
  EXPECT_EQ(rootContainer->second[2], -50.0f);
}

TEST(LoomRendererTest, IdenticalChildOrderDoesNotEmitStructureOperations) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.createNode(3, "RCTView", sizedProps(20));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_TRUE(structureOperations(batch).empty());
}

TEST(LoomRendererTest, MountedChildrenFollowStableZIndexOrder) {
  LoomMountBatch batch;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = std::move(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  auto frontProps = sizedProps(100);
  frontProps["zIndex"] = 10.0;
  auto backProps = sizedProps(100);
  backProps["zIndex"] = 0.0;
  renderer.createNode(2, "RCTView", frontProps);
  renderer.createNode(3, "RCTView", backProps);
  renderer.setChildren(1, {2, 3});
  renderer.completeRoot(1, {2, 3});

  EXPECT_EQ(
      structureOperations(encodeForTest(batch)),
      (std::vector<std::tuple<int, int, int, int>>{
          {2, 1, 3, 0}, {2, 1, 2, 1}}));
}

TEST(LoomRendererTest, SurfaceStopDuringStateApplicationAbortsTheBatch) {
  bool surfaceActive = true;
  int stateUpdates = 0;
  int batches = 0;
  int layoutEvents = 0;
  int finishedCommits = 0;
  auto operations = testOperations();
  operations.isSurfaceActive = [&surfaceActive](SurfaceId) {
    return surfaceActive;
  };
  operations.updateState = [&surfaceActive, &stateUpdates](LoomTextStateUpdate update) {
    ++stateUpdates;
    surfaceActive = false;
  };
  operations.applyBatch = [&batches](SurfaceId, LoomMountBatch) {
    ++batches;
  };
  operations.dispatchEvent = [&layoutEvents](
                                 SurfaceId,
                                 Tag,
                                 const std::string&,
                                 folly::dynamic,
                                 RawEvent::Category,
                                 bool) { ++layoutEvents; };
  operations.commitFinished = [&finishedCommits](SurfaceId) {
    ++finishedCommits;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  auto props = sizedProps(100);
  props["onLayout"] = true;
  renderer.createNode(2, "RCTScrollView", props);
  renderer.completeRoot(1, {2});

  EXPECT_EQ(stateUpdates, 1);
  EXPECT_EQ(batches, 0);
  EXPECT_EQ(layoutEvents, 0);
  EXPECT_EQ(finishedCommits, 0);
}

TEST(LoomRendererTest, StateFailureFailStopsSurfaceAndResetsCommitGuard) {
  bool surfaceActive = true;
  std::string failureReason;
  int finishedCommits = 0;
  auto operations = testOperations();
  operations.isSurfaceActive = [&surfaceActive](SurfaceId) {
    return surfaceActive;
  };
  operations.updateState = [](LoomTextStateUpdate update) {
    throw std::runtime_error("state preparation failed");
  };
  operations.failSurface = [&surfaceActive, &failureReason](
                               SurfaceId, const std::string& reason) {
    surfaceActive = false;
    failureReason = reason;
  };
  operations.commitFinished = [&finishedCommits](SurfaceId) {
    ++finishedCommits;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTScrollView", sizedProps(100));

  EXPECT_THROW(renderer.completeRoot(1, {2}), std::runtime_error);
  EXPECT_EQ(failureReason, "state preparation failed");
  EXPECT_FALSE(renderer.isCommitting());
  EXPECT_TRUE(renderer.hasFailed());
  EXPECT_EQ(finishedCommits, 0);
}

TEST(LoomRendererTest, TextPreparationFailureFailStopsSurface) {
  bool surfaceActive = true;
  std::string failureReason;
  auto operations = testOperations();
  operations.isSurfaceActive = [&surfaceActive](SurfaceId) {
    return surfaceActive;
  };
  operations.parseTextAttributes = [](SurfaceId,
                                      const std::string&,
                                      const folly::dynamic&,
                                      const TextAttributes&,
                                      ParagraphAttributes*) -> TextAttributes {
    throw std::runtime_error("text attribute preparation failed");
  };
  operations.failSurface = [&surfaceActive, &failureReason](
                               SurfaceId, const std::string& reason) {
    surfaceActive = false;
    failureReason = reason;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(100));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});

  EXPECT_THROW(renderer.completeRoot(1, {2}), std::runtime_error);
  EXPECT_EQ(failureReason, "text attribute preparation failed");
  EXPECT_TRUE(renderer.hasFailed());
  EXPECT_FALSE(renderer.isCommitting());
}

TEST(LoomRendererTest, BatchFailureFailStopsSurface) {
  bool surfaceActive = true;
  std::string failureReason;
  auto operations = testOperations();
  operations.isSurfaceActive = [&surfaceActive](SurfaceId) {
    return surfaceActive;
  };
  operations.applyBatch = [](SurfaceId, LoomMountBatch) {
    throw std::runtime_error("host batch failed");
  };
  operations.failSurface = [&surfaceActive, &failureReason](
                               SurfaceId, const std::string& reason) {
    surfaceActive = false;
    failureReason = reason;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(30));

  EXPECT_THROW(renderer.completeRoot(1, {2}), std::runtime_error);
  EXPECT_EQ(failureReason, "host batch failed");
  EXPECT_FALSE(renderer.isCommitting());
  EXPECT_TRUE(renderer.hasFailed());
}

TEST(LoomRendererTest, InlineAttachmentValidationDoesNotMutateLiveTree) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(120));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "old"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  const auto selectedNames = std::unordered_map<Tag, std::string>{
      {2, "RCTText"},
      {3, "RCTRawText"},
      {4, "RCTView"},
  };
  const auto selectedChildren = std::unordered_map<Tag, std::vector<Tag>>{
      {2, {3, 4}},
      {3, {}},
      {4, {}},
  };

  const auto nameFor = [&](Tag tag) {
    const auto found = selectedNames.find(tag);
    return found == selectedNames.end()
        ? LoomComponentTable::Roles{}
        : LoomPlatformTraits::current().components.classify(found->second);
  };
  const auto childrenFor = [&](Tag tag) -> const std::vector<Tag>* {
    const auto found = selectedChildren.find(tag);
    return found == selectedChildren.end() ? nullptr : &found->second;
  };
  EXPECT_EQ(
      LoomRenderer::findUnsupportedInlineAttachment(
          {2}, nameFor, childrenFor),
      4);
  EXPECT_TRUE(renderer.exists(2));
  EXPECT_TRUE(renderer.exists(3));
  EXPECT_FALSE(renderer.exists(4));
  EXPECT_EQ(renderer.childrenOf(2), (std::vector<Tag>{3}));
}

TEST(LoomRendererTest, NestedVirtualTextAndRawTextAreValidInlineContent) {
  const auto names = std::unordered_map<Tag, std::string>{
      {2, "RCTText"},
      {3, "RCTVirtualText"},
      {4, "RCTRawText"},
      {5, "Text"},
      {6, "RawText"},
  };
  const auto children = std::unordered_map<Tag, std::vector<Tag>>{
      {2, {3}},
      {3, {4, 5}},
      {4, {}},
      {5, {6}},
      {6, {}},
  };

  const auto nameFor = [&](Tag tag) {
    const auto found = names.find(tag);
    return found == names.end()
        ? LoomComponentTable::Roles{}
        : LoomPlatformTraits::current().components.classify(found->second);
  };
  const auto childrenFor = [&](Tag tag) -> const std::vector<Tag>* {
    const auto found = children.find(tag);
    return found == children.end() ? nullptr : &found->second;
  };
  EXPECT_EQ(
      LoomRenderer::findUnsupportedInlineAttachment(
          {2}, nameFor, childrenFor),
      0);
}

TEST(
    LoomRendererTest,
    SurfaceStopDuringLayoutEventSuppressesLaterCallbacks) {
  bool surfaceActive = true;
  int layoutEvents = 0;
  int finishedCommits = 0;
  auto operations = testOperations();
  operations.isSurfaceActive = [&surfaceActive](SurfaceId) {
    return surfaceActive;
  };
  operations.dispatchEvent = [&surfaceActive, &layoutEvents](
                                 SurfaceId,
                                 Tag,
                                 const std::string&,
                                 folly::dynamic,
                                 RawEvent::Category,
                                 bool) {
    ++layoutEvents;
    surfaceActive = false;
  };
  operations.commitFinished = [&finishedCommits](SurfaceId) {
    ++finishedCommits;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  auto firstProps = sizedProps(40);
  firstProps["onLayout"] = true;
  auto secondProps = sizedProps(40);
  secondProps["onLayout"] = true;
  renderer.createNode(2, "RCTView", firstProps);
  renderer.createNode(3, "RCTView", secondProps);
  renderer.completeRoot(1, {2, 3});

  EXPECT_EQ(layoutEvents, 1);
  EXPECT_EQ(finishedCommits, 0);
}

TEST(
    LoomRendererTest,
    UsesPreparedTextOnlyWhenItMatchesFinalYogaContentSize) {
  bool preparedStatePublished = false;
  int prepareCalls = 0;
  auto operations = testOperations();
  operations.prepareText = [&prepareCalls](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size& minimum,
                               const Size& maximum) {
    ++prepareCalls;
    LoomTextMeasurement result;
    result.size = {100, 20};
    result.minimumSize = minimum;
    result.maximumSize = maximum;
    result.valid = true;
    result.preparedLayout = std::make_shared<int>(42);
    return result;
  };
  operations.updateState = [&preparedStatePublished](LoomTextStateUpdate update) {
    const auto& prepared = update.prepared;
    preparedStatePublished =
        preparedStatePublished || (prepared.valid && prepared.preparedLayout);
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", folly::dynamic::object("width", 100.0));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_EQ(prepareCalls, 1);
  LoomRenderer::Metrics metrics;
  EXPECT_TRUE(renderer.metricsRelativeTo(2, 1, metrics));
  EXPECT_TRUE(preparedStatePublished)
      << "final text frame=" << metrics.width << "x" << metrics.height;
}

TEST(
    LoomRendererTest,
    PaintOnlyTextChangeRefreshesLayoutWithoutYogaMeasure) {
  int yogaPreparations = 0;
  int mountPreparations = 0;
  std::vector<SharedColor> publishedColors;
  auto operations = testOperations();
  operations.parseTextAttributes = [](SurfaceId,
                                      const std::string&,
                                      const folly::dynamic& props,
                                      const TextAttributes& inherited,
                                      ParagraphAttributes*) {
    auto attributes = inherited;
    if (props.getDefault("color", "red").asString() == "blue") {
      attributes.foregroundColor = colorFromRGBA(0, 0, 255, 255);
    } else {
      attributes.foregroundColor = colorFromRGBA(255, 0, 0, 255);
    }
    return attributes;
  };
  operations.prepareText = [&yogaPreparations](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size& minimum,
                               const Size& maximum) {
    ++yogaPreparations;
    LoomTextMeasurement measurement;
    measurement.size = {100, 20};
    measurement.minimumSize = minimum;
    measurement.maximumSize = maximum;
    measurement.valid = true;
    measurement.preparedLayout = std::make_shared<int>(yogaPreparations);
    return measurement;
  };
  operations.prepareTextForMount = [&mountPreparations](
                                       SurfaceId,
                                       const AttributedString&,
                                       const ParagraphAttributes&,
                                       const Size& minimum,
                                       const Size& maximum) {
    ++mountPreparations;
    LoomTextMeasurement prepared;
    prepared.valid = true;
    prepared.preparedLayout = std::make_shared<int>(mountPreparations);
    prepared.minimumSize = minimum;
    prepared.maximumSize = maximum;
    return prepared;
  };
  operations.updateState = [&publishedColors](LoomTextStateUpdate update) {
    const auto& prepared = update.prepared;
    const auto& text = *update.attributedString;
    ASSERT_TRUE(prepared.valid);
    ASSERT_FALSE(text.getFragments().empty());
    publishedColors.push_back(
        text.getFragments().front().textAttributes.foregroundColor);
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2, "RCTText", folly::dynamic::object("width", 100.0)("color", "red"));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});
  ASSERT_EQ(yogaPreparations, 1);

  renderer.cloneNodeWithNewProps(2, folly::dynamic::object("color", "blue"));
  renderer.completeRoot(1, {2});

  EXPECT_EQ(yogaPreparations, 1);
  EXPECT_EQ(mountPreparations, 1);
  ASSERT_EQ(publishedColors.size(), 2u);
  EXPECT_EQ(publishedColors.back(), colorFromRGBA(0, 0, 255, 255));
}

TEST(
    LoomRendererTest,
    SkipsMountReprepareWhenMeasurementHasNoRenderableLayout) {
  int yogaPreparations = 0;
  int mountPreparations = 0;
  bool stateReceivedPreparedLayout = true;
  auto operations = testOperations();
  operations.prepareTextForMountProvidesLayout = false;
  operations.prepareText = [&yogaPreparations](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size& minimum,
                               const Size& maximum) {
    ++yogaPreparations;
    LoomTextMeasurement measurement;
    measurement.size = {100, 20};
    measurement.minimumSize = minimum;
    measurement.maximumSize = maximum;
    measurement.valid = true;
    return measurement;
  };
  operations.prepareTextForMount = [&mountPreparations](
                                       SurfaceId,
                                       const AttributedString&,
                                       const ParagraphAttributes&,
                                       const Size& minimum,
                                       const Size& maximum) {
    ++mountPreparations;
    LoomTextMeasurement measurement;
    measurement.size = {100, 20};
    measurement.minimumSize = minimum;
    measurement.maximumSize = maximum;
    measurement.valid = true;
    return measurement;
  };
  operations.updateState = [&stateReceivedPreparedLayout](LoomTextStateUpdate update) {
    const auto& prepared = update.prepared;
    stateReceivedPreparedLayout = prepared.valid && prepared.preparedLayout;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", folly::dynamic::object("width", 100.0));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_EQ(yogaPreparations, 1);
  EXPECT_EQ(mountPreparations, 0);
  EXPECT_FALSE(stateReceivedPreparedLayout);
}

/**
 * G9: `aspectRatio` also accepts a CSS ratio string, but only under the same
 * feature flag Fabric's `convertAspectRatio` uses, and a degenerate ratio is
 * ignored there too.
 *
 * The flag has to be installed before anything reads a flag at all — the
 * renderer's constructor reads one, and the API refuses to override afterwards —
 * so this is a static initialiser rather than a per-test override. Everything
 * else keeps its default, including `fixYogaFlexBasisFitContentInMainAxis`.
 */
class AspectRatioFlagOverride : public ReactNativeFeatureFlagsDefaults {
 public:
  bool enableNativeCSSParsing() override {
    return true;
  }
};

const bool aspectRatioFlagInstalled = [] {
  ReactNativeFeatureFlags::override(std::make_unique<AspectRatioFlagOverride>());
  return true;
}();

TEST(LoomRendererTest, AspectRatioAcceptsACssRatioStringWhenEnabled) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  // Width fixed, height unset: the ratio decides the height.
  renderer.createNode(
      2, "RCTView",
      folly::dynamic::object("width", 160.0)("aspectRatio", "16/9"));
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(2, 0, metrics));
  EXPECT_FLOAT_EQ(metrics.width, 160);
  EXPECT_FLOAT_EQ(metrics.height, 90);
}

TEST(LoomRendererTest, DegenerateAspectRatioStringIsIgnored) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  // A zero denominator is degenerate: Fabric ignores the ratio, and so does the
  // Loom renderer, so the height stays unset rather than becoming infinite.
  renderer.createNode(
      2, "RCTView",
      folly::dynamic::object("width", 160.0)("aspectRatio", "16/0"));
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(2, 0, metrics));
  EXPECT_FLOAT_EQ(metrics.width, 160);
  EXPECT_FLOAT_EQ(metrics.height, 0);
}

TEST(LoomRendererTest, UnchangedTextNodesAreNotReparsedOnRebuild) {
  // A text root rebuilds its whole subtree whenever anything under it changes.
  // Every node in that subtree used to cross the attribute bridge again, even
  // the ones whose props and inherited attributes were untouched. Counting the
  // bridge calls is the only way to see the difference, because the rendering
  // result is identical either way.
  int parseCalls = 0;
  auto operations = testOperations();
  operations.parseTextAttributes = [&parseCalls](
                                      SurfaceId,
                                      const std::string&,
                                      const folly::dynamic& props,
                                      const TextAttributes& inherited,
                                      ParagraphAttributes*) {
    ++parseCalls;
    auto attributes = inherited;
    attributes.fontSize = props.getDefault("fontSize", 14.0).asDouble();
    return attributes;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2, "RCTText", folly::dynamic::object("width", 100.0)("fontSize", 14.0));
  // Two virtual text children with different sizes; only one will change.
  renderer.createNode(
      3, "RCTVirtualText", folly::dynamic::object("fontSize", 12.0));
  renderer.createNode(
      4, "RCTVirtualText", folly::dynamic::object("fontSize", 16.0));
  renderer.createNode(5, "RCTRawText", folly::dynamic::object("text", "a"));
  renderer.createNode(6, "RCTRawText", folly::dynamic::object("text", "b"));
  renderer.setChildren(3, {5});
  renderer.setChildren(4, {6});
  renderer.setChildren(2, {3, 4});
  renderer.completeRoot(1, {2});

  // Rebuild the subtree by changing only tag 4. Tag 3 keeps its props and its
  // inherited attributes, so its parse is still valid.
  const auto parsedBefore = parseCalls;
  renderer.cloneNodeWithNewProps(
      4, folly::dynamic::object("fontSize", 20.0));
  renderer.completeRoot(1, {2});

  // Tag 4 must be reparsed; the paragraph root and tag 3 need not be.
  const auto reparsed = parseCalls - parsedBefore;
  EXPECT_GT(reparsed, 0) << "the changed node must be reparsed";
  // Only `textLike` nodes cross the bridge: raw-text leaves are appended with
  // the attributes they inherit. So a rebuild touches the paragraph root and the
  // two virtual-text children, and of those only tag 4 changed. Everything else
  // must come from the parse cache.
  EXPECT_EQ(reparsed, 1)
      << "only the changed text node may be reparsed";

  // A second commit with no changes at all must not reparse anything.
  const auto parsedAfterChange = parseCalls;
  renderer.completeRoot(1, {2});
  EXPECT_EQ(parseCalls, parsedAfterChange)
      << "a rebuild with no prop changes must reuse every parse";
}

TEST(LoomRendererTest, FontSizeChangeInvalidatesTextMeasurement) {
  int yogaPreparations = 0;
  auto operations = testOperations();
  operations.parseTextAttributes = [](SurfaceId,
                                      const std::string&,
                                      const folly::dynamic& props,
                                      const TextAttributes& inherited,
                                      ParagraphAttributes*) {
    auto attributes = inherited;
    attributes.fontSize = props.getDefault("fontSize", 14.0).asDouble();
    return attributes;
  };
  operations.prepareText = [&yogaPreparations](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size& minimum,
                               const Size& maximum) {
    ++yogaPreparations;
    LoomTextMeasurement measurement;
    measurement.size = {100, 20};
    measurement.minimumSize = minimum;
    measurement.maximumSize = maximum;
    measurement.valid = true;
    measurement.preparedLayout = std::make_shared<int>(yogaPreparations);
    return measurement;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2, "RCTText", folly::dynamic::object("width", 100.0)("fontSize", 14.0));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});
  ASSERT_EQ(yogaPreparations, 1);

  renderer.cloneNodeWithNewProps(2, folly::dynamic::object("fontSize", 22.0));
  renderer.completeRoot(1, {2});

  EXPECT_EQ(yogaPreparations, 2);
}

TEST(LoomRendererTest, ControlledTextInputStateTracksItsValueProp) {
  std::vector<std::string> publishedValues;
  auto operations = testOperations();
  operations.updateState = [&publishedValues](LoomTextStateUpdate update) {
    const auto& text = *update.attributedString;
    ASSERT_EQ(update.kind, LoomTextStateKind::TextInput);
    std::string value;
    for (const auto& fragment : text.getFragments()) {
      value += fragment.string;
    }
    publishedValues.push_back(std::move(value));
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "AndroidTextInput",
      folly::dynamic::object("width", 160.0)("height", 40.0)("value", ""));
  renderer.completeRoot(1, {2});

  renderer.cloneNodeWithNewProps(2, folly::dynamic::object("value", "x"));
  renderer.completeRoot(1, {2});

  ASSERT_EQ(publishedValues.size(), 2u);
  EXPECT_EQ(publishedValues[0], "");
  EXPECT_EQ(publishedValues[1], "x");
}

/** Drives the private text-measurement path, which is otherwise unreachable. */
struct LoomRendererTestAccess {
  static LoomRenderer::TextMeasurement measure(
      const LoomRenderer& renderer,
      Tag tag,
      const Size& minimum,
      const Size& maximum) {
    const auto* node = renderer.find(tag);
    if (node == nullptr) {
      throw std::logic_error("no such node");
    }
    return renderer.measureTextNode(*node, minimum, maximum);
  }
};

TEST(LoomRendererTest, EmptyTextInputMeasuresItsPlaceholderButPublishesNothing) {
  // G4: an empty TextInput measured an empty attributed string, so an
  // auto-height input collapsed to zero while Fabric sizes it from the
  // placeholder. The substitution must be measurement-only - the hint is not
  // the user's text, so publishing it would put the placeholder into the field.
  std::vector<std::string> measuredContents;
  std::vector<std::string> publishedValues;
  auto operations = testOperations();
  operations.measureText = [&measuredContents](
                               SurfaceId,
                               const AttributedString& text,
                               const ParagraphAttributes&,
                               const Size&,
                               const Size& maximum) {
    std::string content;
    for (const auto& fragment : text.getFragments()) {
      content += fragment.string;
    }
    measuredContents.push_back(std::move(content));
    LoomRenderer::TextMeasurement result;
    // Report a real height only when there is something to draw; that is what
    // makes the measurement, not the assertion, the thing under test.
    result.width = maximum.width;
    result.height = text.getString().empty() ? 0.0f : 18.0f;
    result.valid = true;
    return result;
  };
  operations.updateState = [&publishedValues](LoomTextStateUpdate update) {
    const auto& text = *update.attributedString;
    std::string value;
    for (const auto& fragment : text.getFragments()) {
      value += fragment.string;
    }
    publishedValues.push_back(std::move(value));
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "AndroidTextInput",
      folly::dynamic::object("width", 160.0)("value", "")(
          "placeholder", "Search"));
  renderer.completeRoot(1, {2});

  const auto measurement =
      LoomRendererTestAccess::measure(renderer, 2, Size{}, Size{160, 1000});
  EXPECT_GT(measurement.height, 0.0f)
      << "an empty input with a placeholder must not measure zero height";

  ASSERT_FALSE(measuredContents.empty());
  EXPECT_EQ(measuredContents.front(), "Search")
      << "the placeholder is what gets measured";

  // And the published state is still the empty text, never the hint.
  ASSERT_FALSE(publishedValues.empty());
  EXPECT_EQ(publishedValues.front(), "")
      << "the placeholder must not be published as the input's value";
}

TEST(
    LoomRendererTest,
    ExistingViewsReceiveFullMergedPropsAndExplicitResets) {
  const ScopedPropDiffPolicy policy(coreViewsSendPropDiffs);
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "LoomTestLibraryView",
      folly::dynamic::object(
          "style",
          folly::dynamic::object("width", 100.0)("backgroundColor", "red")));
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  renderer.cloneNodeWithNewProps(
      2,
      folly::dynamic::object(
          "style", folly::dynamic::object("backgroundColor", "blue")));
  renderer.completeRoot(1, {2});
  auto patches = updatedProps(batch);
  ASSERT_EQ(patches.size(), 1u);
  ASSERT_EQ(patches[0].size(), 2u);
  EXPECT_DOUBLE_EQ(patches[0]["width"].asDouble(), 100.0);
  EXPECT_EQ(patches[0]["backgroundColor"].asString(), "blue");

  batch = folly::dynamic::array();
  renderer.cloneNodeWithNewProps(
      2,
      folly::dynamic::object(
          "style", folly::dynamic::object("backgroundColor", nullptr)));
  renderer.completeRoot(1, {2});
  patches = updatedProps(batch);
  ASSERT_EQ(patches.size(), 1u);
  ASSERT_EQ(patches[0].size(), 2u);
  EXPECT_DOUBLE_EQ(patches[0]["width"].asDouble(), 100.0);
  EXPECT_TRUE(patches[0]["backgroundColor"].isNull());

  batch = folly::dynamic::array();
  renderer.cloneNodeWithNewProps(2, folly::dynamic::object("style", nullptr));
  renderer.completeRoot(1, {2});
  patches = updatedProps(batch);
  ASSERT_EQ(patches.size(), 1u);
  EXPECT_TRUE(patches[0]["width"].isNull());
  EXPECT_TRUE(patches[0]["backgroundColor"].isNull());
}


// A host whose core view managers apply props incrementally sends only the
// keys that changed for an update to an existing core view (resets as explicit
// nulls). Library components keep receiving the full merged set (above).
TEST(LoomRendererTest, CoreViewsReceiveOnlyChangedPropsAndExplicitResets) {
  const ScopedPropDiffPolicy policy(coreViewsSendPropDiffs);
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object(
          "style",
          folly::dynamic::object("width", 100.0)("backgroundColor", "red")));
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  renderer.cloneNodeWithNewProps(
      2,
      folly::dynamic::object(
          "style", folly::dynamic::object("backgroundColor", "blue")));
  renderer.completeRoot(1, {2});
  auto patches = updatedProps(batch);
  ASSERT_EQ(patches.size(), 1u);
  ASSERT_EQ(patches[0].size(), 1u);
  EXPECT_EQ(patches[0]["backgroundColor"].asString(), "blue");

  batch = folly::dynamic::array();
  renderer.cloneNodeWithNewProps(
      2,
      folly::dynamic::object(
          "style", folly::dynamic::object("backgroundColor", nullptr)));
  renderer.completeRoot(1, {2});
  patches = updatedProps(batch);
  ASSERT_EQ(patches.size(), 1u);
  ASSERT_EQ(patches[0].size(), 1u);
  EXPECT_TRUE(patches[0]["backgroundColor"].isNull());
}

TEST(LoomRendererTest, TagReuseIsMarkedOnlyOnReplacementBatch) {
  std::vector<bool> replacementBatches;
  auto operations = testOperations();
  operations.applyBatch = [&replacementBatches](
                              SurfaceId, LoomMountBatch batch) {
    replacementBatches.push_back(batch.requiresTagReplacement);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(80));
  renderer.completeRoot(1, {2});
  renderer.createNode(2, "RCTView", sizedProps(80));
  renderer.completeRoot(1, {2});

  EXPECT_EQ(replacementBatches, (std::vector<bool>{false, true}));
}

TEST(LoomRendererTest, FlattenAndNativeSwitchEmitsDeleteAndCreateForSameTag) {
  // A View that switches between layout-only and native within one commit
  // re-uses its own tag: the batch must both retire the previous incarnation
  // (Delete) and create the new one (Create) for that tag, and must mark the
  // batch as a replacement. Android's `reusedTags` pre-pass and iOS's
  // replaced-tag bookkeeping are keyed off exactly this contract; without the
  // marking, the trailing delete of the retired incarnation tears down the live
  // tag instead.
  std::vector<LoomMountBatch> batches;
  auto operations = testOperations();
  operations.applyBatch = [&batches](SurfaceId, LoomMountBatch batch) {
    batches.push_back(std::move(batch));
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  const folly::dynamic layoutOnlyProps =
      folly::dynamic::object("width", 80.0)("height", 20.0);
  renderer.createNode(2, "RCTView", sizedProps(80));
  renderer.completeRoot(1, {2});
  // Native -> flattened.
  renderer.createNode(2, "RCTView", layoutOnlyProps);
  renderer.completeRoot(1, {2});
  // Flattened -> native (a paint prop needs a host view).
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 80.0)("height", 20.0)(
          "backgroundColor", "red"));
  renderer.completeRoot(1, {2});

  ASSERT_EQ(batches.size(), 3u);
  const auto transitions = [](const LoomMountBatch& batch) {
    bool created = false;
    bool deleted = false;
    for (const auto& operation : batch.operations) {
      if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
          create != nullptr && create->tag == 2) {
        created = true;
      }
      if (std::holds_alternative<DirectDeleteOperation>(operation) &&
          std::get<DirectDeleteOperation>(operation).tag == 2) {
        deleted = true;
      }
    }
    return std::make_pair(created, deleted);
  };

  EXPECT_FALSE(batches[0].requiresTagReplacement);
  EXPECT_EQ(transitions(batches[0]), std::make_pair(true, false));

  for (const auto index : {1u, 2u}) {
    EXPECT_TRUE(batches[index].requiresTagReplacement);
    const auto [created, deleted] = transitions(batches[index]);
    EXPECT_TRUE(created) << "transition batch " << index << " has no create";
    EXPECT_TRUE(deleted) << "transition batch " << index << " has no delete";
  }
}

TEST(LoomRendererTest, TextMeasurementCacheServesSeveralConstraintPairs) {
  // Yoga measures one paragraph under several constraint pairs in a layout pass.
  // A single cache slot meant the second pair evicted the first, so returning to
  // the first pair rebuilt the platform layout. The bridge call count is the
  // cost being avoided, so that is what this asserts.
  int prepareCalls = 0;
  auto operations = testOperations();
  operations.prepareText = [&prepareCalls](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size& minimum,
                               const Size& maximum) {
    ++prepareCalls;
    LoomTextMeasurement result;
    result.size = Size{maximum.width, minimum.height};
    result.valid = true;
    result.preparedLayout = std::make_shared<int>(7);
    return result;
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  // `maxHeight` makes Yoga measure the paragraph: with an explicit height there
  // is nothing to measure and the bridge is never called.
  renderer.createNode(
      2, "RCTText", folly::dynamic::object("width", 100.0)("maxHeight", 10.0));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});
  const auto afterCommit = prepareCalls;
  ASSERT_GE(afterCommit, 1) << "the commit must have measured the paragraph";

  const auto wide = LoomRendererTestAccess::measure(
      renderer, 2, Size{0, 0}, Size{100, 20});
  const auto afterWide = prepareCalls;
  // A second pair, which used to evict the first.
  LoomRendererTestAccess::measure(renderer, 2, Size{0, 0}, Size{50, 20});
  EXPECT_EQ(prepareCalls, afterWide + 1);

  // Back to the first pair: it must come from the cache, not the bridge.
  const auto wideAgain = LoomRendererTestAccess::measure(
      renderer, 2, Size{0, 0}, Size{100, 20});
  EXPECT_EQ(prepareCalls, afterWide + 1)
      << "the first constraint pair must still be cached";
  EXPECT_FLOAT_EQ(wideAgain.width, wide.width);
}

TEST(LoomRendererTest, RepreparesTextForYogaClampedContentSize) {
  bool preparedStatePublished = false;
  int prepareCalls = 0;
  Size finalMinimum;
  Size finalMaximum;
  auto operations = testOperations();
  operations.prepareText = [&prepareCalls, &finalMinimum, &finalMaximum](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size& minimum,
                               const Size& maximum) {
    ++prepareCalls;
    LoomTextMeasurement result;
    // Simulate an unconstrained intrinsic measurement that Yoga clamps, then
    // a second preparation at the exact final content size.
    result.size = prepareCalls == 1 ? Size{100, 20} : maximum;
    result.minimumSize = minimum;
    result.maximumSize = maximum;
    result.valid = true;
    result.preparedLayout = std::make_shared<int>(42);
    if (prepareCalls == 2) {
      finalMinimum = minimum;
      finalMaximum = maximum;
    }
    return result;
  };
  operations.updateState = [&preparedStatePublished](LoomTextStateUpdate update) {
    const auto& prepared = update.prepared;
    preparedStatePublished =
        preparedStatePublished || (prepared.valid && prepared.preparedLayout);
  };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2, "RCTText", folly::dynamic::object("width", 100.0)("maxHeight", 10.0));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "hello"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_EQ(prepareCalls, 2);
  EXPECT_TRUE(preparedStatePublished);
  EXPECT_FLOAT_EQ(finalMinimum.width, 100);
  EXPECT_FLOAT_EQ(finalMinimum.height, 10);
  EXPECT_FLOAT_EQ(finalMaximum.width, 100);
  EXPECT_FLOAT_EQ(finalMaximum.height, 10);
}

TEST(LoomRendererTest, LiveTreeReplacesRawTextChildWithoutRetainingOldTag) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(100));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "old"));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  renderer.setChildren(2, {});
  renderer.createNode(5, "RCTRawText", folly::dynamic::object("text", "new"));
  renderer.setChildren(2, {5});
  renderer.completeRoot(1, {2});

  EXPECT_TRUE(deletedTags(batch).empty());
  EXPECT_TRUE(createdTags(batch).empty());
  EXPECT_TRUE(renderer.exists(5));
  EXPECT_FALSE(renderer.exists(3));
  EXPECT_EQ(renderer.textContent(2), "new");
}

TEST(LoomRendererTest, RawTextIsAParagraphOwnedLeafWithNoHostLifecycle) {
  folly::dynamic batch = folly::dynamic::array();
  std::vector<std::pair<Tag, std::string>> events;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  operations.dispatchEvent = [&events](
                                 SurfaceId,
                                 Tag tag,
                                 const std::string& type,
                                 folly::dynamic,
                                 RawEvent::Category,
                                 bool) { events.emplace_back(tag, type); };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  auto paragraphProps = sizedProps(100);
  paragraphProps["onLayout"] = true;
  renderer.createNode(2, "RCTText", paragraphProps);
  renderer.createNode(3, "RCTVirtualText", folly::dynamic::object());
  renderer.createNode(
      4,
      "RCTRawText",
      folly::dynamic::object("text", "nested")("onLayout", true));
  renderer.setIdentity(4, 44);
  renderer.setChildren(3, {4});
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_TRUE(renderer.exists(4));
  EXPECT_EQ(renderer.identityOf(4), 44);
  EXPECT_EQ(renderer.componentName(4), "RCTRawText");
  EXPECT_EQ(renderer.parentOf(4), 3);
  EXPECT_EQ(renderer.textContent(2), "nested");
  EXPECT_EQ(createdTags(batch), (std::vector<Tag>{2, 3}));
  EXPECT_TRUE(deletedTags(batch).empty());
  EXPECT_EQ(
      events, (std::vector<std::pair<Tag, std::string>>{{2, "topLayout"}}));
}

TEST(LoomRendererTest, TypedRawTextUpdateInvalidatesOnlyItsParagraph) {
  folly::dynamic batch = folly::dynamic::array();
  std::string mountedText;
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  operations.updateState = [&mountedText](LoomTextStateUpdate update) {
    const auto& text = *update.attributedString;
    mountedText.clear();
    for (const auto& fragment : text.getFragments()) {
      mountedText += fragment.string;
    }
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(100));
  renderer.createRawTextNode(3, "old");
  renderer.setIdentity(3, 31);
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  renderer.updateRawText(3, "new");
  renderer.completeRoot(1, {2});

  EXPECT_EQ(mountedText, "new");
  EXPECT_EQ(renderer.textContent(2), "new");
  EXPECT_TRUE(createdTags(batch).empty());
  EXPECT_TRUE(deletedTags(batch).empty());
}

TEST(
    LoomRendererTest,
    ReusedRawTextTagChangesIdentityWithoutViewOperations) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(100));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "old"));
  renderer.setIdentity(3, 31);
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  batch = folly::dynamic::array();
  renderer.setChildren(2, {});
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "new"));
  renderer.setIdentity(3, 32);
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_TRUE(renderer.exists(3));
  EXPECT_NE(renderer.identityOf(3), 31);
  EXPECT_EQ(renderer.identityOf(3), 32);
  EXPECT_EQ(renderer.parentOf(3), 2);
  EXPECT_EQ(renderer.textContent(2), "new");
  EXPECT_TRUE(createdTags(batch).empty());
  EXPECT_TRUE(deletedTags(batch).empty());
}

TEST(LoomRendererTest, RemovedRawTextDoesNotLeaveAStaleHandle) {
  auto operations = testOperations();
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(100));
  renderer.createNode(3, "RCTRawText", folly::dynamic::object("text", "old"));
  renderer.setIdentity(3, 31);
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  renderer.setChildren(2, {});
  renderer.completeRoot(1, {2});

  EXPECT_FALSE(renderer.exists(3));
  EXPECT_EQ(renderer.identityOf(3), 0);
  EXPECT_TRUE(renderer.componentName(3).empty());
  EXPECT_EQ(renderer.parentOf(3), 0);
}

TEST(LoomRendererTest, LiveTreeMovesRetainedChildWithoutDeletingIt) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  renderer.setChildren(2, {});
  renderer.createNode(4, "RCTView", sizedProps(100));
  renderer.setChildren(4, {3});
  renderer.setChildren(1, {2, 4});
  renderer.completeRoot(1, {2, 4});

  EXPECT_TRUE(deletedTags(batch).empty());
  EXPECT_EQ(
      structureOperations(batch),
      (std::vector<std::tuple<int, int, int, int>>{
          {3, 2, 3, 0}, {2, 1, 4, 1}, {2, 4, 3, 0}}));
  EXPECT_TRUE(renderer.exists(3));
  EXPECT_EQ(renderer.childrenOf(2), std::vector<Tag>{});
  EXPECT_EQ(renderer.childrenOf(4), (std::vector<Tag>{3}));
}

TEST(LoomRendererTest, RemovedNestedViewsDetachAndDeleteChildBeforeParent) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.createNode(3, "RCTView", sizedProps(80));
  renderer.createNode(4, "RCTView", sizedProps(60));
  renderer.setChildren(3, {4});
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  renderer.captureRemovedMountedEdges({{1, 0}});
  renderer.completeRoot(1, {});

  EXPECT_EQ(deletedTags(batch), (std::vector<Tag>{4, 3, 2}));
  EXPECT_EQ(
      structureOperations(batch),
      (std::vector<std::tuple<int, int, int, int>>{
          {3, 3, 4, 0}, {3, 2, 3, 0}, {3, 1, 2, 0}}));
}

TEST(
    LoomRendererTest,
    RemovingFlattenedWrapperDoesNotRemoveThroughNonNativeParent) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 100.0)("height", 100.0)("padding", 4.0));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  renderer.captureRemovedMountedEdges({{1, 0}});
  renderer.completeRoot(1, {});

  EXPECT_EQ(deletedTags(batch), (std::vector<Tag>{3, 2}));
  EXPECT_EQ(
      structureOperations(batch),
      (std::vector<std::tuple<int, int, int, int>>{{3, 1, 3, 0}}));
}

TEST(LoomRendererTest, RemovedParentDetachesButDoesNotDeleteMovedChild) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setIdentity(2, 2);
  renderer.setIdentity(3, 3);
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  renderer.captureRemovedMountedEdges({{1, 0}, {3, 3}});
  renderer.createNode(4, "RCTView", sizedProps(100));
  renderer.setIdentity(4, 4);
  renderer.setChildren(4, {3});
  renderer.completeRoot(1, {4});

  EXPECT_EQ(deletedTags(batch), (std::vector<Tag>{2}));
  const auto structure = structureOperations(batch);
  const std::vector<std::tuple<int, int, int, int>> expected{
      {3, 2, 3, 0}, {3, 1, 2, 0}, {2, 1, 4, 0}, {2, 4, 3, 0}};
  EXPECT_EQ(structure, expected);
}

TEST(LoomRendererTest, UncommittedNodeCreationDoesNotAffectLiveTree) {
  std::vector<folly::dynamic> batches;
  auto operations = testOperations();
  operations.applyBatch = [&batches](SurfaceId, LoomMountBatch next) {
    batches.push_back(encodeForTest(next));
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  renderer.createNode(3, "RCTView", sizedProps(30));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  // Speculative draft nodes are owned by the binding, not this live renderer.
  // Until a selected root includes a tag, the persistent node table and native
  // operation stream remain unchanged.
  EXPECT_EQ(batches.size(), 1);
  EXPECT_TRUE(renderer.exists(3));
  EXPECT_FALSE(renderer.exists(4));
}

TEST(LoomRendererTest, UnmountedSpeculativeNodeDoesNotEmitHostDelete) {
  std::vector<LoomMountBatch> batches;
  auto operations = testOperations();
  operations.applyBatch = [&](SurfaceId, LoomMountBatch batch) {
    batches.push_back(std::move(batch));
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(10));
  renderer.createNode(3, "RCTView", sizedProps(20));
  renderer.completeRoot(1, {3});

  ASSERT_EQ(batches.size(), 1);
  for (const auto& operation : batches.front().operations) {
    EXPECT_FALSE(std::holds_alternative<DirectDeleteOperation>(operation));
  }
}

TEST(LoomRendererTest, ReentrantHostUpdateIsCoalescedAfterTheActiveCommit) {
  std::shared_ptr<LoomRenderer> renderer;
  size_t batchCount = 0;
  auto operations = testOperations();
  operations.applyBatch = [&](SurfaceId, LoomMountBatch) {
    batchCount += 1;
    if (batchCount == 1) {
      renderer->setNativeProps(2, folly::dynamic::object("width", 20.0));
    }
  };

  renderer = std::make_shared<LoomRenderer>(1, std::move(operations));
  makeRoot(*renderer);
  renderer->createNode(2, "RCTView", sizedProps(10));
  renderer->completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer->metricsRelativeTo(2, 0, metrics));
  EXPECT_EQ(batchCount, 2);
  EXPECT_FLOAT_EQ(metrics.width, 20);
}

TEST(LoomRendererTest, RootRelativeMetricsAccumulateAllAncestors) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 100.0)("height", 100.0)(
          "marginLeft", 10.0)("marginTop", 11.0));
  renderer.createNode(
      3,
      "RCTView",
      folly::dynamic::object("width", 20.0)("height", 20.0)("marginLeft", 7.0)(
          "marginTop", 9.0));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(3, 0, metrics));
  EXPECT_FLOAT_EQ(metrics.x, 17);
  EXPECT_FLOAT_EQ(metrics.y, 20);
  ASSERT_TRUE(renderer.metricsRelativeTo(3, 2, metrics));
  EXPECT_FLOAT_EQ(metrics.x, 7);
  EXPECT_FLOAT_EQ(metrics.y, 9);
}

TEST(LoomRendererTest, DocumentPositionIncludesRawTextLeaves) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  renderer.createNode(2, "RCTText", sizedProps(100));
  renderer.createRawTextNode(3, "first");
  renderer.createRawTextNode(4, "second");
  renderer.setChildren(2, {3, 4});
  renderer.completeRoot(1, {2});

  constexpr unsigned disconnected = 0x01;
  constexpr unsigned preceding = 0x02;
  constexpr unsigned following = 0x04;
  constexpr unsigned contains = 0x08;
  constexpr unsigned containedBy = 0x10;
  EXPECT_EQ(renderer.compareDocumentPosition(2, 3), containedBy | following);
  EXPECT_EQ(renderer.compareDocumentPosition(3, 2), contains | preceding);
  EXPECT_EQ(renderer.compareDocumentPosition(3, 4), following);
  EXPECT_EQ(renderer.compareDocumentPosition(4, 3), preceding);
  EXPECT_EQ(renderer.compareDocumentPosition(1, 3), containedBy | following);

  // A live handle for a speculative-but-not-connected node is disconnected
  // from the committed document, as is an unknown/deleted tag.
  renderer.createRawTextNode(5, "detached");
  EXPECT_EQ(renderer.compareDocumentPosition(3, 5), disconnected);
  EXPECT_EQ(renderer.compareDocumentPosition(3, 99), disconnected);
}

TEST(
    LoomRendererTest,
    NativeIdAndTextQueriesUseOnlyCommittedConnectedTree) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  auto containerProps = sizedProps(100);
  containerProps["nativeID"] = "content";
  renderer.createNode(2, "RCTView", containerProps);
  renderer.createNode(3, "RCTText", sizedProps(100));
  renderer.createRawTextNode(4, "hello ");
  renderer.createRawTextNode(5, "world");
  renderer.setChildren(3, {4, 5});
  renderer.setChildren(2, {3});
  auto detachedProps = sizedProps(10);
  detachedProps["nativeID"] = "detached";
  renderer.createNode(6, "RCTView", detachedProps);
  renderer.completeRoot(1, {2});

  EXPECT_EQ(renderer.findByNativeId("content"), 2);
  EXPECT_EQ(renderer.findByNativeId("detached"), 0);
  EXPECT_EQ(renderer.findByNativeId("missing"), 0);
  EXPECT_EQ(renderer.textContent(2), "hello world");
  EXPECT_EQ(renderer.textContent(3), "hello world");
  EXPECT_EQ(renderer.textContent(4), "hello ");
  EXPECT_EQ(renderer.textContent(5), "world");
}

TEST(LoomRendererTest, RelativeMetricsFailForNonAncestorAndDetachedNodes) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(80));
  renderer.createNode(3, "RCTView", sizedProps(20));
  renderer.createNode(4, "RCTView", sizedProps(20));
  renderer.setChildren(2, {3, 4});
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  EXPECT_FALSE(renderer.metricsRelativeTo(3, 4, metrics));

  renderer.createNode(5, "RCTView", sizedProps(10));
  EXPECT_FALSE(renderer.metricsRelativeTo(5, 0, metrics));
}

TEST(LoomRendererTest, MetricsComposeAffineTransformsAroundOrigin) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer, 100, 100);
  auto parentProps = sizedProps(40);
  parentProps["transform"] =
      folly::dynamic::array(folly::dynamic::object("scale", 2.0));
  parentProps["transformOrigin"] = folly::dynamic::array("left", "top");
  renderer.createNode(2, "RCTView", parentProps);
  renderer.createNode(
      3,
      "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0)("marginLeft", 10.0)(
          "marginTop", 5.0));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(3, 0, metrics));
  EXPECT_FLOAT_EQ(metrics.x, 20);
  EXPECT_FLOAT_EQ(metrics.y, 10);
  EXPECT_FLOAT_EQ(metrics.width, 20);
  EXPECT_FLOAT_EQ(metrics.height, 20);
}

TEST(LoomRendererTest, UnsupportedPerspectiveMetricsAreReported) {
  Tag reportedTag = 0;
  auto operations = testOperations();
  operations.reportUnsupportedTransform = [&reportedTag](SurfaceId, Tag tag) {
    reportedTag = tag;
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer, 100, 100);
  auto props = sizedProps(40);
  props["transform"] =
      folly::dynamic::array(folly::dynamic::object("perspective", 500.0));
  renderer.createNode(2, "RCTView", props);
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  EXPECT_FALSE(renderer.metricsRelativeTo(2, 0, metrics));
  EXPECT_EQ(reportedTag, 2);
}

TEST(LoomRendererTest, MetricsUseLiveNativeScrollOffsets) {
  auto operations = testOperations();
  operations.getScrollOffset = [](SurfaceId, Tag tag) {
    return tag == 2 ? std::pair<float, float>{7, 9}
                    : std::pair<float, float>{0, 0};
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer, 100, 100);
  renderer.createNode(2, "RCTScrollView", sizedProps(80));
  renderer.createNode(
      3,
      "RCTView",
      folly::dynamic::object("width", 20.0)("height", 20.0)("marginLeft", 30.0)(
          "marginTop", 25.0));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(3, 0, metrics, false, true));
  EXPECT_FLOAT_EQ(metrics.x, 23);
  EXPECT_FLOAT_EQ(metrics.y, 16);
}

TEST(LoomRendererTest, ScrollContentBoundsUseLaidOutContentChild) {
  Rect reportedBounds{};
  int scrollBoundsUpdates = 0;
  auto operations = testOperations();
  operations.updateScrollContentBounds =
      [&reportedBounds, &scrollBoundsUpdates](
          SurfaceId, Tag tag, const folly::dynamic&, const Rect& bounds) {
        ASSERT_EQ(tag, 2);
        reportedBounds = bounds;
        ++scrollBoundsUpdates;
      };

  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer, 100, 100);
  renderer.createNode(2, "RCTScrollView", sizedProps(80));
  renderer.createNode(
      3,
      "RCTView",
      folly::dynamic::object("width", 80.0)("height", 600.0)(
          "collapsable", false));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_EQ(scrollBoundsUpdates, 1);
  EXPECT_FLOAT_EQ(reportedBounds.origin.x, 0);
  EXPECT_FLOAT_EQ(reportedBounds.origin.y, 0);
  EXPECT_FLOAT_EQ(reportedBounds.size.width, 80);
  EXPECT_FLOAT_EQ(reportedBounds.size.height, 600);
}

TEST(LoomRendererTest, GapStylesReachYogaLayout) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer, 100, 40);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 100.0)("height", 40.0)(
          "flexDirection", "row")("gap", 5.0));
  renderer.createNode(3, "RCTView", sizedProps(10));
  renderer.createNode(4, "RCTView", sizedProps(10));
  renderer.setChildren(2, {3, 4});
  renderer.completeRoot(1, {2});

  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(4, 2, metrics));
  EXPECT_FLOAT_EQ(metrics.x, 15);
}

TEST(LoomRendererTest, HitTestingDoesNotTargetAnAncestorOutsideItsBounds) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer, 100, 100);
  renderer.createNode(2, "RCTView", sizedProps(20));
  renderer.completeRoot(1, {2});

  EXPECT_EQ(renderer.findNodeAtPoint(1, 10, 10), 2);
  EXPECT_EQ(renderer.findNodeAtPoint(1, 150, 10), 0);
}

TEST(LoomRendererTest, BoxOnlyDoesNotCaptureOutsideItsBounds) {
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer, 100, 100);
  renderer.createNode(
      2,
      "RCTView",
      folly::dynamic::object("width", 20.0)("height", 20.0)(
          "pointerEvents", "box-only"));
  renderer.createNode(3, "RCTView", sizedProps(10));
  renderer.setChildren(2, {3});
  renderer.completeRoot(1, {2});

  EXPECT_EQ(renderer.findNodeAtPoint(1, 10, 10), 2);
  EXPECT_EQ(renderer.findNodeAtPoint(1, 50, 10), 0);
}

TEST(LoomRendererTest, WideChildReorderEmitsOrderedMoves) {
  std::vector<folly::dynamic> batches;
  auto operations = testOperations();
  operations.applyBatch = [&batches](
                              SurfaceId, LoomMountBatch batch) {
    batches.push_back(encodeForTest(batch));
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);
  renderer.createNode(2, "RCTView", sizedProps(100));
  for (Tag tag = 3; tag <= 6; ++tag) {
    renderer.createNode(tag, "RCTView", sizedProps(10));
  }
  renderer.setChildren(2, {3, 4, 5, 6});
  renderer.completeRoot(1, {2});
  ASSERT_EQ(batches.size(), 1);

  batches.clear();
  renderer.setChildren(2, {5, 6, 3, 4});
  renderer.completeRoot(1, {2});
  ASSERT_EQ(batches.size(), 1);
  const std::vector<std::tuple<int, int, int, int>> expected{
      {3, 2, 4, 1}, {3, 2, 3, 0}, {2, 2, 3, 2}, {2, 2, 4, 3}};
  EXPECT_EQ(structureOperations(batches.back()), expected);

  batches.clear();
  renderer.completeRoot(1, {2});
  EXPECT_TRUE(batches.empty());
}

TEST(LoomRendererTest, WorkerPreparesTextAndLayoutAndAppliesViewsOnUI) {
  const auto ui = std::this_thread::get_id();
  LoomWorker worker([] {});
  std::vector<LoomMountBatch> batches;
  auto operations = testOperations();
  operations.applyBatch =
      worker.defer(std::function<void(SurfaceId, LoomMountBatch)>(
          [&](SurfaceId, LoomMountBatch batch) {
            EXPECT_EQ(std::this_thread::get_id(), ui);
            batches.push_back(std::move(batch));
          }));
  operations.measureText = [ui](
                               SurfaceId,
                               const AttributedString&,
                               const ParagraphAttributes&,
                               const Size&,
                               const Size&) {
    EXPECT_NE(std::this_thread::get_id(), ui);
    return LoomRenderer::TextMeasurement{
        .width = 50, .height = 20, .valid = true};
  };
  LoomRenderer renderer(1, std::move(operations));
  auto job = std::make_unique<LoomWorker::Job>();
  job->compute = [&] {
    makeRoot(renderer);
    renderer.createNode(2, "RCTText", folly::dynamic::object());
    renderer.createRawTextNode(3, "worker text");
    renderer.setChildren(2, {3});
    renderer.completeRoot(1, {2});
  };
  worker.submit(std::move(job));
  worker.synchronize();
  ASSERT_EQ(batches.size(), 1);
  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(2, 1, metrics));
  EXPECT_FLOAT_EQ(metrics.height, 20);
}

TEST(
    LoomRendererTest,
    WorkerNativePropsDeltaSurvivesFollowingReactCommitAndSyncBorrow) {
  LoomWorker worker([] {});
  std::vector<folly::dynamic> batches;
  auto operations = testOperations();
  operations.applyBatch =
      worker.defer(std::function<void(SurfaceId, LoomMountBatch)>(
          [&](SurfaceId, LoomMountBatch batch) {
            batches.push_back(encodeForTest(batch));
          }));
  LoomRenderer renderer(1, std::move(operations));
  auto submit = [&](std::function<void()> compute, bool sync = false) {
    auto job = std::make_unique<LoomWorker::Job>();
    job->compute = std::move(compute);
    job->synchronous = sync;
    worker.submit(std::move(job));
  };
  submit([&] {
    makeRoot(renderer);
    renderer.createNode(2, "RCTView", sizedProps(100));
    renderer.completeRoot(1, {2});
  });
  submit([&] {
    renderer.setNativeProps(
        2, folly::dynamic::object("width", 75)("opacity", 0.5));
  });
  submit(
      [&] {
        renderer.cloneNodeWithNewProps(
            2, folly::dynamic::object("backgroundColor", 123));
        renderer.completeRoot(1, {2});
      },
      true);
  worker.synchronize();
  LoomRenderer::Metrics metrics;
  ASSERT_TRUE(renderer.metricsRelativeTo(2, 1, metrics));
  EXPECT_FLOAT_EQ(metrics.width, 75);
  EXPECT_EQ(renderer.numericProp(2, "opacity"), 0.5);
  EXPECT_EQ(batches.size(), 3);
}

/**
 * Drives the binding's reconciler directly. It is the same code both production
 * paths run - the worker reconciler and the synchronous one differ only in who
 * calls it and whether it compacts - so these tests cover the real commit path
 * rather than the draft API the older tests exercise.
 */
struct LoomBindingTestAccess {
  using Binding = LoomBinding;
  using Draft = Binding::DraftNode;
  using Token = Binding::SurfaceToken;

  static std::shared_ptr<Token> token(SurfaceId surfaceId, uint64_t generation) {
    auto token = std::make_shared<Token>();
    token->surfaceId = surfaceId;
    token->generation = generation;
    return token;
  }

  /** Mirrors the host `createNode` draft exactly. */
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
    node->componentName = std::make_shared<const std::string>(std::move(componentName));
    node->initialProps =
        std::make_shared<const folly::dynamic>(std::move(props));
    node->hasInitialProps = true;
    node->roles = LoomPlatformTraits::current().components.classify(
        *node->componentName);
    return node;
  }

  /** The token the binding already holds for a surface, or a fresh one. */
  static std::shared_ptr<Token> tokenFor(
      Binding& binding,
      SurfaceId surfaceId) {
    return binding.tokenFor(surfaceId);
  }

  static std::shared_ptr<Binding::NodeRef> nodeRef(
      SurfaceId surfaceId,
      Tag tag,
      std::shared_ptr<Token> token,
      std::shared_ptr<Draft> draft) {
    return std::make_shared<Binding::NodeRef>(
        surfaceId, tag, std::move(token), std::move(draft));
  }

  /** The host's clone step (`cloneNode*`, and `appendChild` on a published parent). */
  static std::shared_ptr<Draft> clone(
      Binding& binding,
      const std::shared_ptr<Binding::NodeRef>& source,
      bool replaceChildren = false) {
    return binding.cloneDraft(source, replaceChildren);
  }

  /** Drives the publication path, which the host API normally reaches. */
  static void publish(
      Binding& binding,
      SurfaceId surfaceId,
      const std::vector<std::shared_ptr<Binding::NodeRef>>& children,
      bool forceSync = false) {
    binding.publishWorkerCommit(surfaceId, children, forceSync);
  }

  static size_t submittedIdentityCount(
      const Binding& binding,
      SurfaceId surface = 1) {
    const auto found = binding.submittedIdentities_.find(surface);
    return found == binding.submittedIdentities_.end() ? 0
                                                      : found->second.size();
  }

  static uint64_t submittedIdentity(
      const Binding& binding,
      Tag tag,
      SurfaceId surface = 1) {
    const auto found = binding.submittedIdentities_.find(surface);
    if (found == binding.submittedIdentities_.end())
      return 0;
    const auto entry = found->second.find(tag);
    return entry == found->second.end() ? 0 : entry->second;
  }

  /** Drops a submitted entry, to observe whether a later walk re-inserts it. */
  static void clearSubmittedIdentity(
      Binding& binding,
      Tag tag,
      SurfaceId surface = 1) {
    if (auto found = binding.submittedIdentities_.find(surface);
        found != binding.submittedIdentities_.end())
      found->second.erase(tag);
  }

  static size_t mountedCount(const Binding& binding) {
    return binding.mountedIdentities_.size();
  }

  static uint64_t mountedIdentity(const Binding& binding, Tag tag) {
    const auto found = binding.mountedIdentities_.find(tag);
    return found == binding.mountedIdentities_.end() ? 0 : found->second;
  }

  static std::shared_ptr<Token> mountedToken(const Binding& binding, Tag tag) {
    const auto found = binding.mountedTokens_.find(tag);
    return found == binding.mountedTokens_.end() ? nullptr : found->second;
  }

  /** Drives the reconcile the commit result normally performs on UI. */
  static void applyPublishedIdentities(
      Binding& binding,
      std::shared_ptr<Token> token,
      const TagMap<uint64_t>& identities,
      const std::vector<std::shared_ptr<Draft>>& committedDrafts = {}) {
    binding.applyPublishedIdentities(token, identities, committedDrafts);
  }

  /** Appends a pending props patch, as `cloneNodeWithNewProps` does. */
  static void patch(const std::shared_ptr<Draft>& node, folly::dynamic props) {
    auto next = std::make_shared<Binding::PropsPatch>();
    next->previous = node->propsTail;
    next->props = std::move(props);
    node->propsTail = std::move(next);
  }

  static Binding::CommitSelection select(
      LoomRenderer& renderer,
      SurfaceId surfaceId,
      const Token& token,
      const std::vector<std::shared_ptr<Draft>>& roots) {
    return Binding::selectCommitDrafts(renderer, surfaceId, token, roots);
  }

  static void reconcile(
      LoomRenderer& renderer,
      SurfaceId surfaceId,
      const Binding::CommitSelection& selection,
      bool compact) {
    Binding::reconcileSelectedDrafts(renderer, surfaceId, selection, compact);
  }
};

/**
 * P1 evidence, not an assertion. A steady-state commit usually changes one prop,
 * and the complaint is that its *fixed* per-commit cost is O(tree). This measures
 * that fixed cost in isolation: the tree is committed unchanged, so every pass
 * (`prepare`, emission, child order, overflow, layout) has nothing to emit and
 * only its traversal and per-node guards are paid. It prints rather than asserts
 * because a wall-clock bound in a unit suite is flaky by construction; the number
 * is what tells us which per-node work is worth removing, and it is compared
 * across builds rather than against a threshold.
 */
TEST(LoomRendererTest, SteadyStateCommitCostProbe) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer, 1000, 1000);

  constexpr Tag kGroups = 20;
  constexpr Tag kLeavesPerGroup = 20;
  std::vector<Tag> groups;
  groups.reserve(kGroups);
  Tag nextTag = 2;
  for (Tag group = 0; group < kGroups; ++group) {
    const auto groupTag = nextTag++;
    renderer.createNode(groupTag, "RCTView", sizedProps(100));
    std::vector<Tag> leaves;
    leaves.reserve(kLeavesPerGroup);
    for (Tag leaf = 0; leaf < kLeavesPerGroup; ++leaf) {
      const auto leafTag = nextTag++;
      renderer.createNode(leafTag, "RCTView", sizedProps(10));
      leaves.push_back(leafTag);
    }
    renderer.setChildren(groupTag, leaves);
    groups.push_back(groupTag);
  }

  // Warm commit: creates every host view and lays the tree out once.
  renderer.completeRoot(1, groups);
  ASSERT_FALSE(renderer.hasFailed());

  // Report the fastest iteration, not the average: the minimum is the sample
  // least disturbed by scheduler and thermal noise, and across invocations it is
  // the stable statistic. The mean is printed too, because a change that helps
  // the minimum but not the mean is not a real win.
  constexpr int kIterations = 500;
  batch = folly::dynamic::array();
  long long bestMicros = std::numeric_limits<long long>::max();
  long long totalMicros = 0;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    renderer.completeRoot(1, groups);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    bestMicros = std::min(bestMicros, elapsed);
    totalMicros += elapsed;
  }

  // An unchanged tree must emit nothing, so the measurement really is fixed cost
  // and not the cost of one prop's operations.
  EXPECT_TRUE(layoutOperations(batch).empty());
  EXPECT_TRUE(structureOperations(batch).empty());

  const auto nodeCount = static_cast<size_t>(nextTag - 1);
  printf(
      "[probe] steady-state commit, %zu nodes: min=%lld us mean=%lld us\n",
      nodeCount,
      bestMicros,
      totalMicros / kIterations);
}

/**
 * The same P1 measurement on a shape the wide probe is blind to.
 *
 * `emitLayout` walks from a node's tree parent up to its *layout* parent
 * (`LoomRenderer.cpp`, the `while (ancestorTag != parent.tag)` loop), and
 * that loop only iterates when the two differ - which is exactly a chain of
 * flattened (layout-only) or virtual nodes. The wide probe sets
 * `collapsable: false` on every node, so its walk never iterates and its number
 * says nothing about this cost. Here every node sits under a chain of collapsable
 * Views, which is the production default, so the review's "O(n*depth)" claim is
 * measurable: per node the walk is proportional to its distance from its layout
 * parent.
 */
TEST(LoomRendererTest, SteadyStateCommitCostProbeNestedLayoutOnlyChains) {
  folly::dynamic batch = folly::dynamic::array();
  auto operations = testOperations();
  operations.applyBatch = [&batch](SurfaceId, LoomMountBatch next) {
    batch = encodeForTest(next);
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer, 1000, 2000);

  constexpr Tag kChains = 20;
  constexpr int kDepth = 19;
  std::vector<Tag> chainRoots;
  chainRoots.reserve(kChains);
  Tag nextTag = 2;
  for (Tag chain = 0; chain < kChains; ++chain) {
    std::vector<Tag> chainTags;
    chainTags.reserve(kDepth);
    for (int depth = 0; depth < kDepth; ++depth) {
      chainTags.push_back(nextTag++);
    }
    for (const Tag tag : chainTags) {
      // No `collapsable: false`, and only style props, so these are layout-only
      // and the host never sees them. That is the production default.
      renderer.createNode(
          tag,
          "RCTView",
          folly::dynamic::object("width", 100.0)("height", 20.0));
    }
    for (size_t index = 1; index < chainTags.size(); ++index) {
      renderer.setChildren(chainTags[index - 1], {chainTags[index]});
    }
    // One materialized leaf so each chain ends in something the host mounts.
    const auto leafTag = nextTag++;
    renderer.createNode(leafTag, "RCTView", sizedProps(10));
    renderer.setChildren(chainTags.back(), {leafTag});
    chainRoots.push_back(chainTags.front());
  }

  renderer.completeRoot(1, chainRoots);
  ASSERT_FALSE(renderer.hasFailed());

  constexpr int kIterations = 500;
  batch = folly::dynamic::array();
  long long bestMicros = std::numeric_limits<long long>::max();
  long long totalMicros = 0;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    renderer.completeRoot(1, chainRoots);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    bestMicros = std::min(bestMicros, elapsed);
    totalMicros += elapsed;
  }

  EXPECT_TRUE(layoutOperations(batch).empty());

  const auto nodeCount = static_cast<size_t>(nextTag - 1);
  printf(
      "[probe] steady-state commit, %zu nodes in %d-deep layout-only chains: "
      "min=%lld us mean=%lld us\n",
      nodeCount,
      kDepth,
      bestMicros,
      totalMicros / kIterations);
}

/**
 * Covers the publication path, which the host API normally reaches and the rest
 * of the suite therefore never touches: `freeze`, its stamp dedup, and the
 * submitted bookkeeping it feeds. It needs a real `UIManager` because the
 * binding reads its host from the UIManager's context container, and a host
 * that returns a UI executor.
 */
class LoomRendererPublishTest : public ::testing::Test {
 protected:
  void SetUp() override {
    contextContainer_ = std::make_shared<ContextContainer>();
    LoomHost::registerIn(*contextContainer_, std::make_shared<TestLoomHost>());
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
    auto manager =
        std::make_shared<UIManager>(runtimeExecutor, contextContainer_);
    manager->setComponentDescriptorRegistry(std::move(registry));
    manager->setDelegate(&delegate_);
    uiManager_ = std::move(manager);
    binding_ = std::make_shared<LoomBinding>(uiManager_);
  }

  void TearDown() override {
    binding_.reset();
    uiManager_->setDelegate(nullptr);
  }

  std::shared_ptr<ContextContainer> contextContainer_;
  std::shared_ptr<UIManager> uiManager_;
  std::shared_ptr<LoomBinding> binding_;
  TestUIManagerDelegate delegate_;
};

TEST_F(LoomRendererPublishTest, PublicationRecordsIdentitiesForEveryDraft) {
  const auto token = LoomBindingTestAccess::tokenFor(*binding_, 1);
  auto root = LoomBindingTestAccess::draft(
      1, token->generation, 11, 2, "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0));
  auto child = LoomBindingTestAccess::draft(
      1, token->generation, 12, 3, "RCTView",
      folly::dynamic::object("width", 5.0)("height", 5.0));
  root->children->push_back(child);

  LoomBindingTestAccess::publish(
      *binding_,
      1,
      {LoomBindingTestAccess::nodeRef(1, 2, token, root)});

  // Both drafts were frozen into the commit. (Their submitted entries are
  // dropped again once the commit is mounted, so the drafts are what to check.)
  EXPECT_TRUE(root->published);
  EXPECT_TRUE(child->published);
}

TEST_F(LoomRendererPublishTest, RepublishingAnUnchangedGraphSkipsTheFreezeWalk) {
  using Access = LoomBindingTestAccess;
  const auto token = Access::tokenFor(*binding_, 1);
  auto root = Access::draft(
      1, token->generation, 10, 1, "Root", folly::dynamic::object());
  auto child = Access::draft(
      1,
      token->generation,
      11,
      2,
      "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0));
  root->children = std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>(
      std::vector<std::shared_ptr<Access::Draft>>{child});
  const auto ref = [&] { return Access::nodeRef(1, 1, token, root); };

  Access::publish(*binding_, 1, {ref()});
  EXPECT_EQ(Access::submittedIdentity(*binding_, 2), 11u);

  // Prove the second publish skipped the unchanged subtree rather than walking it
  // again: an entry removed by hand can only stay removed if nothing re-inserted
  // it. Freeze re-walking the whole graph on every commit is the cost this
  // removes, so a regression here is exactly that cost coming back.
  Access::clearSubmittedIdentity(*binding_, 2);
  Access::publish(*binding_, 1, {ref()});
  EXPECT_EQ(Access::submittedIdentity(*binding_, 2), 0u)
      << "an unchanged subtree must be skipped, not re-walked";
}

TEST_F(LoomRendererPublishTest, ACloneOfAPublishedVersionIsPublishedAgain) {
  using Access = LoomBindingTestAccess;
  const auto token = Access::tokenFor(*binding_, 1);
  auto root = Access::draft(
      1, token->generation, 10, 1, "Root", folly::dynamic::object());
  auto child = Access::draft(
      1,
      token->generation,
      11,
      2,
      "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0));
  root->children = std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>(
      std::vector<std::shared_ptr<Access::Draft>>{child});
  Access::publish(*binding_, 1, {Access::nodeRef(1, 1, token, root)});
  Access::applyPublishedIdentities(
      *binding_, token, {{1, 10u}, {2, 11u}}, {root, child});
  ASSERT_TRUE(root->published);
  ASSERT_TRUE(root->workerCommitted);

  // React's next render clones the committed root with a new child list. The
  // clone is a new version: if it inherited the source's publication state,
  // freeze would skip it as unchanged and selection would read its children
  // back from the live tree, so the new child would never be mounted and every
  // commit after the first would be a no-op.
  auto next = Access::clone(*binding_, Access::nodeRef(1, 1, token, root), true);
  EXPECT_FALSE(next->published);
  EXPECT_FALSE(next->workerCommitted);
  auto added = Access::draft(
      1,
      token->generation,
      12,
      3,
      "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0));
  next->children->push_back(child);
  next->children->push_back(added);
  Access::publish(*binding_, 1, {Access::nodeRef(1, 1, token, next)});

  EXPECT_TRUE(next->published);
  EXPECT_TRUE(added->published)
      << "the clone's new child must be frozen into the commit";
}

TEST_F(LoomRendererPublishTest, TextEffectsScanIsCachedPerDraftVersion) {
  using Access = LoomBindingTestAccess;
  const auto token = Access::tokenFor(*binding_, 1);
  auto root = Access::draft(
      1,
      token->generation,
      11,
      2,
      "RCTView",
      folly::dynamic::object("textEffects", folly::dynamic::object())(
          "width", 10.0));
  Access::publish(*binding_, 1, {Access::nodeRef(1, 2, token, root)});

  // The scan is a full recursive walk of the props, and the worker path never
  // drops `initialProps` (it does not compact), so without the cache this walk
  // repeats for every unchanged node on every commit. A published version is
  // immutable, so the answer is computed once and reused; this pins that it is
  // computed and correct, which is the failure mode that would silently make
  // every commit rescan.
  EXPECT_TRUE(root->initialPropsTextScanned);
  EXPECT_TRUE(root->initialPropsHasTextEffects);
}

TEST_F(LoomRendererPublishTest, ApplyMarksThePublishedVersionsCommitted) {
  using Access = LoomBindingTestAccess;
  const auto token = Access::tokenFor(*binding_, 1);
  auto root = Access::draft(
      1, token->generation, 10, 1, "Root", folly::dynamic::object());
  auto child = Access::draft(
      1,
      token->generation,
      11,
      2,
      "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0));

  EXPECT_FALSE(root->workerCommitted);
  EXPECT_FALSE(child->workerCommitted);

  // This is the callback the commit result runs once the batch has landed, so the
  // live renderer is known to hold these versions. Deliberately *not* set while
  // freezing: a publish can be in flight with the live tree still on an older
  // revision, and treating that as committed would let a later reconcile skip a
  // subtree the renderer has not actually reached.
  Access::applyPublishedIdentities(
      *binding_, token, {{1, 10u}, {2, 11u}}, {root, child});

  EXPECT_TRUE(root->workerCommitted);
  EXPECT_TRUE(child->workerCommitted);
}

TEST_F(LoomRendererPublishTest, MountedBookkeepingReconcilesAsADelta) {
  using Access = LoomBindingTestAccess;
  const auto surfaceOne = Access::token(1, 7);
  const auto surfaceTwo = Access::token(2, 7);

  Access::applyPublishedIdentities(*binding_, surfaceOne, {{2, 11u}, {3, 12u}});
  Access::applyPublishedIdentities(*binding_, surfaceTwo, {{9, 90u}});
  EXPECT_EQ(Access::mountedCount(*binding_), 3);

  // A later commit on surface 1 that keeps 2 and drops 3. This reconcile is the
  // only thing that discovers removals, so absence has to erase.
  Access::applyPublishedIdentities(*binding_, surfaceOne, {{2, 11u}, {4, 14u}});
  EXPECT_EQ(Access::mountedIdentity(*binding_, 3), 0u);
  EXPECT_EQ(Access::mountedIdentity(*binding_, 2), 11u);
  EXPECT_EQ(Access::mountedIdentity(*binding_, 4), 14u);
  EXPECT_EQ(Access::mountedIdentity(*binding_, 9), 90u)
      << "another surface's entries must be left alone";

  // The same tag under a new token must be re-pointed even though its identity
  // is unchanged, so the "already agrees" shortcut cannot ignore the token.
  const auto replacement = Access::token(1, 8);
  Access::applyPublishedIdentities(*binding_, replacement, {{2, 11u}});
  EXPECT_EQ(Access::mountedToken(*binding_, 2), replacement);
  EXPECT_EQ(Access::mountedIdentity(*binding_, 4), 0u);
  EXPECT_EQ(Access::mountedCount(*binding_), 2);

  // Idempotent: a synchronous commit publishes on the worker and is then
  // reported on UI, so one commit runs this twice.
  Access::applyPublishedIdentities(*binding_, replacement, {{2, 11u}});
  EXPECT_EQ(Access::mountedCount(*binding_), 2);
  EXPECT_EQ(Access::mountedIdentity(*binding_, 2), 11u);
}

TEST_F(LoomRendererPublishTest, FreezeVisitsASharedDraftOnlyOnce) {
  // The stamp replaces a visited set, so a version reachable from two parents
  // must be processed once. A repeated tag would corrupt the identity map.
  const auto token = LoomBindingTestAccess::tokenFor(*binding_, 1);
  auto shared = LoomBindingTestAccess::draft(
      1, token->generation, 21, 4, "RCTView",
      folly::dynamic::object("width", 7.0)("height", 7.0));
  auto first = LoomBindingTestAccess::draft(
      1, token->generation, 22, 2, "RCTView", folly::dynamic::object());
  auto second = LoomBindingTestAccess::draft(
      1, token->generation, 23, 3, "RCTView", folly::dynamic::object());
  first->children->push_back(shared);
  second->children->push_back(shared);

  LoomBindingTestAccess::publish(
      *binding_,
      1,
      {LoomBindingTestAccess::nodeRef(1, 2, token, first),
       LoomBindingTestAccess::nodeRef(1, 3, token, second)});

  EXPECT_EQ(
      LoomBindingTestAccess::submittedIdentity(*binding_, 4), 21u);
}


TEST(LoomRendererTest, CommittedSubtreeIsNotReselected) {
  using Access = LoomBindingTestAccess;
  auto operations = testOperations();
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);

  const auto token = Access::token(1, 7);
  auto parent = Access::draft(
      1, 7, 11, 2, "RCTView",
      folly::dynamic::object("width", 50.0)("height", 20.0)("collapsable", false));
  auto child = Access::draft(
      1, 7, 12, 3, "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0)("collapsable", false));
  parent->children->push_back(child);

  // First commit: nothing is committed yet, so the whole graph is selected and
  // then applied.
  auto first = Access::select(renderer, 1, *token, {parent});
  ASSERT_EQ(first.selected.size(), 2u);
  Access::reconcile(renderer, 1, first, false);

  // What an applied commit leaves behind: both versions committed, with the live
  // renderer holding them. The worker path never compacts, so its drafts keep
  // their children, and the flag is what says the subtree needs no second look.
  parent->workerCommitted = true;
  child->workerCommitted = true;

  auto second = Access::select(renderer, 1, *token, {parent});
  EXPECT_EQ(second.selected.size(), 1u)
      << "a committed, unchanged subtree must not be walked again";
}

TEST(LoomRendererTest, ReconcilerCreatesSelectsAndAppliesDrafts) {
  // The merged reconciler, driven end to end: selection, creation, patch-free
  // first commit and the root commit, all against a live renderer.
  auto operations = testOperations();
  std::vector<LoomMountBatch> batches;
  operations.applyBatch = [&batches](SurfaceId, LoomMountBatch batch) {
    batches.push_back(std::move(batch));
  };
  LoomRenderer renderer(1, std::move(operations));
  makeRoot(renderer);

  const auto token = LoomBindingTestAccess::token(1, 7);
  auto parent = LoomBindingTestAccess::draft(
      1, 7, 11, 2, "RCTView",
      folly::dynamic::object("width", 50.0)("height", 20.0)("collapsable", false));
  auto child = LoomBindingTestAccess::draft(
      1, 7, 12, 3, "RCTView",
      folly::dynamic::object("width", 10.0)("height", 10.0)(
          "collapsable", false));
  parent->children->push_back(child);

  auto selection = LoomBindingTestAccess::select(
      renderer, 1, *token, {parent});
  ASSERT_EQ(selection.selected.size(), 2u);
  EXPECT_EQ(selection.rootChildren, (std::vector<Tag>{2}));
  EXPECT_EQ(selection.rootTag, 1u);
  EXPECT_EQ(selection.unsupportedAttachmentTag, 0u);
  // The root plus both drafts carry an identity for removed-edge capture.
  EXPECT_EQ(selection.selectedIdentities.size(), 3u);

  LoomBindingTestAccess::reconcile(renderer, 1, selection, false);

  EXPECT_TRUE(renderer.exists(2));
  EXPECT_TRUE(renderer.exists(3));
  EXPECT_EQ(renderer.identityOf(2), 11u);
  EXPECT_EQ(renderer.identityOf(3), 12u);
  EXPECT_EQ(renderer.childrenOf(2), (std::vector<Tag>{3}));
  EXPECT_EQ(renderer.childrenOf(1), (std::vector<Tag>{2}));
  ASSERT_FALSE(batches.empty());
}

TEST(LoomRendererTest, ReconcilerCompactsOnlyWhenAsked) {
  // `compact` is the one real difference between the two production paths: the
  // synchronous path owns the renderer and drops the applied history, the worker
  // path leaves the drafts to the host. Both must apply the patch first.
  const auto runCommit = [](bool compact) {
    LoomRenderer renderer(1, testOperations());
    makeRoot(renderer);
    const auto token = LoomBindingTestAccess::token(1, 7);
    auto node = LoomBindingTestAccess::draft(
        1, 7, 21, 2, "RCTView",
        folly::dynamic::object("width", 40.0)("height", 10.0)(
            "collapsable", false));
    // A pending patch is what the two modes disagree about: both must apply it,
    // only the compacting path may then drop it.
    LoomBindingTestAccess::patch(
        node, folly::dynamic::object("backgroundColor", "red"));
    auto selection = LoomBindingTestAccess::select(
        renderer, 1, *token, {node});
    // Owned here: the compacting path drops the draft's reference to it.
    const auto pending = node->propsTail;
    LoomBindingTestAccess::reconcile(renderer, 1, selection, compact);
    return std::make_tuple(
        renderer.identityOf(2),
        node->propsTail != nullptr,
        pending->applied.load());
  };

  const auto [compactIdentity, compactTail, compactApplied] = runCommit(true);
  EXPECT_EQ(compactIdentity, 21u);
  EXPECT_TRUE(compactApplied) << "the patch must be applied before it is dropped";
  EXPECT_FALSE(compactTail) << "the compacting path must drop the applied history";

  const auto [workerIdentity, workerTail, workerApplied] = runCommit(false);
  EXPECT_EQ(workerIdentity, 21u);
  EXPECT_TRUE(workerApplied) << "the patch must be applied on the worker path too";
  EXPECT_TRUE(workerTail)
      << "the worker path must keep the draft chain for its host";
}

TEST(LoomRendererTest, ReconcilerRejectsAStaleDraft) {
  // The reconciler is the only thing standing between a stale host draft and the
  // live tree, so its rejection path is worth pinning down.
  LoomRenderer renderer(1, testOperations());
  makeRoot(renderer);
  const auto token = LoomBindingTestAccess::token(1, 7);
  auto stale = LoomBindingTestAccess::draft(
      1, 6, 31, 2, "RCTView", folly::dynamic::object("width", 10.0));
  EXPECT_THROW(
      LoomBindingTestAccess::select(renderer, 1, *token, {stale}),
      std::logic_error);
}

} // namespace facebook::react
