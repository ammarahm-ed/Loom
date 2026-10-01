/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <folly/dynamic.h>
#include <react/renderer/core/ReactPrimitives.h>

namespace facebook::react {

enum class DirectMountOpcode : int {
  Create = 0,
  UpdateProps = 1,
  Insert = 2,
  Remove = 3,
  Layout = 4,
  Delete = 5,
  Padding = 6,
  OverflowInset = 7,
};

static_assert(static_cast<int>(DirectMountOpcode::Create) == 0);
static_assert(static_cast<int>(DirectMountOpcode::UpdateProps) == 1);
static_assert(static_cast<int>(DirectMountOpcode::Insert) == 2);
static_assert(static_cast<int>(DirectMountOpcode::Remove) == 3);
static_assert(static_cast<int>(DirectMountOpcode::Layout) == 4);
static_assert(static_cast<int>(DirectMountOpcode::Delete) == 5);
static_assert(static_cast<int>(DirectMountOpcode::Padding) == 6);
static_assert(static_cast<int>(DirectMountOpcode::OverflowInset) == 7);

struct DirectCreateOperation {
  Tag tag;
  bool layoutable;
  std::string componentName;
  folly::dynamic props;
};

struct DirectUpdatePropsOperation {
  Tag tag;
  folly::dynamic props;
};

struct DirectInsertOperation {
  Tag parentTag;
  Tag tag;
  size_t index;
};

struct DirectRemoveOperation {
  Tag parentTag;
  Tag tag;
  size_t index;
};

struct DirectLayoutOperation {
  Tag tag;
  Tag parentTag;
  int displayType;
  int layoutDirection;
  float x;
  float y;
  float width;
  float height;
};

struct DirectDeleteOperation {
  Tag tag;
};

struct DirectPaddingOperation {
  Tag tag;
  std::array<float, 4> insets;
};

struct DirectOverflowInsetOperation {
  Tag tag;
  std::array<float, 4> insets;
};

using DirectMountOperation = std::variant<
    DirectCreateOperation,
    DirectUpdatePropsOperation,
    DirectInsertOperation,
    DirectRemoveOperation,
    DirectLayoutOperation,
    DirectDeleteOperation,
    DirectPaddingOperation,
    DirectOverflowInsetOperation>;

/** Typed, ordered operations for one direct renderer commit. */
struct UIThreadDirectMountBatch {
  enum Phase : size_t {
    CreateAndProps = 0,
    Removes = 1,
    Inserts = 2,
    Padding = 3,
    Layout = 4,
    Deletes = 5,
    PhaseCount = 6,
  };

  std::vector<DirectMountOperation> operations;
  std::array<std::vector<size_t>, PhaseCount> phaseOperationIndexes;
  size_t instructionCount{0};
  size_t objectCount{0};
  bool requiresTagReplacement{false};

  void append(DirectMountOperation operation) {
    const auto operationIndex = operations.size();
    const auto phase = std::visit(
        [](const auto& typed) -> size_t {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, DirectRemoveOperation>) {
            return Removes;
          } else if constexpr (std::is_same_v<T, DirectInsertOperation>) {
            return Inserts;
          } else if constexpr (
              std::is_same_v<T, DirectPaddingOperation> ||
              std::is_same_v<T, DirectOverflowInsetOperation>) {
            return Padding;
          } else if constexpr (std::is_same_v<T, DirectLayoutOperation>) {
            return Layout;
          } else if constexpr (std::is_same_v<T, DirectDeleteOperation>) {
            return Deletes;
          } else {
            return CreateAndProps;
          }
        },
        operation);
    std::visit(
        [this](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, DirectCreateOperation>) {
            instructionCount += 3;
            objectCount += 2;
          } else if constexpr (std::is_same_v<T, DirectUpdatePropsOperation>) {
            instructionCount += 2;
            objectCount += 1;
          } else if constexpr (
              std::is_same_v<T, DirectInsertOperation> ||
              std::is_same_v<T, DirectRemoveOperation>) {
            instructionCount += 4;
          } else if constexpr (std::is_same_v<T, DirectLayoutOperation>) {
            instructionCount += 9;
          } else if constexpr (std::is_same_v<T, DirectDeleteOperation>) {
            instructionCount += 2;
          } else {
            instructionCount += 6;
          }
        },
        operation);
    operations.push_back(std::move(operation));
    phaseOperationIndexes[phase].push_back(operationIndex);
  }

  void clear() {
    operations.clear();
    for (auto& phase : phaseOperationIndexes)
      phase.clear();
    instructionCount = 0;
    objectCount = 0;
    requiresTagReplacement = false;
  }
};

} // namespace facebook::react
