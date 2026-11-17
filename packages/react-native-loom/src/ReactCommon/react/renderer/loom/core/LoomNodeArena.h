/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace facebook::react {

/**
 * Node storage for one node-based container (`TagMap`, `TagHashSet`).
 *
 * A hash map allocated every node on its own: one `malloc` per insert, per map,
 * which on a large mount was several allocations per view. The arena hands out
 * slots of one size from chunks that grow geometrically (8 slots, then doubling
 * up to `kMaxChunkSlots`), so a map of n entries makes about log2(n) + n / 256
 * allocations, and its nodes sit next to each other. A container's first
 * `kPlainNodes` nodes are still allocated one by one (see `LoomNodeAllocator`),
 * so a small, short-lived map costs no more than before.
 *
 * A chunk whose slots are all free again is returned at once, except the one
 * new nodes are taken from, so a map that shrinks after a large unmount gives
 * its memory back instead of keeping its peak.
 *
 * Not thread-safe, like the container it belongs to: it is reached only through
 * that container's allocator.
 */
class LoomNodeArena {
 public:
  LoomNodeArena() = default;
  LoomNodeArena(const LoomNodeArena&) = delete;
  LoomNodeArena& operator=(const LoomNodeArena&) = delete;
  ~LoomNodeArena() {
    for (auto* chunk : chunks_) {
      freeChunk(chunk);
    }
  }

  /**
   * The size every slot has. Fixed by the first `owns` call, which is the
   * container's first single-object allocation (its first node).
   */
  bool owns(size_t size, size_t alignment) {
    if (slotSize_ == 0 && alignment <= alignof(std::max_align_t)) {
      slotSize_ = std::max(roundUp(size), sizeof(FreeSlot));
      requested_ = size;
    }
    return size == requested_;
  }

  void* allocate() {
    if (current_ == nullptr || current_->free == nullptr) {
      current_ = chunkWithFreeSlot();
    }
    auto* slot = current_->free;
    current_->free = slot->next;
    ++current_->live;
    return slot;
  }

  /** False when `pointer` is not one of this arena's slots. */
  bool deallocate(void* pointer) noexcept {
    auto* chunk = chunkOf(pointer);
    if (chunk == nullptr) {
      return false;
    }
    auto* slot = static_cast<FreeSlot*>(pointer);
    slot->next = chunk->free;
    chunk->free = slot;
    if (--chunk->live == 0 && chunk != current_) {
      chunks_.erase(std::find(chunks_.begin(), chunks_.end(), chunk));
      freeChunk(chunk);
    }
    return true;
  }

 private:
  static constexpr size_t kFirstChunkSlots = 8;
  static constexpr size_t kMaxChunkSlots = 256;

  struct FreeSlot {
    FreeSlot* next;
  };
  struct Chunk {
    uintptr_t begin;
    uintptr_t end;
    FreeSlot* free;
    size_t live;
  };

  static size_t roundUp(size_t size) {
    constexpr size_t align = alignof(std::max_align_t);
    return (size + align - 1) / align * align;
  }

  Chunk* chunkWithFreeSlot() {
    for (auto* chunk : chunks_) {
      if (chunk->free != nullptr) {
        return chunk;
      }
    }
    const auto slots = std::min(
        kMaxChunkSlots, std::max(kFirstChunkSlots, slotCount_));
    slotCount_ += slots;
    const auto header = roundUp(sizeof(Chunk));
    auto* memory = static_cast<char*>(::operator new(header + slots * slotSize_));
    auto* chunk = new (memory) Chunk{};
    chunk->begin = reinterpret_cast<uintptr_t>(memory + header);
    chunk->end = chunk->begin + slots * slotSize_;
    // Thread the free list so slots are handed out in address order.
    FreeSlot* next = nullptr;
    for (size_t index = slots; index-- > 0;) {
      auto* slot = reinterpret_cast<FreeSlot*>(chunk->begin + index * slotSize_);
      slot->next = next;
      next = slot;
    }
    chunk->free = next;
    // Sorted by address, so `chunkOf` is a binary search.
    chunks_.insert(
        std::upper_bound(
            chunks_.begin(),
            chunks_.end(),
            chunk,
            [](const Chunk* left, const Chunk* right) {
              return left->begin < right->begin;
            }),
        chunk);
    return chunk;
  }

  Chunk* chunkOf(void* pointer) const noexcept {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    auto found = std::upper_bound(
        chunks_.begin(),
        chunks_.end(),
        address,
        [](uintptr_t value, const Chunk* chunk) {
          return value < chunk->begin;
        });
    if (found == chunks_.begin() || address >= (*(found - 1))->end) {
      return nullptr;
    }
    return *(found - 1);
  }

  void freeChunk(Chunk* chunk) noexcept {
    slotCount_ -= (chunk->end - chunk->begin) / slotSize_;
    chunk->~Chunk();
    ::operator delete(static_cast<void*>(chunk));
  }

  std::vector<Chunk*> chunks_;
  Chunk* current_{nullptr};
  size_t slotSize_{0};
  size_t requested_{0};
  /** Slots in all live chunks; the next chunk is this large (capped). */
  size_t slotCount_{0};
};

/**
 * Allocator that takes a container's nodes from its own `LoomNodeArena`. Arrays
 * (the bucket array) and anything not node-sized go to `operator new` as
 * before.
 *
 * A copied container gets a new arena; a moved or swapped one takes its arena
 * along, so the nodes of one arena only ever belong to one container.
 */
template <class T>
class LoomNodeAllocator {
 public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  using is_always_equal = std::false_type;

  LoomNodeAllocator() noexcept = default;
  LoomNodeAllocator(const LoomNodeAllocator&) noexcept = default;
  LoomNodeAllocator(LoomNodeAllocator&&) noexcept = default;
  LoomNodeAllocator& operator=(const LoomNodeAllocator&) noexcept = default;
  LoomNodeAllocator& operator=(LoomNodeAllocator&&) noexcept = default;
  template <class U>
  // NOLINTNEXTLINE(google-explicit-constructor): allocator rebinding.
  LoomNodeAllocator(const LoomNodeAllocator<U>& other) noexcept
      : arena_(other.arena_), plainNodes_(other.plainNodes_) {}

  LoomNodeAllocator select_on_container_copy_construction() const noexcept {
    return LoomNodeAllocator();
  }

  T* allocate(size_t count) {
    if (count == 1) {
      if (arena_ == nullptr && plainNodes_ < kPlainNodes) {
        ++plainNodes_;
        return std::allocator<T>{}.allocate(1);
      }
      if (arena_ == nullptr) {
        arena_ = std::make_shared<LoomNodeArena>();
      }
      if (arena_->owns(sizeof(T), alignof(T))) {
        return static_cast<T*>(arena_->allocate());
      }
    }
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* pointer, size_t count) noexcept {
    if (count == 1 && arena_ != nullptr && arena_->deallocate(pointer)) {
      return;
    }
    std::allocator<T>{}.deallocate(pointer, count);
  }

  template <class U>
  bool operator==(const LoomNodeAllocator<U>& other) const noexcept {
    return arena_ == other.arena_;
  }
  template <class U>
  bool operator!=(const LoomNodeAllocator<U>& other) const noexcept {
    return arena_ != other.arena_;
  }

 private:
  template <class U>
  friend class LoomNodeAllocator;
  /** Nodes allocated one by one before the arena is created. */
  static constexpr uint32_t kPlainNodes = 8;
  std::shared_ptr<LoomNodeArena> arena_;
  uint32_t plainNodes_{0};
};

} // namespace facebook::react
