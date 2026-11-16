/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

// Only the Loom renderer build (LOOM_RENDERER) compiles this header; without it Fabric is untouched.
#if LOOM_RENDERER

#include <atomic>
#include <react/renderer/runtimescheduler/LoomSettings.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <folly/dynamic.h>
#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/renderer/core/EventEmitter.h>
#include <react/renderer/core/RawEvent.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/graphics/Rect.h>
#include <react/renderer/graphics/Size.h>
#include <react/renderer/mounting/ShadowView.h>
#include <react/renderer/loom/compat/LoomCompat.h>
#include <react/renderer/loom/mounting/LoomMountBatch.h>
#include <react/renderer/loom/binding/LoomEventManager.h>
#include <react/renderer/loom/host/LoomPlatformTraits.h>
#include <react/renderer/loom/worker/LoomWorkerProfile.h>
#include <react/renderer/loom/text/LoomTextMeasurement.h>
#include <yoga/Yoga.h>

namespace facebook::react {

/**
 * The layout-relevant style props this renderer understands, keyed so that the
 * Yoga style can be reset for every key a revision does not set.
 */
enum class LoomStyleKey : uint8_t {
  Width,
  Height,
  MinWidth,
  MinHeight,
  MaxWidth,
  MaxHeight,
  FlexBasis,
  Flex,
  FlexGrow,
  FlexShrink,
  AspectRatio,
  MarginAll,
  MarginHorizontal,
  MarginVertical,
  MarginStart,
  MarginEnd,
  MarginTop,
  MarginRight,
  MarginBottom,
  MarginLeft,
  PaddingAll,
  PaddingHorizontal,
  PaddingVertical,
  PaddingStart,
  PaddingEnd,
  PaddingTop,
  PaddingRight,
  PaddingBottom,
  PaddingLeft,
  BorderAll,
  BorderHorizontal,
  BorderVertical,
  BorderStart,
  BorderEnd,
  BorderTop,
  BorderRight,
  BorderBottom,
  BorderLeft,
  PositionAll,
  PositionHorizontal,
  PositionVertical,
  PositionStart,
  PositionEnd,
  PositionTop,
  PositionRight,
  PositionBottom,
  PositionLeft,
  GapAll,
  GapRow,
  GapColumn,
  Count,
};

/**
 * A direct JS-to-Yoga-to-host-view renderer used only by LOOM_RENDERER.
 *
 * This renderer owns the whole tree model: one `Node` per React tag holding the
 * *merged* props, a mirror of those props in a retained Yoga tree, and a
 * resolved text payload for every text run. It replaces the shadow-tree
 * renderer, so everything that path did for correctness has to be done here
 * explicitly - in particular text, which on Android Fabric is rendered from a
 * paragraph *state* rather than from props.
 *
 * Threading: all methods run on the JS/UI thread, inside a React commit.
 */
/**
 * How the animation fast lane treats one animated prop key
 *.
 *
 * Paint keys are applied to the native view by the host and folded into the
 * renderer with no style rebuild, no Yoga and no diff. Layout keys change the
 * box, so they need Yoga and take one incremental commit per frame. Text keys
 * drive font shaping and text measurement and always take the ordinary commit
 * path.
 */
enum class AnimatedPropClass {
  Paint,
  Layout,
  Text,
};

/**
 * Classifies one hoisted (top-level) prop key. Anything not recognised is
 * layout: an unknown key takes the ordinary patch-props-and-commit path rather
 * than being folded blind.
 */
AnimatedPropClass classifyAnimatedPropKey(const std::string& key);

/**
 * One animation frame's props, split by class. Keep this type distinct from
 * animationbackend/AnimatedProps.h's facebook::react::AnimatedProps: sharing
 * that name violates the ODR and can select the backend's constructor at link
 * time, leaving the lane's dynamic objects null.
 */
struct ClassifiedAnimatedProps {
  folly::dynamic paint{folly::dynamic::object()};
  folly::dynamic layout{folly::dynamic::object()};
  /**
   * True when any key of the payload is a text key. Such a payload has to go
   * through the normal commit path whole; the split is then unused.
   */
  bool text{false};

  bool empty() const {
    return paint.empty() && layout.empty() && !text;
  }
};

/**
 * Splits an animated props payload into the three classes, hoisting a nested
 * `style` first so the classification sees the keys the host sees.
 */
ClassifiedAnimatedProps classifyAnimatedProps(const folly::dynamic& props);

/**
 * Hash for tag-keyed maps. The identity hash libc++ uses by default pairs with
 * a prime bucket count, so every lookup pays a software division on 32-bit
 * ARM (`__udivmodsi4`). Tags also advance in steps of 2 or 10, which a plain
 * mask would fold onto a fraction of the buckets, hence the multiplicative
 * mix. Used together with a power-of-two `reserve` so libc++ masks instead of
 * dividing (it keeps a power-of-two count as the table grows).
 */
struct LoomTagHash {
  size_t operator()(Tag tag) const noexcept {
    uint32_t h = static_cast<uint32_t>(tag) * 0x9E3779B1u;
    return static_cast<size_t>(h ^ (h >> 15));
  }
};

/**
 * `std::unordered_map<Tag, V>` with `LoomTagHash` and a power-of-two initial
 * bucket count, so libc++ keeps indexing by mask as the table grows (see
 * LoomTagHash). Use it for any map keyed by a React tag on a commit path.
 */
template <class V>
class TagMap : public std::unordered_map<Tag, V, LoomTagHash> {
  using Base = std::unordered_map<Tag, V, LoomTagHash>;

  // Construction allocates nothing: most per-commit maps stay empty or small,
  // and an eager bucket array cost a malloc plus zeroing per map. The first
  // insert sizes the table to a power of two instead (libc++ would otherwise
  // start at 2 buckets and move to prime counts, i.e. division per lookup).
  static constexpr size_t kInitialBuckets = 8;

  void ensureBuckets() {
    if (Base::bucket_count() == 0) {
      Base::rehash(kInitialBuckets);
    }
  }

  static size_t roundUpPow2(size_t n) {
    size_t p = kInitialBuckets;
    while (p < n) {
      p <<= 1;
    }
    return p;
  }

 public:
  TagMap() = default;
  TagMap(std::initializer_list<typename Base::value_type> init) {
    reserve(init.size());
    for (const auto& value : init) {
      Base::insert(value);
    }
  }
  TagMap(const TagMap&) = default;
  TagMap(TagMap&&) noexcept = default;
  TagMap& operator=(const TagMap&) = default;
  TagMap& operator=(TagMap&&) noexcept = default;

  /** Reserves a power-of-two bucket count so indexing stays on a mask. */
  void reserve(size_t count) {
    if (count == 0) {
      return;
    }
    const auto buckets = roundUpPow2(static_cast<size_t>(
        static_cast<float>(count) / Base::max_load_factor() + 0.5f));
    if (buckets > Base::bucket_count()) {
      Base::rehash(buckets);
    }
  }

  V& operator[](Tag tag) {
    ensureBuckets();
    return Base::operator[](tag);
  }

  template <class... Args>
  std::pair<typename Base::iterator, bool> emplace(Args&&... args) {
    ensureBuckets();
    return Base::emplace(std::forward<Args>(args)...);
  }

  template <class... Args>
  std::pair<typename Base::iterator, bool> try_emplace(
      Tag tag,
      Args&&... args) {
    ensureBuckets();
    return Base::try_emplace(tag, std::forward<Args>(args)...);
  }

  template <class M>
  std::pair<typename Base::iterator, bool> insert_or_assign(
      Tag tag,
      M&& value) {
    ensureBuckets();
    return Base::insert_or_assign(tag, std::forward<M>(value));
  }

  std::pair<typename Base::iterator, bool> insert(
      const typename Base::value_type& value) {
    ensureBuckets();
    return Base::insert(value);
  }

  std::pair<typename Base::iterator, bool> insert(
      typename Base::value_type&& value) {
    ensureBuckets();
    return Base::insert(std::move(value));
  }
};

class LoomRenderer final {
 public:
  struct TextMeasurement {
    float width{0};
    float height{0};
    bool valid{false};
    LoomTextMeasurement prepared{};
  };

  /**
   * Host-side operations. Every one of them runs synchronously on the same
   * thread as the renderer.
   */
  struct NativeOperations {
    /** True only while this renderer's surface generation is still current. */
    std::function<bool(SurfaceId)> isSurfaceActive;
    /** Invalidates a surface whose synchronous commit failed part-way through.
     */
    std::function<void(SurfaceId, const std::string&)> failSurface;
    /** Applies one flat, ordered operation batch to the host view hierarchy. */
    std::function<void(SurfaceId, LoomMountBatch)> applyBatch;
    /**
     * Optional. Like `applyBatch`, but may reach the UI thread while this
     * commit is still computing; always before the commit's own batch.
     */
    std::function<void(SurfaceId, LoomMountBatch)> applyBatchEarly;
    std::function<void(SurfaceId, Tag, InstanceHandle::Shared)>
        registerEventTarget;
    std::function<void(SurfaceId, Tag)> unregisterEventTarget;
    std::function<void(
        SurfaceId,
        Tag,
        const std::string&,
        folly::dynamic,
        RawEvent::Category,
        bool)>
        dispatchEvent;
    /**
     * Parses a node's text props through React Native's own props parser,
     * applying them on top of `inherited`. `outParagraphAttributes` is only
     * filled for paragraph components.
     */
    std::function<TextAttributes(
        SurfaceId,
        const std::string& componentName,
        const folly::dynamic& props,
        const TextAttributes& inherited,
        ParagraphAttributes* outParagraphAttributes)>
        parseTextAttributes;
    /**
     * Real platform text measurement, in the layout's own coordinate space.
     * Absent or failing means the renderer falls back to an estimate that is
     * only good enough to bootstrap.
     */
    std::function<TextMeasurement(
        SurfaceId,
        const AttributedString&,
        const ParagraphAttributes&,
        const Size& minimumSize,
        const Size& maximumSize)>
        measureText;
    std::function<LoomTextMeasurement(
        SurfaceId,
        const AttributedString&,
        const ParagraphAttributes&,
        const Size& minimumSize,
        const Size& maximumSize)>
        prepareText;
    /** Builds current visual text state for mounting without measuring it for
     * Yoga. */
    std::function<LoomTextMeasurement(
        SurfaceId,
        const AttributedString&,
        const ParagraphAttributes&,
        const Size& minimumSize,
        const Size& maximumSize)>
        prepareTextForMount;
    /** False when the host consumes paragraph state directly and cannot use
     * an opaque prepared-layout object returned by mount preparation. */
    bool prepareTextForMountProvidesLayout{true};
    /** Reads the host view's current scroll position, not just its props. */
    std::function<std::pair<float, float>(SurfaceId, Tag)> getScrollOffset;
    /** Reads a mounted view's live origin relative to its surface root. */
    std::function<std::optional<std::pair<float, float>>(SurfaceId, Tag)>
        getViewLocationInSurface;
    std::function<void(SurfaceId, Tag)> reportUnsupportedInlineAttachment;
    std::function<void(SurfaceId, Tag)> reportUnsupportedTransform;
    std::function<void(SurfaceId, Tag, const std::string&)>
        reportUnsupportedIntrinsicMeasurement;
    /**
     * Publishes the paragraph state (attributed string + paragraph
     * attributes) for a text root. Android renders text styling exclusively
     * from this state, so this is what makes fonts, weights and line heights
     * come out right.
     */
    /**
     * Publishes the component's StateWrapper payload.  The Loom renderer
     * does not assume that every state is paragraph state: the host selects
     * the component-specific StateData while preserving the normal
     * ViewManager.updateState contract.
     */
    std::function<void(LoomTextStateUpdate)> updateState;
    /**
     * Fabric compatibility layer: hands a compat node's own state object to
     * its host view (Fabric's UpdateState mutation).
     */
    std::function<void(SurfaceId, Tag, State::Shared)> updateComponentState;
    std::function<void(SurfaceId, Tag, const folly::dynamic&, const Rect&)>
        updateScrollContentBounds;
    /** `UIManager`-level host operations that need this renderer's tags only.
     */
    std::function<void(
        SurfaceId,
        Tag,
        const std::string& commandName,
        const folly::dynamic& args)>
        dispatchCommand;
    std::function<void(SurfaceId, Tag, const std::string& eventType)>
        sendAccessibilityEvent;
    std::function<
        void(SurfaceId, Tag, bool isJSResponder, bool blockNativeResponder)>
        setIsJSResponder;
    /** Called after a complete commit and any renderer-local reentrant work. */
    std::function<void(SurfaceId)> commitFinished;
  };

  LoomRenderer(SurfaceId surfaceId, NativeOperations operations);
  ~LoomRenderer();
  LoomRenderer(const LoomRenderer&) = delete;
  LoomRenderer& operator=(const LoomRenderer&) = delete;

  /**
   * The surface's viewport, exactly as the shadow tree receives it: a minimum
   * and a maximum size. The minimum is what gives the root a definite height,
   * without which a `flex: 1` child collapses to nothing.
   */
  void setConstraints(
      float minWidth,
      float minHeight,
      float maxWidth,
      float maxHeight,
      bool isRTL = false);
  /**
   * Sets the host scale factor and reports whether it actually changed. Callers
   * should only reflow when it did: the host republishes the scale on every
   * surface size update, and a reflow re-parses text and re-runs Yoga for the
   * whole tree.
   */
  bool setPointScaleFactor(float pointScaleFactor);
  /** Re-runs layout and publishes whatever the host does not have yet. */
  void reflow();
  /**
   * Fabric compatibility layer (LoomCompat.h). Without it no node gets a
   * ShadowNode of its own.
   */
  void setCompat(std::shared_ptr<LoomCompat> compat) {
    compat_ = std::move(compat);
  }
  /**
   * Applies a native state update to the compat node of its family and lays
   * the surface out again, as `UIManager::updateState` commits in Fabric.
   * False when no live node of this renderer belongs to that family.
   */
  bool applyComponentState(const StateUpdate& update);
  void createNode(
      Tag tag,
      const std::string& componentName,
      const folly::dynamic& props,
      InstanceHandle::Shared instanceHandle = nullptr);
  void createRawTextNode(Tag tag, std::string text);
  void updateRawText(Tag tag, std::string text);
  /** Attaches the NativeDOM document-element handle before/at first commit. */
  void setInstanceHandle(Tag tag, InstanceHandle::Shared instanceHandle);
  /** Stable React host identity, distinct from a potentially reused tag. */
  uint64_t identityOf(Tag tag) const;
  void setIdentity(Tag tag, uint64_t identity);
  /** Capture detach edges for old parents absent from a selected draft tree. */
  void captureRemovedMountedEdges(
      const TagMap<uint64_t>& selectedIdentities);
  /**
   * The incremental form of `captureRemovedMountedEdges`: only the live
   * subtrees under `removalRoots` can lose a node, so those are walked (in the
   * same children-first order) instead of the whole tree. A node whose
   * (tag, identity) is in `keptIdentities` stays in the tree, and so does its
   * subtree.
   */
  void captureRemovedMountedEdgesFrom(
      const TagMap<uint64_t>& keptIdentities,
      const std::vector<Tag>& removalRoots);
  /**
   * Tags this renderer dropped since the last call. Lets the binding keep its
   * mounted-identity bookkeeping as a delta instead of re-walking the tree.
   */
  std::vector<Tag> takeDroppedTags() {
    return std::exchange(droppedTags_, {});
  }
  /**
   * The commits until this is reset are frames of a JS-driven animation (a
   * requestAnimationFrame callback's), so their batches are animation frames.
   */
  void setAnimationFrameCommit(bool animationFrame) {
    animationFrameCommit_ = animationFrame;
  }
  /** True when the next commit walks the whole tree (first commit, recovery). */
  bool nextCommitIsFull() const {
    return fullPrepare_;
  }
  /** `propsPatch` is an attribute-payload diff, exactly as React produced it.
   */
  void cloneNodeWithNewProps(Tag tag, const folly::dynamic& propsPatch);
  /** Sets whether an offscreen boundary hides the node (see `offscreenHidden`). */
  void setOffscreenHidden(Tag tag, bool hidden);
  /**
   * Records the props a compat component's descriptor parsed for the props
   * this node holds now (call after the node's props are applied).
   */
  void setCompatProps(Tag tag, std::shared_ptr<const Props> props);
  void setChildren(Tag parentTag, const std::vector<Tag>& children);
  /**
   * The commit. `containerTag` is the surface's own tag and `children` is the
   * authoritative child list of the surface root, which React passes along.
   */
  void completeRoot(Tag containerTag, const std::vector<Tag>& children);
  void completeRoot(
      Tag containerTag,
      const std::vector<Tag>& children,
      bool runCommit);
  /**
   * Prepares a commit. Reports false when a commit is already running, in which
   * case this one has been deferred onto `commitPending_` and the caller must not
   * continue: the running commit drains it.
   */
  bool beginCommitLayout();
  void calculateCommitLayout();
  void finishCommitAfterLayout();
  /** Applies only a prevalidated paint-only prop commit, without layout walks.
   */
  bool tryCommitPaintOnlyProps(
      const std::vector<std::pair<Tag, folly::dynamic>>& patches);
  bool hasSameInstanceHandle(Tag tag, const InstanceHandle::Shared& handle)
      const;

  /**
   * Folds paint-class animated props into their nodes: one merged prop write
   * per tag, deliberately without the style rebuild, the text invalidation and
   * the batch emission a normal commit does, because the host has already
   * applied those values to the native views (design §3.2).
   *
   * Only legal while this renderer owns the computation, which every caller
   * guarantees by running inside a job.
   */
  void foldPaintProps(
      const std::vector<std::pair<Tag, folly::dynamic>>& patches);

  /**
   * Applies one animation frame. The paint half is folded; the layout half goes
   * through `patchProps` and then ONE commit for the whole frame, whatever the
   * number of animated nodes (design §5.2). Layout keys are never sent to a
   * native view directly: a view's frame comes only from a Yoga layout
   * operation.
   */
  void applyAnimationFrame(
      const std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
      const std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches);

  void dispatchCommand(
      Tag tag,
      const std::string& commandName,
      const folly::dynamic& args);
  void sendAccessibilityEvent(Tag tag, const std::string& eventType);
  void setIsJSResponder(Tag tag, bool isJSResponder, bool blockNativeResponder);

  bool isTextNode(Tag tag) const;
  bool exists(Tag tag) const;
  std::vector<Tag> childrenOf(Tag tag) const;
  /**
   * Borrowed form of `childrenOf` for hot paths that only read the list: a
   * commit walks every selected node, and copying a vector per node is pure
   * waste there. Null when the node is unknown. Invalidated by any mutation.
   */
  const std::vector<Tag>* childrenOfRef(Tag tag) const;
  bool isConnected(Tag tag) const;
  Tag findByNativeId(const std::string& nativeId) const;
  std::string componentName(Tag tag) const;
  /**
   * The roles resolved when the node was created; a raw text leaf has only
   * `RawText`, an unknown tag none.
   */
  LoomComponentTable::Roles componentRoles(Tag tag) const;
  std::string textContent(Tag tag) const;
  double numericProp(Tag tag, const char* name) const;
  std::array<double, 4> borderWidths(Tag tag) const;
  std::pair<float, float> scrollOffset(Tag tag) const;
  std::pair<float, float> scrollContentSize(Tag tag) const;
  bool isPositioned(Tag tag) const;
  bool isCommitting() const;

  /** A frame in DIP: an origin relative to some ancestor, plus the own size. */
  struct Metrics {
    float x{0};
    float y{0};
    float width{0};
    float height{0};
  };

  /** The instance handle React registered for `tag`, or null if it gave none.
   */
  InstanceHandle::Shared instanceHandle(Tag tag) const;
  Tag parentOf(Tag tag) const;
  /** The surface this renderer belongs to. */
  SurfaceId surfaceId() const;
  /** The surface root's tag, or 0 before React has created it. */
  Tag rootTag() const;

  /**
   * The deepest node under `tag` whose frame contains the point, following the
   * rules the shadow tree uses: children are hit back to front, and a node is
   * only reachable through an ancestor that lets children be touch targets.
   * Returns 0 for a miss.
   */
  Tag findNodeAtPoint(Tag tag, float x, float y) const;

  /**
   * `Node.compareDocumentPosition` for two of this renderer's nodes, using the
   * bit values the DOM API defines. A tag this renderer does not know is
   * disconnected.
   */
  unsigned compareDocumentPosition(Tag tag, Tag otherTag) const;

  /**
   * `tag`'s size and its origin relative to `ancestorTag`, which has to be an
   * ancestor of `tag` (`ancestorTag == 0` means the surface root). False when
   * `tag` is unknown, has no frame of its own, or `ancestorTag` is not an
   * ancestor - the cases `measureLayout` reports as a failure.
   */
  bool metricsRelativeTo(
      Tag tag,
      Tag ancestorTag,
      Metrics& out,
      bool includeTransform = true,
      bool includeScroll = true) const;
  std::optional<std::pair<float, float>> viewLocationInSurface(Tag tag) const;

  /**
   * Fabric compatibility layer: what a Fabric node of `tag` is built from when
   * a library asks for one. Pointers are borrowed and invalidated by any
   * mutation, so callers read them right after the worker is synchronized.
   */
  struct FabricNodeInfo {
    const std::string* componentName{nullptr};
    const folly::dynamic* props{nullptr};
    uint64_t propsGeneration{0};
    uint64_t identity{0};
    Tag parent{0};
    /** Mounts no view of its own (flattened). */
    bool layoutOnly{false};
    bool virtualNode{false};
    const LoomCompatNode* compat{nullptr};
  };
  std::optional<FabricNodeInfo> fabricNodeInfo(Tag tag) const;
  /** How many views are mounted in `tag`'s view (flattened children excluded). */
  size_t mountedChildCount(Tag tag) const;

  /** Applies a props patch outside a commit, for `setNativeProps`. */
  void setNativeProps(Tag tag, const folly::dynamic& propsPatch);
  /** Stops this surface after host publication or reconciliation fails. */
  void abortCommit(const std::string& reason) noexcept;
  bool hasFailed() const;
  /** Returns the first ordinary view nested below a text root, or tag 0. */
  static Tag findUnsupportedInlineAttachment(
      const std::vector<Tag>& rootChildren,
      const std::function<LoomComponentTable::Roles(Tag)>& rolesFor,
      const std::function<const std::vector<Tag>* (Tag)>& childrenFor);

 private:
  /**
   * The native suite drives `measureTextNode` through this named accessor. The
   * measurement cache can only be shown to work by asking for one constraint
   * pair, then another, then the first again, and the method that answers is
   * private to this class.
   */
  friend struct LoomRendererTestAccess;

  using StyleKey = LoomStyleKey;

  /** Surface roots are owned by the host; they only receive child operations.
   */
  static constexpr const char* kRootComponentName = "Root";

  /** A single Yoga style value as React expresses it. */
  struct StyleValue {
    enum class Unit : uint8_t { Point, Percent };
    float value{0};
    Unit unit{Unit::Point};

    bool operator==(const StyleValue& other) const {
      return value == other.value && unit == other.unit;
    }
  };

  /**
   * The layout-relevant slice of a node's props, parsed from the full prop set
   * on every change. Keyed by `StyleKey` rather than stored as raw props so
   * that yoga can be reset for keys this revision does not set.
   */
  struct LayoutStyle {
    std::array<std::optional<StyleValue>, size_t(StyleKey::Count)> values{};
    /** Only meaningful for the keys that accept `auto`. */
    std::array<bool, size_t(StyleKey::Count)> autoFlags{};

    bool operator==(const LayoutStyle& other) const {
      return values == other.values && autoFlags == other.autoFlags;
    }
    bool operator!=(const LayoutStyle& other) const {
      return !(*this == other);
    }
  };

  /**
   * [LoomPerf item 2] Parsed layout inputs of a props shape: everything
   * `refreshStyle`, the layout-related role flags and `applyYogaProps` derive
   * from the props, keyed by the layout-relevant keys only (paint props do not
   * take part, so rows that differ only in colour share a shape). `entries`
   * holds the keys themselves so a hash hit is verified, never trusted.
   */
  struct LayoutShape {
    std::vector<std::pair<std::string, folly::dynamic>> entries;
    std::string componentName;
    bool hasOtherKeys{false};
    LayoutStyle style;
    YGDirection direction{YGDirectionInherit};
    YGBoxSizing boxSizing{YGBoxSizingBorderBox};
    bool displayNone{false};
    bool layoutOnly{false};
    float zIndex{0};
    bool clipsOverflow{false};
    /** A configured Yoga node whose style is copied into new nodes. */
    mutable YGNodeRef yogaTemplate{nullptr};
    LayoutShape() = default;
    LayoutShape(const LayoutShape&) = delete;
    LayoutShape& operator=(const LayoutShape&) = delete;
    ~LayoutShape() {
      if (yogaTemplate != nullptr) {
        YGNodeFree(yogaTemplate);
      }
    }
  };

  struct Node {
    Tag tag{0};
    uint64_t identity{0};
    /** Valid only while `shapeGeneration == propsGeneration`. */
    std::shared_ptr<const LayoutShape> shape;
    uint64_t shapeGeneration{0};
    Tag parent{0};
    /** Component name as JS sent it (`RCTText`, `RCTView`, ...). */
    std::string componentName;
    /** Resolved from `componentName` once, in `createNode`. */
    LoomComponentTable::Roles componentRoles;
    /** Merged, flattened props. Never a diff. */
    folly::dynamic props{folly::dynamic::object()};
    /**
     * Props changed since the host last saw them. The host is always sent the
     * whole merged set, which is what the shadow-tree path mounts too, so a
     * prop that a patch removed is reset instead of lingering.
     */
    bool propsDirty{false};
    /**
     * The keys merged since the host last saw this node (null when none). For
     * the core view managers in `roleSendsPropDiffs` an update ships this
     * instead of the whole merged set; reset values arrive as explicit nulls.
     */
    folly::dynamic propsDelta{nullptr};
    /** `componentSendsPropDiffs(componentName)`: whether `mergeProps` records `propsDelta`. */
    bool collectPropsDelta{false};
    /**
     * Bumped whenever `props` changes. Two funnels mutate props — `mergeProps`
     * for a patch and `createNode` for the initial set — so this is a reliable
     * key for anything derived from them.
     */
    uint64_t propsGeneration{0};
    /**
     * Everything only text needs. A View, an Image or a ScrollView carries none
     * of this (about 3.5 KB), so `Node::text` is allocated by the first write
     * (`ensureText`) on a text root, a nested text or a TextInput, and readers
     * of a node without one see the defaults (`textOrEmpty`).
     */
    struct TextData {
      /**
       * The last `parseTextAttributes` result for this node.
       *
       * Rebuilding a text subtree calls the platform bridge once per node in it,
       * including nodes whose props did not change. The parse depends only on
       * `props`, the inherited attributes and whether this is a paragraph root, so
       * those three are the key: a match means the previous answer is still exact
       * and no invalidation is needed anywhere.
       */
      struct ParsedTextAttributes {
        bool valid{false};
        uint64_t propsGeneration{0};
        bool textRoot{false};
        TextAttributes inherited{};
        TextAttributes attributes{};
        ParagraphAttributes paragraphAttributes{};
      };
      ParsedTextAttributes parsedTextAttributes;
      /** Text attributes with the inherited chain already applied. */
      TextAttributes textAttributes{};
      /** Paragraph attributes; only filled for text roots. */
      ParagraphAttributes paragraphAttributes{};
      /**
       * The styled strings below are shared and immutable: one text change
       * builds one `AttributedString` and every user (measurement, the measured
       * and prepared records, the state published to the host) holds the same
       * object. Comparisons test the pointer first and fall back to the content
       * compare only when two different builds meet.
       */
      using SharedString = std::shared_ptr<const AttributedString>;
      static const AttributedString& stringOf(const SharedString& string) {
        static const AttributedString kEmpty;
        return string ? *string : kEmpty;
      }
      /** Text content of this subtree. */
      SharedString attributedString;
      /**
       * Text content to *measure* with, which is not always `attributedString`.
       *
       * An empty `TextInput` that has a `placeholder` measures the placeholder, so
       * that an auto-height input stays as tall as its hint instead of collapsing
       * to zero. Fabric makes the same split: `attributedStringBoxToMeasure` in
       * `BaseTextInputShadowNode.h` substitutes the placeholder, while the state
       * box keeps the real text. The published state therefore still carries the
       * *actual* content - appending the placeholder to `attributedString` would
       * send the hint to the host as if the user had typed it.
       */
      SharedString measurementAttributedString;
      /** Text attributes used to key Yoga measurement; paint-only fields are
       * ignored. */
      SharedString measuredAttributedString;
      ParagraphAttributes measuredParagraphAttributes{};
      Size measuredMinimumSize{};
      Size measuredMaximumSize{};
      float measuredPointScaleFactor{1.0f};
      TextMeasurement measuredText{};
      /** Full attributes represented by measuredText.prepared for mounting. */
      SharedString preparedAttributedString;
      ParagraphAttributes preparedParagraphAttributes{};
      /**
       * Recent measurements of the same content under other constraints.
       *
       * Yoga measures one text node with several `(min, max)` pairs in a single
       * layout pass — a bounded measure and then an exact one, for instance — so a
       * single slot misses on nearly every call and each miss rebuilds the
       * platform text layout. These entries are consulted before the bridge call;
       * `measuredText` above stays the most recent one because mounting reads
       * `preparedAttributedString`, which describes exactly that measurement.
       */
      struct TextMeasurementEntry {
        Size minimumSize{};
        Size maximumSize{};
        float pointScaleFactor{1.0f};
        TextMeasurement measurement{};
      };
      static constexpr size_t kTextMeasurementCacheEntries = 4;
      std::array<TextMeasurementEntry, kTextMeasurementCacheEntries>
          recentTextMeasurements_{};
      size_t recentTextMeasurementCount_{0};
    };
    std::unique_ptr<TextData> text;
    TextData& ensureText() {
      if (!text) {
        text = std::make_unique<TextData>();
      }
      return *text;
    }
    const TextData& textOrEmpty() const {
      static const TextData kEmpty;
      return text ? *text : kEmpty;
    }
    LayoutStyle style;
    YGDirection direction{YGDirectionInherit};
    YGBoxSizing boxSizing{YGBoxSizingBorderBox};
    bool textLike{false};
    bool inlineAttachment{false};
    bool inlineAttachmentReported{false};
    /**
     * No Yoga node, no host view, no operations. A text node nested in another
     * text node is rendered by the enclosing paragraph and must not create a
     * second TextView; the same is true of raw text and of the non-text
     * children of a paragraph, which the platform cannot lay out inline.
     */
    bool virtualNode{false};
    /** Yoga-only View whose children mount under its nearest native ancestor.
     */
    bool layoutOnlyNode{false};
    /** Outermost text node: owns the host view, the measure function, state. */
    bool textRoot{false};
    /** Surface root: host-owned, never created, laid out or deleted here. */
    bool hostOwned{false};
    /** Content changed since the attributed string was last built. */
    bool textDirty{false};
    /** The host still needs the paragraph state for this text root. */
    bool needsState{false};
    /** The host has received the initial generic StateWrapper, if required. */
    bool genericStateInitialized{false};
    bool hasScrollContentBounds{false};
    Rect mountedScrollContentBounds{};
    bool intrinsicMeasurementReported{false};
    /** Last successful measure; avoids repeated bridge calls during one tree
     * layout. */
    mutable bool hasTextMeasurementCache{false};
    /**
     * Bumped when `measurementAttributedString` or the paragraph attributes
     * change what a measurement depends on; a cached measurement is valid only
     * for the generation it was taken at.
     */
    uint64_t measurementContentGeneration{1};
    mutable uint64_t measuredContentGeneration{0};

    YGNodeRef yogaNode{nullptr};
    /** Yoga children currently attached, so they are only touched on change. */
    std::vector<Tag> yogaChildren;
    std::vector<Tag> children;
    /** Children the host currently has (its own, view-forming ones). */
    std::vector<Tag> mountedChildren;
    /** The selected child order or host-child representation changed. */
    bool childrenDirty{true};
    bool styleDirty{true};
    bool hasEventTarget{false};
    /**
     * The handle React registered with `createNode`. Kept because a hit test
     * has to hand the target's handle back to JS, and a node that React never
     * gave a handle for is not a valid hit-test answer.
     */
    InstanceHandle::Shared instanceHandle;
    bool hostCreated{false};
    bool hostLayoutable{false};
    /** A StateWrapper emitted before this node's first direct create. */
    bool createdThisCommit{false};
    /** This identity replaces an older Java ViewState with the same tag. */
    bool replacesHostTagThisCommit{false};
    bool hasMountedLayout{false};
    float mountedLeft{0};
    float mountedTop{0};
    float mountedNativeLeft{0};
    float mountedNativeTop{0};
    float mountedWidth{0};
    float mountedHeight{0};
    Tag mountedParentTag{0};
    bool displayNone{false};
    /**
     * Hidden by an offscreen boundary (Activity, Suspense, react-freeze): React
     * commits a hidden clone of the version rather than new props, and shows
     * the node again by committing the version itself. It is version state,
     * not a prop, so showing again needs no props to be undone.
     */
    bool offscreenHidden{false};
    /** Last values handed to the host, so only real changes are re-emitted. */
    int mountedDisplayType{1};
    int mountedDirection{1};
    bool hasMountedInsets{false};
    std::array<float, 4> mountedContentInsets{};
    std::array<float, 4> mountedOverflowInsets{};
    /**
     * Per-commit marks, replacing the two hash containers this used to consult.
     * `commitStamp_` is bumped once per commit, so "was this node reached by the
     * current prepare walk" and "is `commitOverflow` current" are one integer
     * comparison instead of a hash lookup - and, in `prepare`'s case, one
     * allocation per node per commit. Zero means "never", so the counter starts
     * above it and is 64-bit to keep wraparound out of the argument.
     */
    uint64_t visitedStamp{0};
    uint64_t overflowStamp{0};
    /**
     * Incremental commit. A change marks its node (`prepareSelf`) and every
     * ancestor (`prepareDescendant`), so a commit follows only those paths and
     * never visits an unchanged subtree. `lastInsideText` and
     * `lastParentViewTag` are the context the node was last prepared in; a
     * different context (a move, or an ancestor changing role) re-prepares it.
     */
    bool prepareSelf{true};
    bool prepareDescendant{true};
    bool preparedOnce{false};
    bool lastInsideText{false};
    Tag lastParentViewTag{0};
    /** Reached by the post-layout walk in the commit with this stamp. */
    uint64_t layoutVisitStamp{0};
    /** Cached `calculateOverflow` result for the stamping commit. */
    std::array<float, 4> commitOverflow{};
    /**
     * Role flags derived from `componentName` and `props`, cached across commits.
     *
     * `prepare` recomputed these for every node on every commit, and
     * `isLayoutOnlyView` is not a name check: it walks every prop and, for each
     * key that is not one of its thirteen special names, linearly scans the
     * whole style table with string comparisons. That made it the most expensive
     * thing in the commit walk, for a result that only changes when `props` or
     * `componentName` does. Both writes go through `createNode` and `mergeProps`,
     * which clear `roleFlagsValid`.
     */
    bool roleFlagsValid{false};
    /** `zIndex` as the mount order sees it (0 when absent), cached with the role flags. */
    float roleZIndex{0};
    bool roleIsText{false};
    bool roleIsParagraph{false};
    bool roleIsTextInput{false};
    /** What the host builds a state for; set with the other role flags. */
    LoomTextStateKind roleStateKind{LoomTextStateKind::Other};
    bool roleRequiresIntrinsicMeasurement{false};
    bool roleRequiresGenericStateWrapper{false};
    bool roleIsLayoutOnlyView{false};
    /** `overflow` is "hidden" or "scroll": the node's own bounds clip. */
    bool roleClipsOverflow{false};
    /**
     * Android core view manager known to apply props incrementally (it did so
     * for the legacy architecture's diff updates), so updates can carry only
     * the changed keys. Every other component gets the full merged set, which
     * is what Fabric sends.
     */
    bool roleSendsPropDiffs{false};
    /**
     * Fabric compatibility layer: the component's own ShadowNode, for
     * components whose descriptor carries C++ logic (LoomCompat.h). Null for
     * everything else, which is nearly every node.
     */
    std::unique_ptr<LoomCompatNode> compat;
    const ComponentDescriptor* compatDescriptor{nullptr};
    /**
     * Props the component's own descriptor parsed from the JSI value on the JS
     * thread, current while `compatPropsGeneration == propsGeneration`. Set
     * only for components with a compat descriptor.
     */
    std::shared_ptr<const Props> compatProps;
    uint64_t compatPropsGeneration{0};
    bool compatChecked{false};
    LoomRenderer* owner{nullptr};
  };

  /** Raw text is a paragraph-owned string leaf, not a mounted renderer node. */
  struct RawTextLeaf {
    Tag tag{0};
    uint64_t identity{0};
    Tag parent{0};
    std::string text;
    /** See `Node::visitedStamp`; raw text is reachable but not a `Node`. */
    uint64_t visitedStamp{0};
  };

  struct RemovedMountedEdge {
    Tag parentTag{0};
    Tag childTag{0};
    size_t index{0};
  };

  struct LayoutParent {
    Tag tag{0};
    float offsetX{0};
    float offsetY{0};
  };

  /**
   * Direct operation codes. They must stay in sync with
   * `FabricMountingManager::directApplyBatch` and `LoomViewApplier`,
   * which walk the same flat array. Operations are emitted in phase order
   * (create, props, structural, layout, delete) so that every host call finds
   * the views it needs already in place.
   *
   *   0 Create      [0, tag, isLayoutable, componentName, props]
   *   1 UpdateProps [1, tag, props]
   *   2 Insert      [2, parentTag, tag, index]
   *   3 Remove      [3, parentTag, tag, index]
   *   4 Layout      [4, tag, parentTag, displayType, layoutDirection, x, y, w,
   * h] 5 Delete      [5, tag] 6 Padding     [6, tag, left, top, right, bottom]
   *   7 OverflowInset [7, tag, left, top, right, bottom]
   */
  static YGSize measureText(
      YGNodeConstRef node,
      float width,
      YGMeasureMode widthMode,
      float height,
      YGMeasureMode heightMode);
  /** Yoga measure function of a compat node: its ShadowNode's measureContent. */
  static YGSize measureCompat(
      YGNodeConstRef node,
      float width,
      YGMeasureMode widthMode,
      float height,
      YGMeasureMode heightMode);
  /**
   * Creates or re-props the node's compat ShadowNode when its component needs
   * one, and queues its state for the host when that changed.
   */
  void syncCompatNode(Node& node);

  Node* find(Tag tag);
  const Node* find(Tag tag) const;
  RawTextLeaf* findRawText(Tag tag);
  const RawTextLeaf* findRawText(Tag tag) const;
  Tag parentOfAny(Tag tag) const;
  void detachRawTextFromParent(RawTextLeaf& leaf);


  /** A stable `const char*` view of a component name. */
  const char* internComponentName(const std::string& name) const;
  /** A fragment's identity: the tag React gave it, and where it lives. */
  ShadowView shadowView(Tag tag) const;

  void mergeProps(Node& node, const folly::dynamic& patch);
  /** Applies a props patch to a live node and re-derives everything from it. */
  void patchProps(Node& node, const folly::dynamic& propsPatch);
  void refreshStyle(Node& node);
  void applyYogaProps(Node& node);

  /** Marks the paragraph this node belongs to as needing a rebuild. */
  void markTextDirty(Tag tag);
  TextAttributes attributesFor(Node& node, const TextAttributes& inherited);
  /** Rebuilds a text root's attributed string from its subtree. */
  void buildTextContent(Node& root);
  void appendTextContent(
      Node& node,
      const TextAttributes& inherited,
      AttributedString& out);
  TextMeasurement measureTextNode(
      const Node& node,
      const Size& minimumSize,
      const Size& maximumSize) const;
  /** The platform measurement alone: no cache lookup, no cache store. */
  TextMeasurement measureTextWithPlatform(
      const Node& node,
      const Size& minimumSize,
      const Size& maximumSize) const;
  void storeTextMeasurement(
      const Node& node,
      const Size& minimumSize,
      const Size& maximumSize,
      const TextMeasurement& measurement) const;
  /** Gated by `Loom.parallelText` (LoomSettings.h), read per commit. */
  void speculateTextMeasurements();
  /** Gated by `Loom.earlyCreate` (LoomSettings.h), read per commit. */
  void emitEarlyCreates();
  static bool sameTextMeasurementContent(
      const AttributedString& left,
      const AttributedString& right);
  /** The same compare, answered by pointer identity when both are one build. */
  static bool sameTextMeasurementContent(
      const std::shared_ptr<const AttributedString>& left,
      const std::shared_ptr<const AttributedString>& right) {
    if (left == right) {
      return true;
    }
    if (!left || !right) {
      return false;
    }
    return sameTextMeasurementContent(*left, *right);
  }

  void syncYogaNode(Node& node);
  void syncYogaChildren(Node& node);
  /** Recomputes the retained tree's layout. Emits nothing. */
  void layoutTree();
  /** Resolves roles and prepares the subtree while collecting preorder once. */
  void prepare(
      Node& node,
      bool insideText,
      LayoutParent parentView,
      std::vector<Tag>& traversal,
      std::vector<LayoutParent>& layoutParents,
      std::vector<Node*>& nodes);
  /** Marks `node` and the path above it for the next commit's prepare walk. */
  void markNeedsPrepare(Node& node);
  void markNeedsPrepare(Tag tag);
  /** A node that may have left the tree; checked by `collectUnreachable`. */
  void noteOrphanCandidate(Tag tag) {
    orphanCandidates_.push_back(tag);
  }
  /**
   * Post-layout walk: the nodes whose geometry may have changed, i.e. those
   * this commit prepared plus those Yoga reports a new layout for, with the
   * host parent each one is positioned in.
   */
  void collectLayoutNodes(
      Node& node,
      LayoutParent parentView,
      bool ancestorOffsetChanged);
  /** Runs one full commit: prepare, layout, then the emission phases. */
  void commit();
  /**
   * Tells JavaScript that a node's frame changed, for the nodes whose props
   * declare an `onLayout`. The shadow-tree path does the same after every
   * commit; virtualized lists size their render window from it.
   */
  void emitLayoutEvents();
  bool isSurfaceActive() const;
  void failSurface(const std::string& reason) noexcept;

  void emitCreate(Node& node);
  void emitUpdateProps(Node& node);
  static std::shared_ptr<const Props> currentCompatProps(const Node& node);
  void emitRemovals(
      Node& node,
      const std::unordered_set<uint64_t>& stationaryChildren);
  void emitInsertions(
      Node& node,
      const Tag* next,
      size_t nextCount,
      const std::unordered_set<uint64_t>& stationaryChildren);
  void emitLayout(
      Node& node,
      const LayoutParent& parent,
      const std::array<float, 4>& overflowInsets);
  void emitDelete(Tag tag);
  /** Detaches a node from whichever parent currently claims it. */
  void detachFromParent(Node& node);
  void dropSubtree(Tag tag);
  void collectUnreachable();
  void flush();
  /**
   * Whether `tag` was reached by the current commit's prepare walk, using the
   * per-node stamp rather than a visited set. Callers that already hold the node
   * compare the stamp directly; this is for the ones that only have a tag.
   */
  bool wasVisitedThisCommit(Tag tag) const;
  /**
   * Recomputes the name- and props-derived role flags cached on `node`. Call it
   * only through `roleFlagsValid`; every write to `props` or `componentName`
   * clears that.
   */
  void refreshRoleFlags(Node& node);
  /** [LoomPerf item 2] `refreshStyle` through the layout shape cache. */
  void refreshStyleCached(Node& node);
  static bool shapeValid(const Node& node) {
    return node.shape != nullptr && node.shapeGeneration == node.propsGeneration;
  }
  std::unordered_map<uint64_t, std::shared_ptr<const LayoutShape>> shapeCache_;

  SurfaceId surfaceId_;
  NativeOperations operations_;
  std::shared_ptr<LoomCompat> compat_;
  /** The host name of `ScrollView`, resolved once. */
  const std::string scrollViewName_;
  YGConfigRef yogaConfig_{nullptr};
  float pointScaleFactor_{1.0f};
  std::unordered_map<Tag, std::unique_ptr<Node>, LoomTagHash> nodes_;
  /**
   * Direct-mapped cache in front of `nodes_` for the non-const `find`: a commit
   * resolves the same few tags (a node, its parent, its children) over and
   * over, and each map lookup is a hash plus two dependent cache misses. Entries
   * hold stable `Node*`s (nodes are unique_ptr-owned) and are dropped when the
   * node is erased. Worker-confined like the rest of the tree; the const
   * `find`, which other threads may call, never touches it.
   */
  struct FindCacheEntry {
    Tag tag{0};
    Node* node{nullptr};
  };
  static constexpr size_t kFindCacheSize = 128;
  FindCacheEntry findCache_[kFindCacheSize];
  static size_t findCacheSlot(Tag tag) {
    return (static_cast<uint32_t>(tag) * 0x9E3779B1u) >> 25; // 7 bits
  }
  void invalidateFindCache(Tag tag) {
    auto& entry = findCache_[findCacheSlot(tag)];
    if (entry.node != nullptr && entry.tag == tag) {
      entry.node = nullptr;
    }
  }
  std::unordered_map<Tag, RawTextLeaf, LoomTagHash> rawTextLeaves_;
  /** Component names a `ShadowView` points at; kept alive as long as this. */
  mutable std::unordered_set<std::string> internedComponentNames_;
  /**
   * The current commit's mark, stamped onto every node `prepare` reaches (and
   * every raw text leaf it passes over). Replaces a `std::unordered_set<Tag>`
   * that had to insert - and therefore allocate - one node per tag per commit,
   * then be cleared again. Zero means "no commit yet".
   */
  uint64_t commitStamp_{0};
  /** The next commit walks everything (first commit, scale change, recovery). */
  bool fullPrepare_{true};
  std::vector<Tag> scratchMounted_;
  std::vector<size_t> scratchChildStarts_;
  std::vector<size_t> scratchChildCounts_;
  std::vector<uint8_t> scratchChildUnchanged_;
  std::unordered_set<uint64_t> scratchStationary_;
  template <class V>
  static void trimScratch(V& v) {
    if (v.capacity() > 1024 && v.size() * 4 < v.capacity()) {
      V().swap(v);
    }
  }
  void releaseSlack();
  std::vector<Tag> orphanCandidates_;
  std::vector<Tag> droppedTags_;
  std::vector<Node*> layoutNodes_;
  std::vector<LayoutParent> layoutNodeParents_;
  /**
   * Tags in insertion order, each at most once.
   *
   * The pending state updates and layout events used to be `std::vector<Tag>`
   * with a `std::find` before every `push_back`, which is quadratic in the
   * number of nodes that report layout or text state in one commit. Membership
   * is tracked here instead, so the commit stays linear and the publication
   * order is unchanged.
   */
  class TagSet {
   public:
    void push_back(Tag tag) {
      if (members_.insert(tag).second) {
        order_.push_back(tag);
      }
    }

    bool empty() const {
      return order_.empty();
    }

    size_t size() const {
      return order_.size();
    }

    void clear() {
      order_.clear();
      members_.clear();
    }

    std::vector<Tag>::const_iterator begin() const {
      return order_.begin();
    }

    std::vector<Tag>::const_iterator end() const {
      return order_.end();
    }

   private:
    std::vector<Tag> order_;
    std::unordered_set<Tag> members_;
  };

  /** Text roots whose state has to be published after the batch. */
  TagSet stateUpdates_;
  /** Nodes whose frame changed and whose props asked for `onLayout`. */
  TagSet layoutEvents_;
  Tag rootTag_{0};
  /** `applyAnimationFrame` is committing: its batch is an animation frame. */
  bool animationFrameCommit_{false};
  float minWidth_{0};
  float minHeight_{0};
  float maxWidth_{0};
  float maxHeight_{0};
  bool isRTL_{false};
  /** True after at least one valid constraint set has produced layout. */
  bool hasCommittedLayout_{false};
  /** Constraints already written onto the root's Yoga style. */
  float laidOutMinWidth_{-1};
  float laidOutMinHeight_{-1};
  float laidOutMaxWidth_{-1};
  float laidOutMaxHeight_{-1};
  /** Guards against a host callback re-entering the commit machinery. */
  bool insideCommit_{false};
  bool surfaceFailed_{false};
  bool commitPending_{false};
  bool unsupportedInlineAttachment_{false};
  struct SpeculativeTextRequest {
    const Node* node;
    Size minimumSize;
    Size maximumSize;
  };
  // Set only during the estimating Yoga pass of `speculateTextMeasurements`:
  // text measure calls record their constraints here and return an estimate.
  mutable std::vector<SpeculativeTextRequest>* speculativeTextRequests_{nullptr};
  LoomMountBatch pendingOperations_;
  std::vector<RemovedMountedEdge> removedMountedEdges_;
  /**
   * Membership for `removedMountedEdges_`, keyed by the pair the list is
   * deduplicated on. Kept next to the list and cleared with it, like
   * `removedNodeTagSet_` below; the alternative was an `any_of` scan before
   * every recorded edge, which is quadratic in the number of removed edges.
   */
  std::unordered_set<uint64_t> removedMountedEdgeKeys_;
  std::vector<Tag> removedNodeTags_;
  std::unordered_set<Tag> removedNodeTagSet_;
  std::vector<Tag> commitTraversal_;
  /**
   * The `Node` for each entry in `commitTraversal_`, in the same order.
   *
   * Every emission phase used to re-resolve its tags with `find()`, so one
   * commit hashed and pointer-chased the same nodes about six times over. The
   * `Node`s are stable for the whole of `finishCommitAfterLayout`: the only
   * erasure is `dropSubtree`, and its sole caller is `collectUnreachable`, which
   * runs after every phase that reads this. `prepare` already holds the
   * reference it fills in here, so keeping the two vectors in lockstep costs one
   * push_back and removes a hash lookup per node per phase.
   */
  std::vector<Node*> commitNodes_;
  std::vector<LayoutParent> commitLayoutParents_;
  bool commitPrepared_{false};
  LoomWorkerProfile::Clock::time_point workerCommitStart_{};
  LoomWorkerProfile::Clock::time_point workerPreparedAt_{};
  LoomWorkerProfile::Clock::time_point workerLaidOutAt_{};
#if LOOM_PROFILE
  std::chrono::steady_clock::time_point profileCommitStart_{};
  std::chrono::steady_clock::time_point profileAfterPrepare_{};
  std::chrono::steady_clock::time_point profileAfterLayout_{};
#endif
#if LOOM_PROFILE
  mutable uint64_t profileTextMeasureCalls_{0};
  mutable uint64_t profileTextMeasureCacheHits_{0};
  mutable uint64_t profileTextMeasureNanoseconds_{0};
  mutable uint64_t profileTextSpannableBuildNanos_{0};
  mutable uint64_t profileTextLayoutCreateNanos_{0};
  mutable uint64_t profileTextMeasurementResultNanos_{0};
  uint64_t profileStateUpdateCalls_{0};
  uint64_t profileStateUpdateNanoseconds_{0};
  uint64_t profileApplyBatchNanoseconds_{0};
  uint64_t profilePreparedTextStateCount_{0};
  mutable uint64_t profileTextPaintSetupNanos_{0};
  mutable uint64_t profileTextIsBoringNanos_{0};
  mutable uint64_t profileTextBoringMakeNanos_{0};
  mutable uint64_t profileTextBoringLayouts_{0};
  mutable uint64_t profileTextStaticLayouts_{0};
  mutable uint64_t profileSimplePlainTextLayouts_{0};
#endif
};

} // namespace facebook::react

#endif // LOOM_RENDERER
