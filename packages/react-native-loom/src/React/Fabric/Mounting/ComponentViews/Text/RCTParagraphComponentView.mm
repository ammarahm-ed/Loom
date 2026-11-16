/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import "RCTParagraphComponentView.h"
#import "RCTParagraphComponentAccessibilityProvider.h"

#import <MobileCoreServices/UTCoreTypes.h>
#import <react/featureflags/ReactNativeFeatureFlags.h>
#import <react/renderer/components/text/ParagraphComponentDescriptor.h>
#import <react/renderer/components/text/ParagraphProps.h>
#import <react/renderer/components/text/ParagraphState.h>
#import <react/renderer/components/text/RawTextComponentDescriptor.h>
#import <react/renderer/components/text/TextComponentDescriptor.h>
#import <react/renderer/runtimescheduler/LoomSettings.h>
#import <react/renderer/textlayoutmanager/RCTAttributedTextUtils.h>
#import <react/renderer/textlayoutmanager/RCTTextLayoutManager.h>
#import <react/renderer/textlayoutmanager/TextLayoutManager.h>
#import <react/utils/ManagedObjectWrapper.h>

#import <atomic>
#import "RCTConversions.h"
#import "RCTFabricComponentsPlugins.h"

using namespace facebook::react;

// Loom: rasterize paragraph text on a background queue instead of in drawRect: on the main thread
// (the Texture / AsyncDisplayKit technique). Set to 0 to always draw synchronously.
#ifndef LOOM_IOS_ASYNC_TEXT
#if LOOM_RENDERER
#define LOOM_IOS_ASYNC_TEXT 1
#else
#define LOOM_IOS_ASYNC_TEXT 0
#endif
#endif
// Whether a view's first drawing is asynchronous too (its text appears a frame after the view).
#ifndef LOOM_IOS_ASYNC_TEXT_FIRST_DRAW
#define LOOM_IOS_ASYNC_TEXT_FIRST_DRAW 1
#endif

@interface RCTTextLayoutManager (RCTParagraphComponentViewPrivate)

- (CGRect)drawingFrameForAttributedString:(facebook::react::AttributedString)attributedString
                      paragraphAttributes:(facebook::react::ParagraphAttributes)paragraphAttributes
                                    frame:(CGRect)frame
                           containerFrame:(CGRect *)containerFrame;

#if LOOM_IOS_ASYNC_TEXT
- (void)drawAttributedStringInBackground:(const facebook::react::AttributedString &)attributedString
                     paragraphAttributes:(facebook::react::ParagraphAttributes)paragraphAttributes
                                   frame:(CGRect)frame;
#endif

@end

#if LOOM_IOS_ASYNC_TEXT
#define LOOM_TEXT_NEEDS_DISPLAY(view) [view loomMarkNeedsDisplay]
#else
#define LOOM_TEXT_NEEDS_DISPLAY(view) [view setNeedsDisplay]
#endif

static BOOL RCTAttributedStringHasAttachments(const AttributedString &attributedString)
{
  for (const auto &fragment : attributedString.getFragments()) {
    if (fragment.isAttachment()) {
      return YES;
    }
  }
  return NO;
}

// ParagraphTextView is an auxiliary view we set as contentView so the drawing
// can happen on top of the layers manipulated by RCTViewComponentView (the parent view)
@interface RCTParagraphTextView : UIView

@property (nonatomic) ParagraphShadowNode::ConcreteState::Shared state;
@property (nonatomic) ParagraphAttributes paragraphAttributes;
@property (nonatomic) LayoutMetrics layoutMetrics;
@property (nonatomic) CGRect drawingFrame;

#if LOOM_IOS_ASYNC_TEXT
// Selectable text keeps drawing synchronously.
@property (nonatomic) BOOL selectable;
// Asks for the text to be drawn again: asynchronously when that is safe, otherwise through drawRect:.
// The drawing happens in loomFlushDisplay, once the final frame of the view is known.
- (void)loomMarkNeedsDisplay;
- (void)loomFlushDisplay;
// The view is being recycled: drop what it shows and any drawing in flight.
- (void)loomReset;
#endif

@end

#if !TARGET_OS_TV
@interface RCTParagraphComponentView () <UIEditMenuInteractionDelegate>

@property (nonatomic, nullable) UIEditMenuInteraction *editMenuInteraction API_AVAILABLE(ios(16.0));

@end
#else
@interface RCTParagraphComponentView ()
@end
#endif

@implementation RCTParagraphComponentView {
  ParagraphAttributes _paragraphAttributes;
  RCTParagraphComponentAccessibilityProvider *_accessibilityProvider;
  UILongPressGestureRecognizer *_longPressGestureRecognizer;
  RCTParagraphTextView *_textView;
  CGRect _textLayoutFrame;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  if (self = [super initWithFrame:frame]) {
    _props = ParagraphShadowNode::defaultSharedProps();

    self.opaque = NO;
    _textView = [RCTParagraphTextView new];
    _textView.backgroundColor = UIColor.clearColor;
    _textView.drawingFrame = self.bounds;
    self.contentView = _textView;
  }

  return self;
}

- (NSString *)description
{
  NSString *superDescription = [super description];

  // Cutting the last `>` character.
  if (superDescription.length > 0 && [superDescription characterAtIndex:superDescription.length - 1] == '>') {
    superDescription = [superDescription substringToIndex:superDescription.length - 1];
  }

  return [NSString stringWithFormat:@"%@; attributedText = %@>", superDescription, self.attributedText];
}

- (NSAttributedString *_Nullable)attributedText
{
  if (!_textView.state) {
    return nil;
  }

  return RCTNSAttributedStringFromAttributedString(_textView.state->getData().attributedString);
}

#pragma mark - RCTComponentViewProtocol

+ (ComponentDescriptorProvider)componentDescriptorProvider
{
  return concreteComponentDescriptorProvider<ParagraphComponentDescriptor>();
}

+ (std::vector<facebook::react::ComponentDescriptorProvider>)supplementalComponentDescriptorProviders
{
  return {
      concreteComponentDescriptorProvider<RawTextComponentDescriptor>(),
      concreteComponentDescriptorProvider<TextComponentDescriptor>()};
}

- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps
{
  const auto &oldParagraphProps = static_cast<const ParagraphProps &>(*_props);
  const auto &newParagraphProps = static_cast<const ParagraphProps &>(*props);

  _paragraphAttributes = newParagraphProps.paragraphAttributes;
  _textView.paragraphAttributes = _paragraphAttributes;
#if LOOM_IOS_ASYNC_TEXT
  _textView.selectable = newParagraphProps.isSelectable;
#endif

  if (newParagraphProps.isSelectable != oldParagraphProps.isSelectable) {
    if (newParagraphProps.isSelectable) {
      [self enableContextMenu];
    } else {
      [self disableContextMenu];
    }
  }

  [super updateProps:props oldProps:oldProps];
}

- (void)updateState:(const State::Shared &)state oldState:(const State::Shared &)oldState
{
  auto newState = std::static_pointer_cast<const ParagraphShadowNode::ConcreteState>(state);
  // A relayout yields a new state even for unchanged text (the attributed
  // string records fragment layout metrics); redraw only when content or
  // paragraph attributes changed. Attachments are sized from those metrics.
  BOOL needsDisplay = YES;
  if (newState && _textView.state) {
    const auto &newData = newState->getData();
    // Compare with the state the view last drew, which recycling can make differ from `oldState`.
    const auto &oldData = _textView.state->getData();
    needsDisplay = !newData.attributedString.isContentEqual(oldData.attributedString) ||
        newData.paragraphAttributes != oldData.paragraphAttributes ||
        RCTAttributedStringHasAttachments(newData.attributedString);
  }
  _textView.state = newState;
  if (needsDisplay) {
    LOOM_TEXT_NEEDS_DISPLAY(_textView);
  }
}

- (void)updateLayoutMetrics:(const LayoutMetrics &)layoutMetrics
           oldLayoutMetrics:(const LayoutMetrics &)oldLayoutMetrics
{
  // Drawing depends on size, content frame and scale, not on position.
  BOOL needsDisplay = _layoutMetrics.frame.size != layoutMetrics.frame.size ||
      _layoutMetrics.getContentFrame() != layoutMetrics.getContentFrame() ||
      _layoutMetrics.pointScaleFactor != layoutMetrics.pointScaleFactor;

  // Using stored `_layoutMetrics` as `oldLayoutMetrics` here to avoid
  // re-applying individual sub-values which weren't changed.
  [super updateLayoutMetrics:layoutMetrics oldLayoutMetrics:_layoutMetrics];
  _textView.layoutMetrics = _layoutMetrics;
  if (needsDisplay) {
    LOOM_TEXT_NEEDS_DISPLAY(_textView);
  }
}

- (void)finalizeUpdates:(RNComponentViewUpdateMask)updateMask
{
  [super finalizeUpdates:updateMask];
  if ((updateMask & (RNComponentViewUpdateMaskState | RNComponentViewUpdateMaskLayoutMetrics)) != 0) {
    [self _updateTextViewFrame];
  }
#if LOOM_IOS_ASYNC_TEXT
  [_textView loomFlushDisplay];
#endif
}

- (void)prepareForRecycle
{
  [super prepareForRecycle];
  _textView.state = nullptr;
#if LOOM_IOS_ASYNC_TEXT
  [_textView loomReset];
#endif
  _accessibilityProvider = nil;
}

- (void)_updateTextViewFrame
{
  CGRect textViewFrame = self.bounds;
  CGRect drawingFrame = RCTCGRectFromRect(_layoutMetrics.getContentFrame());

  if (ReactNativeFeatureFlags::enableIOSCompressedTextFrameAdjustment() && _textView.state &&
      drawingFrame.size.height > 0) {
    const auto &stateData = _textView.state->getData();
    auto textLayoutManager = stateData.layoutManager.lock();
    if (textLayoutManager) {
      RCTTextLayoutManager *nativeTextLayoutManager =
          (RCTTextLayoutManager *)unwrapManagedObject(textLayoutManager->getNativeTextLayoutManager());
      CGRect drawingContainerFrame = drawingFrame;
      drawingFrame = [nativeTextLayoutManager drawingFrameForAttributedString:stateData.attributedString
                                                          paragraphAttributes:_paragraphAttributes
                                                                        frame:drawingFrame
                                                               containerFrame:&drawingContainerFrame];
      textViewFrame = CGRectUnion(textViewFrame, drawingContainerFrame);
    }
  }

  _textLayoutFrame = drawingFrame;
  _textView.frame = textViewFrame;
  _textView.drawingFrame = CGRectOffset(drawingFrame, -textViewFrame.origin.x, -textViewFrame.origin.y);
}

#pragma mark - Accessibility

- (NSString *)accessibilityLabel
{
  NSString *label = super.accessibilityLabel;
  if ([label length] > 0) {
    return label;
  }
  return self.attributedText.string;
}

- (NSString *)accessibilityLabelForCoopting
{
  return self.accessibilityLabel;
}

- (BOOL)isAccessibilityElement
{
  // All accessibility functionality of the component is implemented in `accessibilityElements` method below.
  // Hence to avoid calling all other methods from `UIAccessibilityContainer` protocol (most of them have default
  // implementations), we return here `NO`.
  return NO;
}

- (NSArray *)accessibilityElements
{
  const auto &paragraphProps = static_cast<const ParagraphProps &>(*_props);

  // If the component is not `accessible`, we return an empty array.
  // We do this because logically all nested <Text> components represent the content of the <Paragraph> component;
  // in other words, all nested <Text> components individually have no sense without the <Paragraph>.
  if (!_textView.state || !paragraphProps.accessible) {
    return [NSArray new];
  }

  auto &data = _textView.state->getData();

  if (![_accessibilityProvider isUpToDate:data.attributedString]) {
    auto textLayoutManager = data.layoutManager.lock();
    if (textLayoutManager) {
      RCTTextLayoutManager *nativeTextLayoutManager =
          (RCTTextLayoutManager *)unwrapManagedObject(textLayoutManager->getNativeTextLayoutManager());
      CGRect frame = _textLayoutFrame;
      _accessibilityProvider =
          [[RCTParagraphComponentAccessibilityProvider alloc] initWithString:data.attributedString
                                                               layoutManager:nativeTextLayoutManager
                                                         paragraphAttributes:data.paragraphAttributes
                                                                       frame:frame
                                                                        view:self];
    }
  }

  NSArray<UIAccessibilityElement *> *elements = _accessibilityProvider.accessibilityElements;
  if ([elements count] > 0) {
    elements[0].isAccessibilityElement =
        elements[0].accessibilityTraits & UIAccessibilityTraitLink || ![self isAccessibilityCoopted];
  }
  return elements;
}

- (BOOL)isAccessibilityCoopted
{
  UIView *ancestor = self.superview;
  NSMutableSet<UIView *> *cooptingCandidates = [NSMutableSet new];
  while (ancestor) {
    if ([ancestor isKindOfClass:[RCTViewComponentView class]]) {
      if ([((RCTViewComponentView *)ancestor) accessibilityLabelForCoopting]) {
        // We found a label above us. That would be coopted before we would be
        return NO;
      } else if ([((RCTViewComponentView *)ancestor) wantsToCooptLabel]) {
        // We found an view that is looking to coopt a label below it
        [cooptingCandidates addObject:ancestor];
      }

      NSArray *elements = ancestor.accessibilityElements;
      if ([elements count] > 0 && [cooptingCandidates count] > 0) {
        for (NSObject *element in elements) {
          if ([element isKindOfClass:[UIView class]] && [cooptingCandidates containsObject:((UIView *)element)]) {
            return YES;
          }
        }
      }
    } else if (![ancestor isKindOfClass:[RCTViewComponentView class]] && ancestor.accessibilityLabel) {
      // Same as above, for UIView case. Cannot call this on RCTViewComponentView
      // as it is recursive and quite expensive.
      return NO;
    }
    ancestor = ancestor.superview;
  }

  return NO;
}

- (UIAccessibilityTraits)accessibilityTraits
{
  return [super accessibilityTraits] | UIAccessibilityTraitStaticText;
}

#pragma mark - RCTTouchableComponentViewProtocol

- (SharedTouchEventEmitter)touchEventEmitterAtPoint:(CGPoint)point
{
  const auto &state = _textView.state;
  if (!state) {
    return _eventEmitter;
  }

  const auto &stateData = state->getData();
  auto textLayoutManager = stateData.layoutManager.lock();

  if (!textLayoutManager) {
    return _eventEmitter;
  }

  RCTTextLayoutManager *nativeTextLayoutManager =
      (RCTTextLayoutManager *)unwrapManagedObject(textLayoutManager->getNativeTextLayoutManager());
  CGRect frame = _textLayoutFrame;

  auto eventEmitter = [nativeTextLayoutManager getEventEmitterWithAttributeString:stateData.attributedString
                                                              paragraphAttributes:_paragraphAttributes
                                                                            frame:frame
                                                                          atPoint:point];

  if (!eventEmitter) {
    return _eventEmitter;
  }

  assert(std::dynamic_pointer_cast<const TouchEventEmitter>(eventEmitter));
  return std::static_pointer_cast<const TouchEventEmitter>(eventEmitter);
}

#pragma mark - Context Menu

#if !TARGET_OS_TV
- (void)enableContextMenu
{
  _longPressGestureRecognizer = [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                                              action:@selector(handleLongPress:)];

  if (@available(iOS 16.0, *)) {
    _editMenuInteraction = [[UIEditMenuInteraction alloc] initWithDelegate:self];
    [self addInteraction:_editMenuInteraction];
  }
  [self addGestureRecognizer:_longPressGestureRecognizer];
}

- (void)disableContextMenu
{
  [self removeGestureRecognizer:_longPressGestureRecognizer];
  if (@available(iOS 16.0, *)) {
    [self removeInteraction:_editMenuInteraction];
    _editMenuInteraction = nil;
  }
  _longPressGestureRecognizer = nil;
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture
{
  if (@available(iOS 16.0, macCatalyst 16.0, *)) {
    CGPoint location = [gesture locationInView:self];
    UIEditMenuConfiguration *config = [UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:location];
    if (_editMenuInteraction) {
      [_editMenuInteraction presentEditMenuWithConfiguration:config];
    }
  } else {
    UIMenuController *menuController = [UIMenuController sharedMenuController];

    if (menuController.isMenuVisible) {
      return;
    }

    [menuController showMenuFromView:self rect:self.bounds];
  }
}

- (BOOL)canBecomeFirstResponder
{
  const auto &paragraphProps = static_cast<const ParagraphProps &>(*_props);
  return paragraphProps.isSelectable;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender
{
  const auto &paragraphProps = static_cast<const ParagraphProps &>(*_props);

  if (paragraphProps.isSelectable && action == @selector(copy:)) {
    return YES;
  }

  return [self.nextResponder canPerformAction:action withSender:sender];
}

- (void)copy:(id)sender
{
  NSAttributedString *attributedText = self.attributedText;

  NSMutableDictionary *item = [NSMutableDictionary new];

  NSData *rtf = [attributedText dataFromRange:NSMakeRange(0, attributedText.length)
                           documentAttributes:@{NSDocumentTypeDocumentAttribute : NSRTFDTextDocumentType}
                                        error:nil];

  if (rtf) {
    [item setObject:rtf forKey:(id)kUTTypeFlatRTFD];
  }

  [item setObject:attributedText.string forKey:(id)kUTTypeUTF8PlainText];

  UIPasteboard *pasteboard = [UIPasteboard generalPasteboard];
  pasteboard.items = @[ item ];
}
#else
- (void)enableContextMenu
{
}

- (void)disableContextMenu
{
}
#endif

@end

Class<RCTComponentViewProtocol> RCTParagraphCls(void)
{
  return RCTParagraphComponentView.class;
}

// Set by the surface presenter while it applies a batch that is an animation frame (see
// LoomMountBatch::animationFrame): text resized by it is drawn in this frame.
static bool gLoomTextApplyingAnimationFrame = false;

extern "C" void LoomTextSetApplyingAnimationFrame(bool animationFrame)
{
  gLoomTextApplyingAnimationFrame = animationFrame;
}

#if LOOM_IOS_ASYNC_TEXT
// Loom: asynchronous text display.
//
// When a paragraph is redrawn at the size it already has on screen (new text in the same box: a
// label, a counter, a row's content), the new text is rasterized on a background queue and the
// layer keeps showing the previous drawing until the new one is ready, normally within a frame. A
// view's first drawing is asynchronous as well (its text appears a frame after the view), unless
// that is switched off. Everything else draws synchronously in drawRect:, exactly as React Native
// does:
//   - a size change, with new text or not: the previous drawing does not fit the new geometry, and
//     this is also how text animates through redraws (LayoutAnimation, resizing);
//   - selectable text, pressed links (highlight layer), inline attachments, and any redraw UIKit asks
//     for itself (trait or scale changes).
// Transform and opacity animations never redraw text, so they are unaffected.
//
// Switches, runtime settings (`Loom.asyncText`, `Loom.asyncTextFirstDraw`, `Loom.asyncTextResize`; see
// LoomSettings.h). Startup values come from the Info.plist `Loom` dictionary, then NSUserDefaults
// (so also launch arguments, e.g. `-LoomAsyncText NO`); JavaScript can change them while running.
// A change applies to the next drawing.

namespace {
struct LoomTextDisplayCounters {
  std::atomic<uint64_t> asyncDraw{0}; // drawn on the background queue and shown
  std::atomic<uint64_t> asyncSkipped{0}; // superseded before it was drawn or shown
  std::atomic<uint64_t> syncFirst{0}; // first drawing of a view (asynchronous first draws off)
  std::atomic<uint64_t> syncResize{0}; // same content, new size or scale
  std::atomic<uint64_t> syncUnsafe{0}; // selectable, highlight, attachments, nothing to draw
  std::atomic<uint64_t> syncDisabled{0}; // asynchronous text switched off
  std::atomic<uint64_t> wideColor{0}; // background drawings that needed an extended-range bitmap
  std::atomic<uint64_t> latencySumUs{0}; // request (commit) to image set, asyncDraw only
  std::atomic<uint64_t> latencyMaxUs{0};
};
LoomTextDisplayCounters gLoomTextCounters;

// Read on every use (a relaxed load, see LoomSettings.h), so `Loom.asyncText = false` applies to
// the next drawing. The registry is filled at startup by RCTSurfacePresenter.
bool LoomAsyncTextEnabled()
{
  return facebook::react::LoomSettings::enabled(facebook::react::LoomSettingId::AsyncText);
}

bool LoomAsyncTextFirstDrawEnabled()
{
  return facebook::react::LoomSettings::enabled(facebook::react::LoomSettingId::AsyncTextFirstDraw);
}

// A one-off resize (a commit, not an animation) drawn in the background.
bool LoomAsyncTextResizeEnabled()
{
  return facebook::react::LoomSettings::enabled(facebook::react::LoomSettingId::AsyncTextResize);
}
} // namespace

// Read by the bench probe, in this order: asyncDraw, asyncSkipped, syncFirst, syncResize,
// syncUnsafe, syncDisabled, wideColor, latencySumUs, latencyMaxUs; and the switches.
extern "C" void LoomTextDisplayCountersRead(uint64_t out[9])
{
  out[0] = gLoomTextCounters.asyncDraw.load();
  out[1] = gLoomTextCounters.asyncSkipped.load();
  out[2] = gLoomTextCounters.syncFirst.load();
  out[3] = gLoomTextCounters.syncResize.load();
  out[4] = gLoomTextCounters.syncUnsafe.load();
  out[5] = gLoomTextCounters.syncDisabled.load();
  out[6] = gLoomTextCounters.wideColor.load();
  out[7] = gLoomTextCounters.latencySumUs.load();
  out[8] = gLoomTextCounters.latencyMaxUs.load();
}

extern "C" void LoomTextDisplaySwitchesRead(bool out[2])
{
  out[0] = LoomAsyncTextEnabled();
  out[1] = LoomAsyncTextFirstDrawEnabled();
}

// Shared with the drawing blocks: the generation of the drawing a view wants on screen.
@interface LoomTextDisplayToken : NSObject {
 @public
  std::atomic<uint64_t> generation;
}
@end

@implementation LoomTextDisplayToken
@end

static dispatch_queue_t LoomTextDisplayQueue(void)
{
  // Two serial queues, used in turn: a screen of new text appears about twice as fast as with one,
  // without spreading over every core. Drawings superseded while queued are skipped, so a burst of
  // updates costs one drawing per view rather than one per update. A result is applied only if it
  // is still the latest for its view, so the order between the queues does not matter.
  constexpr int kQueues = 2;
  static dispatch_queue_t queues[kQueues];
  static dispatch_once_t once;
  static std::atomic<uint32_t> next{0};
  dispatch_once(&once, ^{
    dispatch_queue_attr_t attr =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    for (int i = 0; i < kQueues; i++) {
      queues[i] = dispatch_queue_create("com.loom.text-display", attr);
    }
  });
  return queues[next.fetch_add(1, std::memory_order_relaxed) % kQueues];
}

// Where a drawing made at the previous size sits in the resized view until its replacement is
// drawn: pinned to the text's alignment instead of stretched.
static UIViewContentMode LoomTextContentMode(const AttributedString &attributedString, LayoutDirection direction)
{
  const auto alignment = attributedString.getBaseTextAttributes().alignment.value_or(TextAlignment::Natural);
  const bool rightToLeft = direction == LayoutDirection::RightToLeft;
  switch (alignment) {
    case TextAlignment::Center:
      return UIViewContentModeTop;
    case TextAlignment::Right:
      return UIViewContentModeTopRight;
    case TextAlignment::Left:
      return UIViewContentModeTopLeft;
    default:
      return rightToLeft ? UIViewContentModeTopRight : UIViewContentModeTopLeft;
  }
}

static BOOL LoomTextNeedsSynchronousDraw(const AttributedString &attributedString)
{
  // Highlighted ranges (pressed links) add a highlight layer from the draw; inline views are laid
  // out from the fragment metrics.
  for (const auto &fragment : attributedString.getFragments()) {
    if (fragment.isAttachment() || fragment.textAttributes.isHighlighted.value_or(false)) {
      return YES;
    }
  }
  return NO;
}

static BOOL LoomColorIsWide(const SharedColor &color)
{
  if (!color) {
    return NO;
  }
  CGColorRef cgColor = RCTUIColorFromSharedColor(color).CGColor;
  if (cgColor == nullptr) {
    return NO;
  }
  CGColorSpaceRef space = CGColorGetColorSpace(cgColor);
  CFStringRef name = space != nullptr ? CGColorSpaceGetName(space) : nullptr;
  if (name != nullptr &&
      (CFEqual(name, kCGColorSpaceSRGB) || CFEqual(name, kCGColorSpaceGenericGrayGamma2_2) ||
       CFEqual(name, kCGColorSpaceExtendedSRGB) || CFEqual(name, kCGColorSpaceExtendedGray))) {
    // UIKit's RGB colors live in extended sRGB; they fit an 8-bit bitmap while in [0, 1].
    const size_t count = CGColorGetNumberOfComponents(cgColor);
    const CGFloat *components = CGColorGetComponents(cgColor);
    for (size_t i = 0; i + 1 < count; i++) {
      if (components[i] < 0 || components[i] > 1) {
        return YES;
      }
    }
    return NO;
  }
  // Display P3 and other spaces: keep the extended range so the colors are not clamped.
  return YES;
}

static BOOL LoomTextNeedsWideColor(const AttributedString &attributedString)
{
  for (const auto &fragment : attributedString.getFragments()) {
    const auto &attributes = fragment.textAttributes;
    if (LoomColorIsWide(attributes.foregroundColor) || LoomColorIsWide(attributes.backgroundColor) ||
        LoomColorIsWide(attributes.textDecorationColor) || LoomColorIsWide(attributes.textShadowColor)) {
      return YES;
    }
  }
  return NO;
}
#endif

#if LOOM_IOS_ASYNC_TEXT
// Draws a paragraph into an image off the main thread: an 8-bit bitmap unless a color needs more,
// as UIKit's own backing stores do (an extended-range bitmap is twice the memory and the work).
static UIImage *LoomRenderTextImage(
    RCTTextLayoutManager *textLayoutManager,
    const AttributedString &attributedString,
    const ParagraphAttributes &paragraphAttributes,
    CGRect frame,
    CGSize size,
    CGFloat scale)
{
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = scale;
  format.opaque = NO;
  if (LoomTextNeedsWideColor(attributedString)) {
    gLoomTextCounters.wideColor.fetch_add(1);
  } else {
    format.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
  }
  UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size format:format];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext *_Nonnull) {
    [textLayoutManager drawAttributedStringInBackground:attributedString
                                    paragraphAttributes:paragraphAttributes
                                                  frame:frame];
  }];
}

#endif

@implementation RCTParagraphTextView {
  CAShapeLayer *_highlightLayer;
#if LOOM_IOS_ASYNC_TEXT
  LoomTextDisplayToken *_loomToken;
  BOOL _loomDirty;
  // What the layer shows: a drawing made at this size and scale (none: nothing drawn yet).
  BOOL _loomShown;
  CGSize _loomShownSize;
  CGFloat _loomShownScale;
  // When the view's size last changed (a size change in the frames after it draws synchronously).
  CFTimeInterval _loomLastResizeTime;
#endif
}

#if LOOM_IOS_ASYNC_TEXT
- (void)loomMarkNeedsDisplay
{
  _loomDirty = YES;
}

- (void)loomReset
{
  _loomDirty = NO;
  _loomShown = NO;
  if (_loomToken) {
    _loomToken->generation.fetch_add(1);
  }
  self.layer.contents = nil;
}

- (void)loomRecordShownSize:(CGSize)size scale:(CGFloat)scale
{
  _loomShown = YES;
  _loomShownSize = size;
  _loomShownScale = scale;
}

- (void)loomDrawSynchronously:(std::atomic<uint64_t> &)counter
{
  counter.fetch_add(1);
  [self setNeedsDisplay];
}

- (void)loomFlushDisplay
{
  const CGSize size = self.bounds.size;
  if (!_loomDirty || _state == nullptr || size.width <= 0 || size.height <= 0) {
    // Nothing to draw before the view has text and a size; it is drawn once it has both.
    return;
  }
  _loomDirty = NO;
  const CGFloat scale = self.traitCollection.displayScale > 0 ? self.traitCollection.displayScale : UIScreen.mainScreen.scale;

  // The drawing on screen can stay up until its replacement is ready only while the view still has
  // the geometry it was drawn for. Once the size changed (with new text, or animating), the old
  // drawing would be stretched or sit at the wrong place for a frame, so the view is drawn in this
  // frame instead.
  if (!LoomAsyncTextEnabled()) {
    [self loomDrawSynchronously:gLoomTextCounters.syncDisabled];
    return;
  }
  if (!_loomShown) {
    if (!LoomAsyncTextFirstDrawEnabled()) {
      [self loomDrawSynchronously:gLoomTextCounters.syncFirst];
      return;
    }
  } else if (!CGSizeEqualToSize(size, _loomShownSize) || scale != _loomShownScale) {
    // A size change is drawn in this frame when text is animating: in an animation frame, or when
    // the size changed in the last frames too (a JS-driven animation); the previous drawing would
    // otherwise trail the view by a frame on every frame. A one-off change (a commit) is drawn in
    // the background, the previous drawing pinned to the text's alignment for that frame.
    const CFTimeInterval now = CACurrentMediaTime();
    const bool recentlyResized = now - _loomLastResizeTime < 0.05;
    _loomLastResizeTime = now;
    if (gLoomTextApplyingAnimationFrame || recentlyResized || scale != _loomShownScale ||
        !LoomAsyncTextResizeEnabled()) {
      [self loomDrawSynchronously:gLoomTextCounters.syncResize];
      return;
    }
    self.contentMode = LoomTextContentMode(_state->getData().attributedString, _layoutMetrics.layoutDirection);
  }
  std::shared_ptr<const TextLayoutManager> textLayoutManager;
  BOOL asyncSafe = !_selectable && _highlightLayer == nil && _drawingFrame.size.width > 0 &&
      _drawingFrame.size.height > 0 && !LoomTextNeedsSynchronousDraw(_state->getData().attributedString);
  if (asyncSafe) {
    textLayoutManager = _state->getData().layoutManager.lock();
    asyncSafe = textLayoutManager != nullptr;
  }
  if (!asyncSafe) {
    [self loomDrawSynchronously:gLoomTextCounters.syncUnsafe];
    return;
  }

  if (!_loomToken) {
    _loomToken = [LoomTextDisplayToken new];
  }
  const uint64_t generation = _loomToken->generation.fetch_add(1) + 1;
  RCTTextLayoutManager *nativeTextLayoutManager =
      (RCTTextLayoutManager *)unwrapManagedObject(textLayoutManager->getNativeTextLayoutManager());
  const auto state = _state; // immutable; keeps the attributed string alive for the queue
  const ParagraphAttributes paragraphAttributes = _paragraphAttributes;
  const CGRect frame = _drawingFrame;
  LoomTextDisplayToken *token = _loomToken;
  __weak RCTParagraphTextView *weakSelf = self;
  const CFTimeInterval requestedAt = CACurrentMediaTime();

  dispatch_async(LoomTextDisplayQueue(), ^{
    if (token->generation.load() != generation) {
      gLoomTextCounters.asyncSkipped.fetch_add(1);
      return; // a newer drawing was asked for
    }
    UIImage *image = LoomRenderTextImage(
        nativeTextLayoutManager, state->getData().attributedString, paragraphAttributes, frame, size, scale);
    // Keeps the layout manager alive while its native object draws.
    (void)textLayoutManager;
    dispatch_async(dispatch_get_main_queue(), ^{
      RCTParagraphTextView *strongSelf = weakSelf;
      if (strongSelf == nil || token->generation.load() != generation) {
        gLoomTextCounters.asyncSkipped.fetch_add(1);
        return;
      }
      gLoomTextCounters.asyncDraw.fetch_add(1);
      const auto latencyUs = (uint64_t)((CACurrentMediaTime() - requestedAt) * 1e6);
      gLoomTextCounters.latencySumUs.fetch_add(latencyUs);
      uint64_t max = gLoomTextCounters.latencyMaxUs.load();
      while (latencyUs > max && !gLoomTextCounters.latencyMaxUs.compare_exchange_weak(max, latencyUs)) {
      }
      [strongSelf loomRecordShownSize:size scale:scale];
      CALayer *layer = strongSelf.layer;
      layer.contentsScale = scale;
      layer.contents = (__bridge id)image.CGImage;
    });
  });
}

- (void)setNeedsDisplay
{
  // A drawRect: pass supersedes any background drawing in flight.
  if (_loomToken) {
    _loomToken->generation.fetch_add(1);
  }
  [super setNeedsDisplay];
}
#endif

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  return nil;
}

- (void)drawRect:(CGRect)rect
{
#if LOOM_IOS_ASYNC_TEXT
  [self loomRecordShownSize:self.bounds.size scale:self.layer.contentsScale];
#endif
  if (!_state) {
    return;
  }

  const auto &stateData = _state->getData();
  auto textLayoutManager = stateData.layoutManager.lock();
  if (!textLayoutManager) {
    return;
  }

  RCTTextLayoutManager *nativeTextLayoutManager =
      (RCTTextLayoutManager *)unwrapManagedObject(textLayoutManager->getNativeTextLayoutManager());

  CGRect frame = _drawingFrame;

  [nativeTextLayoutManager drawAttributedString:stateData.attributedString
                            paragraphAttributes:_paragraphAttributes
                                          frame:frame
                              drawHighlightPath:^(UIBezierPath *highlightPath) {
                                if (highlightPath) {
                                  if (!self->_highlightLayer) {
                                    self->_highlightLayer = [CAShapeLayer layer];
                                    self->_highlightLayer.fillColor = [UIColor colorWithWhite:0 alpha:0.25].CGColor;
                                    [self.layer addSublayer:self->_highlightLayer];
                                  }
                                  self->_highlightLayer.position = frame.origin;
                                  self->_highlightLayer.path = highlightPath.CGPath;
                                } else {
                                  [self->_highlightLayer removeFromSuperlayer];
                                  self->_highlightLayer = nil;
                                }
                              }];
}

@end
