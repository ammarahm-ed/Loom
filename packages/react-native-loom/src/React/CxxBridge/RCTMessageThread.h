/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import <memory>
#import <string>

#import <Foundation/Foundation.h>

#import <React/RCTJavaScriptExecutor.h>
#import <cxxreact/MessageQueueThread.h>
#import <atomic>

namespace facebook::react {

class RCTMessageThread : public MessageQueueThread, public std::enable_shared_from_this<RCTMessageThread> {
 public:
  // React Native's own signature, kept exported (libraries link against it).
  RCTMessageThread(NSRunLoop *runLoop, RCTJavaScriptCompleteBlock errorBlock);
  RCTMessageThread(NSRunLoop *runLoop, RCTJavaScriptCompleteBlock errorBlock, bool stopsRunLoopOnShutdown);
  ~RCTMessageThread() override;
  void runOnQueue(std::function<void()> && /*func*/) override;
  void runOnQueueSync(std::function<void()> && /*func*/) override;
  void quitSynchronous() override;
  void setRunLoop(NSRunLoop *runLoop);

 private:
  void tryFunc(const std::function<void()> &func);
  void runAsync(std::function<void()> func);
  void runSync(std::function<void()> func);

  CFRunLoopRef m_cfRunLoop;
  RCTJavaScriptCompleteBlock m_errorBlock;
  std::atomic_bool m_shutdown;
  const bool m_stopsRunLoopOnShutdown;
};

} // namespace facebook::react
