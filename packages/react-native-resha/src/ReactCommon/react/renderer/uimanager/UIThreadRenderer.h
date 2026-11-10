/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
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
#include <react/renderer/uimanager/UIThreadDirectMountBatch.h>
#include <react/renderer/uimanager/UIThreadEventManager.h>
#include <react/renderer/uimanager/UIThreadRendererWorkerProfile.h>
#include <react/renderer/uimanager/UIThreadTextMeasurement.h>
#include <yoga/Yoga.h>

namespace facebook::react {

/**
 * The layout-relevant style props this renderer understands, keyed so that the
 * Yoga style can be reset for every key a revision does not set.
 */
enum class UIThreadStyleKey : uint8_t {
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
 * A direct JS-to-Yoga-to-host-view renderer used only by UI_THREAD_RENDERER.
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
 * (`docs/resha/animation-fast-lane-design.md` §3.2).
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

class UIThreadRenderer final {
 public:
  struct TextMeasurement {
    float width{0};
    float height{0};
    bool valid{false};
    UIThreadTextMeasurement prepared{};
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
    std::function<void(SurfaceId, UIThreadDirectMountBatch)> applyBatch;
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
    std::function<UIThreadTextMeasurement(
        SurfaceId,
        const AttributedString&,
        const ParagraphAttributes&,
        const Size& minimumSize,
        const Size& maximumSize)>
        prepareText;
    /** Builds current visual text state for mounting without measuring it for
     * Yoga. */
    std::function<UIThreadTextMeasurement(
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
     * Publishes the component's StateWrapper payload.  The direct renderer
     * does not assume that every state is paragraph state: the host selects
     * the component-specific StateData while preserving the normal
     * ViewManager.updateState contract.
     */
    std::function<void(
        SurfaceId,
        Tag,
        const std::string& componentName,
        const folly::dynamic& props,
        const AttributedString&,
        const ParagraphAttributes&,
        const UIThreadTextMeasurement&)>
        updateState;
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

  UIThreadRenderer(SurfaceId surfaceId, NativeOperations operations);
  ~UIThreadRenderer();
  UIThreadRenderer(const UIThreadRenderer&) = delete;
  UIThreadRenderer& operator=(const UIThreadRenderer&) = delete;

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
      const std::unordered_map<Tag, uint64_t>& selectedIdentities);
  /** `propsPatch` is an attribute-payload diff, exactly as React produced it.
   */
  void cloneNodeWithNewProps(Tag tag, const folly::dynamic& propsPatch);
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
  /**
   * Borrowed form of `componentName` for the same reason. Null when the node is
   * unknown. Invalidated by any mutation.
   */
  const std::string* componentNameRef(Tag tag) const;
  bool isConnected(Tag tag) const;
  Tag findByNativeId(const std::string& nativeId) const;
  std::string componentName(Tag tag) const;
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

  /** Applies a props patch outside a commit, for `setNativeProps`. */
  void setNativeProps(Tag tag, const folly::dynamic& propsPatch);
  /** Stops this surface after host publication or reconciliation fails. */
  void abortCommit(const std::string& reason) noexcept;
  bool hasFailed() const;
  /** Returns the first ordinary view nested below a text root, or tag 0. */
  static Tag findUnsupportedInlineAttachment(
      const std::vector<Tag>& rootChildren,
      const std::function<const std::string*(Tag)>& componentNameFor,
      const std::function<const std::vector<Tag>* (Tag)>& childrenFor);

 private:
  /**
   * The native suite drives `measureTextNode` through this named accessor. The
   * measurement cache can only be shown to work by asking for one constraint
   * pair, then another, then the first again, and the method that answers is
   * private to this class.
   */
  friend struct UIThreadRendererTestAccess;

  using StyleKey = UIThreadStyleKey;

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

  struct Node {
    Tag tag{0};
    uint64_t identity{0};
    Tag parent{0};
    /** Component name as JS sent it (`RCTText`, `RCTView`, ...). */
    std::string componentName;
    /** Merged, flattened props. Never a diff. */
    folly::dynamic props{folly::dynamic::object()};
    /**
     * Props changed since the host last saw them. The host is always sent the
     * whole merged set, which is what the shadow-tree path mounts too, so a
     * prop that a patch removed is reset instead of lingering.
     */
    bool propsDirty{false};
    /**
     * Bumped whenever `props` changes. Two funnels mutate props — `mergeProps`
     * for a patch and `createNode` for the initial set — so this is a reliable
     * key for anything derived from them.
     */
    uint64_t propsGeneration{0};
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
    /** Text attributes with the inherited chain already applied. */
    TextAttributes textAttributes{};
    /** Paragraph attributes; only filled for text roots. */
    ParagraphAttributes paragraphAttributes{};
    /** Text content of this subtree. */
    AttributedString attributedString{};
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
    AttributedString measurementAttributedString{};
    /** Last successful measure; avoids repeated bridge calls during one tree
     * layout. */
    mutable bool hasTextMeasurementCache{false};
    /** Text attributes used to key Yoga measurement; paint-only fields are
     * ignored. */
    mutable AttributedString measuredAttributedString{};
    mutable ParagraphAttributes measuredParagraphAttributes{};
    mutable Size measuredMinimumSize{};
    mutable Size measuredMaximumSize{};
    mutable float measuredPointScaleFactor{1.0f};
    mutable TextMeasurement measuredText{};
    /** Full attributes represented by measuredText.prepared for mounting. */
    mutable AttributedString preparedAttributedString{};
    mutable ParagraphAttributes preparedParagraphAttributes{};
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
    mutable std::array<TextMeasurementEntry, kTextMeasurementCacheEntries>
        recentTextMeasurements_{};
    mutable size_t recentTextMeasurementCount_{0};

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
    bool roleIsText{false};
    bool roleIsParagraph{false};
    bool roleIsTextInput{false};
    bool roleRequiresIntrinsicMeasurement{false};
    bool roleRequiresGenericStateWrapper{false};
    bool roleIsLayoutOnlyView{false};
    UIThreadRenderer* owner{nullptr};
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
   * `FabricMountingManager::directApplyBatch` and `UIThreadDirectViewApplier`,
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

  Node* find(Tag tag);
  const Node* find(Tag tag) const;
  RawTextLeaf* findRawText(Tag tag);
  const RawTextLeaf* findRawText(Tag tag) const;
  Tag parentOfAny(Tag tag) const;
  void detachRawTextFromParent(RawTextLeaf& leaf);

  static bool isTextComponentName(const std::string& componentName);
  static bool isRawTextComponentName(const std::string& componentName);
  static std::string hostComponentName(const std::string& componentName);

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
  static bool sameTextMeasurementContent(
      const AttributedString& left,
      const AttributedString& right);

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

  SurfaceId surfaceId_;
  NativeOperations operations_;
  YGConfigRef yogaConfig_{nullptr};
  float pointScaleFactor_{1.0f};
  std::unordered_map<Tag, std::unique_ptr<Node>> nodes_;
  std::unordered_map<Tag, RawTextLeaf> rawTextLeaves_;
  /** Component names a `ShadowView` points at; kept alive as long as this. */
  mutable std::unordered_set<std::string> internedComponentNames_;
  /**
   * The current commit's mark, stamped onto every node `prepare` reaches (and
   * every raw text leaf it passes over). Replaces a `std::unordered_set<Tag>`
   * that had to insert - and therefore allocate - one node per tag per commit,
   * then be cleared again. Zero means "no commit yet".
   */
  uint64_t commitStamp_{0};
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
  UIThreadDirectMountBatch pendingOperations_;
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
  UIThreadRendererWorkerProfile::Clock::time_point workerCommitStart_{};
  UIThreadRendererWorkerProfile::Clock::time_point workerPreparedAt_{};
  UIThreadRendererWorkerProfile::Clock::time_point workerLaidOutAt_{};
#if RN_PROFILE_UI_THREAD_RENDERER
  std::chrono::steady_clock::time_point profileCommitStart_{};
  std::chrono::steady_clock::time_point profileAfterPrepare_{};
  std::chrono::steady_clock::time_point profileAfterLayout_{};
#endif
#if RN_PROFILE_UI_THREAD_RENDERER
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
