/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import <UIKit/UIKit.h>
#import <memory>

#import <react/renderer/componentregistry/ComponentDescriptorFactory.h>
#import <react/renderer/core/ComponentDescriptor.h>
#import <react/renderer/core/EventListener.h>
#import <react/renderer/core/LayoutConstraints.h>
#import <react/renderer/core/LayoutContext.h>
#import <react/renderer/graphics/Rect.h>
#import <react/renderer/mounting/MountingCoordinator.h>
#import <react/renderer/scheduler/SchedulerToolbox.h>
#import <react/renderer/scheduler/SurfaceHandler.h>
#import <react/renderer/uimanager/UIManager.h>
#import <react/renderer/uimanager/UIThreadDirectMountBatch.h>
#import <react/renderer/uimanager/UIThreadTextMeasurement.h>
#import <react/utils/ContextContainer.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Exactly same semantic as `facebook::react::SchedulerDelegate`.
 */
@protocol RCTSchedulerDelegate

- (void)schedulerDidFinishTransaction:(std::shared_ptr<const facebook::react::MountingCoordinator>)mountingCoordinator;

- (void)schedulerShouldRenderTransactions:
    (std::shared_ptr<const facebook::react::MountingCoordinator>)mountingCoordinator;

- (void)schedulerShouldMergeReactRevision:(facebook::react::SurfaceId)surfaceId;

- (void)schedulerDidDispatchCommand:(const facebook::react::ShadowView &)shadowView
                        commandName:(const std::string &)commandName
                               args:(const folly::dynamic &)args;

- (void)schedulerDidSendAccessibilityEvent:(const facebook::react::ShadowView &)shadowView
                                 eventType:(const std::string &)eventType;

- (void)schedulerDidSetIsJSResponder:(BOOL)isJSResponder
                blockNativeResponder:(BOOL)blockNativeResponder
                       forShadowView:(const facebook::react::ShadowView &)shadowView;

- (void)schedulerDidSynchronouslyUpdateViewOnUIThread:(facebook::react::Tag)reactTag props:(folly::dynamic)props;

#if UI_THREAD_RENDERER
- (BOOL)schedulerDirectRendererCanMeasureTextOnWorker;
- (void)schedulerDirectRendererApplyBatch:(facebook::react::SurfaceId)surfaceId
                                    batch:(facebook::react::UIThreadDirectMountBatch)batch;
- (void)schedulerDirectRendererFailSurface:(facebook::react::SurfaceId)surfaceId
                                    reason:(const std::string &)reason;
- (void)schedulerDirectRendererRegisterEventTarget:(facebook::react::SurfaceId)surfaceId
                                               tag:(facebook::react::Tag)tag
                                    instanceHandle:(facebook::react::InstanceHandle::Shared)instanceHandle;
- (void)schedulerDirectRendererUnregisterEventTarget:(facebook::react::SurfaceId)surfaceId
                                                 tag:(facebook::react::Tag)tag;
- (void)schedulerDirectRendererDispatchEvent:(facebook::react::SurfaceId)surfaceId
                                         tag:(facebook::react::Tag)tag
                                       name:(const std::string &)name
                                     payload:(folly::dynamic)payload
                                    category:(facebook::react::RawEvent::Category)category
                                      unique:(BOOL)unique;
- (facebook::react::TextAttributes)schedulerDirectRendererParseTextAttributes:(facebook::react::SurfaceId)surfaceId
                                                                          name:(const std::string &)name
                                                                         props:(const folly::dynamic &)props
                                                                     inherited:(const facebook::react::TextAttributes &)inherited
                                                            paragraphAttributes:(facebook::react::ParagraphAttributes &)paragraphAttributes;
- (std::optional<facebook::react::Size>)schedulerDirectRendererMeasureText:(facebook::react::SurfaceId)surfaceId
                                                              attributedString:(const facebook::react::AttributedString &)attributedString
                                                           paragraphAttributes:(const facebook::react::ParagraphAttributes &)paragraphAttributes
                                                                  minimumSize:(const facebook::react::Size &)minimumSize
                                                                  maximumSize:(const facebook::react::Size &)maximumSize;
- (facebook::react::UIThreadTextMeasurement)schedulerDirectRendererPrepareText:(facebook::react::SurfaceId)surfaceId
                                                               attributedString:(const facebook::react::AttributedString &)attributedString
                                                            paragraphAttributes:(const facebook::react::ParagraphAttributes &)paragraphAttributes
                                                                   minimumSize:(const facebook::react::Size &)minimumSize
                                                                   maximumSize:(const facebook::react::Size &)maximumSize;
- (facebook::react::UIThreadTextMeasurement)schedulerDirectRendererPrepareTextForMount:(facebook::react::SurfaceId)surfaceId
                                                                      attributedString:(const facebook::react::AttributedString &)attributedString
                                                                   paragraphAttributes:(const facebook::react::ParagraphAttributes &)paragraphAttributes
                                                                          minimumSize:(const facebook::react::Size &)minimumSize
                                                                          maximumSize:(const facebook::react::Size &)maximumSize;
- (void)schedulerDirectRendererUpdateTextState:(facebook::react::SurfaceId)surfaceId
                                            tag:(facebook::react::Tag)tag
                                  componentName:(const std::string &)componentName
                                          props:(const folly::dynamic &)props
                                attributedString:(const facebook::react::AttributedString &)attributedString
                             paragraphAttributes:(const facebook::react::ParagraphAttributes &)paragraphAttributes;
- (void)schedulerDirectRendererUpdateScrollContentBounds:(facebook::react::SurfaceId)surfaceId
                                                     tag:(facebook::react::Tag)tag
                                                    props:(const folly::dynamic &)props
                                                   bounds:(const facebook::react::Rect &)bounds;
- (void)schedulerDirectRendererDispatchCommand:(facebook::react::SurfaceId)surfaceId
                                           tag:(facebook::react::Tag)tag
                                         name:(const std::string &)name
                                         args:(const folly::dynamic &)args;
- (void)schedulerDirectRendererAccessibilityEvent:(facebook::react::SurfaceId)surfaceId
                                               tag:(facebook::react::Tag)tag
                                              name:(const std::string &)name;
- (void)schedulerDirectRendererSetJSResponder:(facebook::react::SurfaceId)surfaceId
                                           tag:(facebook::react::Tag)tag
                                     isResponder:(BOOL)isResponder
                            blockNativeResponder:(BOOL)blockNativeResponder;
#endif
@end

/**
 * `facebook::react::Scheduler` as an Objective-C class.
 */
@interface RCTScheduler : NSObject

@property (atomic, weak, nullable) id<RCTSchedulerDelegate> delegate;
@property (readonly) const std::shared_ptr<facebook::react::UIManager> uiManager;

- (instancetype)initWithToolbox:(facebook::react::SchedulerToolbox)toolbox;

- (void)registerSurface:(const facebook::react::SurfaceHandler &)surfaceHandler;
- (void)unregisterSurface:(const facebook::react::SurfaceHandler &)surfaceHandler;

- (const facebook::react::ComponentDescriptor *)findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN:
    (facebook::react::ComponentHandle)handle;
- (facebook::react::EventDispatcher::Shared)directRendererEventDispatcher;
- (facebook::react::SharedComponentDescriptorRegistry)directRendererComponentDescriptorRegistry;

- (void)setupAnimationDriver:(const facebook::react::SurfaceHandler &)surfaceHandler;

- (void)onAnimationStarted;

- (void)onAllAnimationsComplete;

- (void)animationTick;

- (void)reportMount:(facebook::react::SurfaceId)surfaceId;

- (void)addEventListener:(const std::shared_ptr<facebook::react::EventListener> &)listener;

- (void)removeEventListener:(const std::shared_ptr<facebook::react::EventListener> &)listener;

@end

NS_ASSUME_NONNULL_END
