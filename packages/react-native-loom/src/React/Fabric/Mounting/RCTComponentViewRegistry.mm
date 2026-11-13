/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import "RCTComponentViewRegistry.h"

#import <Foundation/NSMapTable.h>
#import <React/RCTAssert.h>
#import <React/RCTConstants.h>

#import <React/RCTImageComponentView.h>
#import <React/RCTParagraphComponentView.h>
#import <React/RCTViewComponentView.h>
#import <unordered_map>

using namespace facebook;
using namespace facebook::react;

const NSInteger RCTComponentViewRegistryRecyclePoolMaxSize = 1024;
// [LoomMemory] Views a pool keeps once nothing has been recycled or reused for a while. A list that
// unmounts leaves up to MaxSize views per component type behind (UIView + CALayer each); without this
// they stay until a memory warning, so an app that scrolled a long list and went home keeps the peak.
static const NSInteger RCTComponentViewRegistryRecyclePoolIdleSize = 16;
static const int64_t RCTComponentViewRegistryRecyclePoolIdleNs = 4 * NSEC_PER_SEC;

@implementation RCTComponentViewRegistry {
  std::unordered_map<Tag, RCTComponentViewDescriptor> _registry;
  std::unordered_map<ComponentHandle, std::vector<RCTComponentViewDescriptor>> _recyclePool;
  BOOL _poolTouched;
  BOOL _trimScheduled;
}

- (instancetype)init
{
  if (self = [super init]) {
    _componentViewFactory = [RCTComponentViewFactory currentComponentViewFactory];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(handleApplicationDidReceiveMemoryWarningNotification)
                                                 name:UIApplicationDidReceiveMemoryWarningNotification
                                               object:nil];
  }

  return self;
}

- (const RCTComponentViewDescriptor &)dequeueComponentViewWithComponentHandle:(ComponentHandle)componentHandle
                                                                          tag:(Tag)tag
{
  RCTAssertMainQueue();

  RCTAssert(
      _registry.find(tag) == _registry.end(),
      @"RCTComponentViewRegistry: Attempt to dequeue already registered component.");

  auto componentViewDescriptor = [self _dequeueComponentViewWithComponentHandle:componentHandle];
  componentViewDescriptor.view.tag = tag;
  auto it = _registry.insert({tag, componentViewDescriptor});
  return it.first->second;
}

- (void)enqueueComponentViewWithComponentHandle:(ComponentHandle)componentHandle
                                            tag:(Tag)tag
                        componentViewDescriptor:(RCTComponentViewDescriptor)componentViewDescriptor
{
  RCTAssertMainQueue();

  RCTAssert(
      _registry.find(tag) != _registry.end(), @"RCTComponentViewRegistry: Attempt to enqueue unregistered component.");

  _registry.erase(tag);
  componentViewDescriptor.view.tag = 0;
  [self _enqueueComponentViewWithComponentHandle:componentHandle componentViewDescriptor:componentViewDescriptor];
}

- (const RCTComponentViewDescriptor &)componentViewDescriptorWithTag:(Tag)tag
{
  RCTAssertMainQueue();
  auto iterator = _registry.find(tag);
  RCTAssert(iterator != _registry.end(), @"RCTComponentViewRegistry: Attempt to query unregistered component.");
  return iterator->second;
}

- (nullable UIView<RCTComponentViewProtocol> *)findComponentViewWithTag:(Tag)tag
{
  RCTAssertMainQueue();
  auto iterator = _registry.find(tag);
  if (iterator == _registry.end()) {
    return nil;
  }
  return iterator->second.view;
}

- (RCTComponentViewDescriptor)_dequeueComponentViewWithComponentHandle:(ComponentHandle)componentHandle
{
  RCTAssertMainQueue();
  auto &recycledViews = _recyclePool[componentHandle];

  if (recycledViews.empty()) {
    return [self.componentViewFactory createComponentViewWithComponentHandle:componentHandle];
  }

  auto componentViewDescriptor = recycledViews.back();
  recycledViews.pop_back();
  _poolTouched = YES;
  return componentViewDescriptor;
}

- (void)_enqueueComponentViewWithComponentHandle:(ComponentHandle)componentHandle
                         componentViewDescriptor:(RCTComponentViewDescriptor)componentViewDescriptor
{
  RCTAssertMainQueue();
  auto &recycledViews = _recyclePool[componentHandle];

  if (recycledViews.size() > RCTComponentViewRegistryRecyclePoolMaxSize || !componentViewDescriptor.shouldBeRecycled) {
    [componentViewDescriptor.view invalidate];
    return;
  }

  RCTAssert(
      componentViewDescriptor.view.superview == nil, @"RCTComponentViewRegistry: Attempt to recycle a mounted view.");
  [componentViewDescriptor.view prepareForRecycle];

  recycledViews.push_back(componentViewDescriptor);
  _poolTouched = YES;
  if (!_trimScheduled && recycledViews.size() > RCTComponentViewRegistryRecyclePoolIdleSize) {
    [self _scheduleRecyclePoolTrim];
  }
}

// One timer at a time, only while some pool is over the idle size. It fires every idle interval: if the
// pool was used in between it just re-arms, otherwise every pool is cut to the idle size and the timer stops.
- (void)_scheduleRecyclePoolTrim
{
  _trimScheduled = YES;
  __weak RCTComponentViewRegistry *weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, RCTComponentViewRegistryRecyclePoolIdleNs), dispatch_get_main_queue(), ^{
    RCTComponentViewRegistry *strongSelf = weakSelf;
    if (strongSelf == nil) {
      return;
    }
    strongSelf->_trimScheduled = NO;
    if (strongSelf->_poolTouched) {
      strongSelf->_poolTouched = NO;
      [strongSelf _scheduleRecyclePoolTrim];
      return;
    }
    for (auto &entry : strongSelf->_recyclePool) {
      auto &views = entry.second;
      if (views.size() > RCTComponentViewRegistryRecyclePoolIdleSize) {
        while (views.size() > RCTComponentViewRegistryRecyclePoolIdleSize) {
          views.pop_back();
        }
      }
    }
  });
}

- (void)handleApplicationDidReceiveMemoryWarningNotification
{
  _recyclePool.clear();
}

@end
