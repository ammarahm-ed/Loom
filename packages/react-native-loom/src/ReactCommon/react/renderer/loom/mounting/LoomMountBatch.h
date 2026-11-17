/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <folly/dynamic.h>
#include <react/renderer/core/Props.h>
#include <react/renderer/core/ReactPrimitives.h>

namespace facebook::react {

enum class LoomOpcode : int {
  Create = 0,
  UpdateProps = 1,
  Insert = 2,
  Remove = 3,
  Layout = 4,
  Delete = 5,
  Padding = 6,
  OverflowInset = 7,
};

static_assert(static_cast<int>(LoomOpcode::Create) == 0);
static_assert(static_cast<int>(LoomOpcode::UpdateProps) == 1);
static_assert(static_cast<int>(LoomOpcode::Insert) == 2);
static_assert(static_cast<int>(LoomOpcode::Remove) == 3);
static_assert(static_cast<int>(LoomOpcode::Layout) == 4);
static_assert(static_cast<int>(LoomOpcode::Delete) == 5);
static_assert(static_cast<int>(LoomOpcode::Padding) == 6);
static_assert(static_cast<int>(LoomOpcode::OverflowInset) == 7);

/**
 * Merged props, shareable with the immutable object they came from.
 *
 * A node holds its props in one of these, and a create operation shares the
 * same object instead of copying it. Reads go through `get()`, the implicit
 * conversion to `const folly::dynamic&`, `->`, `*`, or the forwarded read
 * methods below, so host code written against a plain `folly::dynamic` field
 * (`op.props.isObject()`, `op.props.find(...)`, `RawProps(op.props)`,
 * `folly::dynamic copy = op.props`) keeps compiling. Assigning or
 * constructing from a `folly::dynamic` replaces the object. The first write
 * through `mutate()` copies when anyone else still holds the object, so a
 * version JavaScript or a pending batch retains is never changed under it.
 */
class LoomSharedProps {
 public:
  LoomSharedProps() : value_(emptyProps()) {}
  /** Shares `props`; it must not be written by anyone afterwards. */
  explicit LoomSharedProps(std::shared_ptr<const folly::dynamic> props)
      : value_(props ? std::move(props) : emptyProps()) {}
  /** Owns a new object made from anything a `folly::dynamic` is made from. */
  template <
      typename T,
      typename = std::enable_if_t<
          !std::is_same_v<std::decay_t<T>, LoomSharedProps> &&
          std::is_constructible_v<folly::dynamic, T&&>>>
  // NOLINTNEXTLINE(google-explicit-constructor): stands in for a dynamic field
  LoomSharedProps(T&& props)
      : value_(std::make_shared<const folly::dynamic>(std::forward<T>(props))) {}
  template <
      typename T,
      typename = std::enable_if_t<
          !std::is_same_v<std::decay_t<T>, LoomSharedProps> &&
          std::is_constructible_v<folly::dynamic, T&&>>>
  LoomSharedProps& operator=(T&& props) {
    value_ = std::make_shared<const folly::dynamic>(std::forward<T>(props));
    return *this;
  }

  const folly::dynamic& get() const {
    return *value_;
  }
  // NOLINTNEXTLINE(google-explicit-constructor)
  operator const folly::dynamic&() const {
    return *value_;
  }
  const folly::dynamic* operator->() const {
    return value_.get();
  }
  const folly::dynamic& operator*() const {
    return *value_;
  }
  /** The shared object itself, to hold the props without copying them. */
  const std::shared_ptr<const folly::dynamic>& shared() const {
    return value_;
  }

  // Read access as on a `folly::dynamic`.
  bool isObject() const {
    return value_->isObject();
  }
  bool isNull() const {
    return value_->isNull();
  }
  bool isArray() const {
    return value_->isArray();
  }
  bool isString() const {
    return value_->isString();
  }
  folly::dynamic::Type type() const {
    return value_->type();
  }
  bool empty() const {
    return value_->empty();
  }
  std::size_t size() const {
    return value_->size();
  }
  auto items() const {
    return value_->items();
  }
  auto keys() const {
    return value_->keys();
  }
  auto values() const {
    return value_->values();
  }
  template <typename K>
  auto find(K&& key) const {
    return value_->find(std::forward<K>(key));
  }
  template <typename K>
  std::size_t count(K&& key) const {
    return value_->count(std::forward<K>(key));
  }
  template <typename K>
  const folly::dynamic& at(K&& key) const {
    return value_->at(std::forward<K>(key));
  }
  template <typename K>
  const folly::dynamic& operator[](K&& key) const {
    return (*value_)[std::forward<K>(key)];
  }
  template <typename K>
  const folly::dynamic* get_ptr(K&& key) const {
    return value_->get_ptr(std::forward<K>(key));
  }
  template <typename K, typename D>
  folly::dynamic getDefault(K&& key, D&& fallback) const {
    return value_->getDefault(std::forward<K>(key), std::forward<D>(fallback));
  }
  friend bool operator==(const LoomSharedProps& a, const folly::dynamic& b) {
    return a.get() == b;
  }
  friend bool operator==(const folly::dynamic& a, const LoomSharedProps& b) {
    return a == b.get();
  }

  /** The props for writing: a private copy if the object is shared. */
  folly::dynamic& mutate() {
    if (value_.use_count() != 1) {
      value_ = std::make_shared<const folly::dynamic>(*value_);
    }
    return const_cast<folly::dynamic&>(*value_);
  }

 private:
  static const std::shared_ptr<const folly::dynamic>& emptyProps() {
    static const auto kEmpty =
        std::make_shared<const folly::dynamic>(folly::dynamic::object());
    return kEmpty;
  }
  std::shared_ptr<const folly::dynamic> value_;
};

struct DirectCreateOperation {
  Tag tag;
  bool layoutable;
  std::string componentName;
  /**
   * The node's full merged props, shared with the renderer rather than copied
   * (read it as a `const folly::dynamic&`; see `LoomSharedProps`).
   */
  LoomSharedProps props;
  /**
   * For a component with its own descriptor: the props that descriptor parsed
   * from the JSI value. A host that builds Props objects uses these instead of
   * re-parsing `props`, which cannot carry JS-only values.
   */
  std::shared_ptr<const Props> parsedProps{};
};

struct DirectUpdatePropsOperation {
  Tag tag;
  folly::dynamic props;
  std::shared_ptr<const Props> parsedProps{};
};

struct DirectInsertOperation {
  Tag parentTag;
  Tag tag;
  size_t index;
  /**
   * Only for a host with `LoomPlatformTraits::insertsCarryInitialLayout`: the
   * inserted view was created in this batch and this is its first frame, as a
   * `DirectLayoutOperation` would carry it (relative to `parentTag`). No Layout
   * operation follows for the tag in this batch. Always false otherwise.
   */
  bool hasLayout{false};
  int displayType{1};
  int layoutDirection{1};
  float x{0};
  float y{0};
  float width{0};
  float height{0};
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

/** Typed, ordered operations for one Loom renderer commit. */
struct LoomMountBatch {
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
  /**
   * The batch is a frame of an animation (Loom's animation lane, a layout
   * animation driver), not a React commit. Text resized by it is drawn in the
   * same frame; text resized by a commit can be drawn in the background.
   */
  bool animationFrame{false};

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
    animationFrame = false;
  }
};

} // namespace facebook::react
