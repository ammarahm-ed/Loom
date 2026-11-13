// Native timing hooks for the iOS benchmark scenarios (see Bench.tsx and bench/run-ios.js), the
// counterpart of BenchProbe.kt, plus the iOS LoomBuildConfig module the smoke screen reads.
//
// Everything is measured on the main thread with a CADisplayLink, so the numbers are taken where
// frames are produced and stay comparable between stock React Native (JS on its own thread) and
// Loom with JS on the main thread:
//   - `startup` cold-start milestones in milliseconds since the process was created (kinfo_proc
//               start time, react-native-performance's `nativeLaunchStart`), following
//               https://github.com/callstackincubator/agent-skills (native-measure-tti.md):
//                 nativeLaunchEnd   this class's +load (pre-main, dyld done with the app image)
//                 appCreationEnd    UIApplicationDidFinishLaunchingNotification
//                 runJSBundleEnd    RCTInstanceDidLoadBundle (bundle evaluated, on the JS thread)
//                 contentAppeared   RCTContentDidAppearNotification (root got its first child)
//                 screenInteractive JS `useEffect` of the first screen (LoomBuildConfig.mark, sync)
//                 displayed         the Core Animation commit that first contains the view with
//                                   nativeID `bench-ready` (run-loop observer ordered after CA's
//                                   commit, so not quantized to display-link frames)
//               TTI is `screenInteractive`. Prewarmed or background launches are flagged.
//   - `frames`  frame-interval statistics over a window, from the display link: jank % (an
//               interval above 1.5x the refresh interval), p50/p90/p95/p99 and the number of
//               frames of 32 ms or more.
// Lines are printed to stderr with the prefix `LoomBench ` for the host script to parse. JS-side
// `[bench] result {...}` lines are forwarded here by LoomBuildConfig.log so they are visible in a
// Release build, where console.log does not reach the device console.
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <React/RCTBridgeModule.h>
#import <objc/runtime.h>

#include <algorithm>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <vector>

#ifndef LOOM_BENCH_RENDERER
#define LOOM_BENCH_RENDERER 0
#endif
#if LOOM_BENCH_RENDERER
// Loom's asynchronous text display counters (RCTParagraphComponentView.mm).
extern "C" void LoomTextDisplayCountersRead(uint64_t out[9]);
extern "C" void LoomTextDisplaySwitchesRead(bool out[2]);
#endif
#ifndef LOOM_BENCH_WORKER
#define LOOM_BENCH_WORKER 0
#endif
#ifndef LOOM_BENCH_JS_ON_MAIN
#define LOOM_BENCH_JS_ON_MAIN 0
#endif
#ifndef LOOM_BENCH_STOCK
#define LOOM_BENCH_STOCK 0
#endif

// CPU time per thread group over a measurement window (thread_info user + system time). Threads are
// grouped by role: the main thread, React Native's JS thread, Loom's worker; `total` is every live
// thread of the process. A thread that exits during the window loses its share, which only
// matters for short-lived helpers.
struct LoomCpuSnapshot {
  double mainMs{0};
  double jsMs{0};
  double workerMs{0};
  double totalMs{0};
};

// The main thread's Mach port, recorded when the class loads (+load runs on the main thread;
// iOS has no pthread_main_thread_np).
static mach_port_t gMainThreadPort = MACH_PORT_NULL;

static LoomCpuSnapshot TakeCpuSnapshot()
{
  LoomCpuSnapshot snapshot;
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
    return snapshot;
  }
  const mach_port_t mainPort = gMainThreadPort;
  for (mach_msg_type_number_t index = 0; index < count; index++) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t infoCount = THREAD_BASIC_INFO_COUNT;
    if (thread_info(threads[index], THREAD_BASIC_INFO, (thread_info_t)&info, &infoCount) ==
        KERN_SUCCESS) {
      const double ms = (info.user_time.seconds + info.system_time.seconds) * 1000.0 +
          (info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
      snapshot.totalMs += ms;
      if (threads[index] == mainPort) {
        snapshot.mainMs += ms;
      } else {
        char name[96] = {0};
        pthread_t thread = pthread_from_mach_thread_np(threads[index]);
        if (thread != nullptr) {
          pthread_getname_np(thread, name, sizeof(name));
        }
        if (strstr(name, "JavaScript") != nullptr) {
          snapshot.jsMs += ms;
        } else if (strstr(name, "loom.worker") != nullptr) {
          snapshot.workerMs += ms;
        }
      }
    }
    mach_port_deallocate(mach_task_self(), threads[index]);
  }
  vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_act_t));
  return snapshot;
}

static void BenchLine(NSString *line)
{
  fprintf(stderr, "LoomBench %s\n", line.UTF8String);
  fflush(stderr);
}

static double NowEpochMs(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static double ProcessStartEpochMs(void)
{
  struct kinfo_proc info;
  size_t size = sizeof(info);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) {
    return 0;
  }
  struct timeval start = info.kp_proc.p_un.__p_starttime;
  return start.tv_sec * 1000.0 + start.tv_usec / 1000.0;
}

// Startup milestones, epoch ms (0: not reached). Written on the main thread except
// runJSBundleEnd/screenInteractive (JS thread); read on the main thread once `displayed` and
// `screenInteractive` are both set.
static UIView *FindViewWithNativeID(UIView *root, NSString *nativeID);
static double gNativeLaunchEnd, gAppCreationEnd, gRunJSBundleEnd, gContentAppeared, gScreenInteractive, gDisplayed;
static BOOL gPrewarm, gForeground;
// Start of the Core Animation commit that produced `displayed` (an observer ordered just before CA's).
static double gPreCommit;
// End of the main-thread mount that put the ready view in the window: RCTSurfacePresenter's
// mountingManager:didMountComponentsWithRootTag:, which stock Fabric and Loom both call after a
// mount transaction. Hooked by replacing the method's implementation (bench builds only).
static double gMounted;
static IMP gOriginalDidMount;
static IMP gOriginalWillMount;
static double gLastWillMount, gMountStart;
static void BenchWillMount(id self, SEL _cmd, id mountingManager, NSInteger rootTag)
{
  gLastWillMount = NowEpochMs();
  ((void (*)(id, SEL, id, NSInteger))gOriginalWillMount)(self, _cmd, mountingManager, rootTag);
}
static void BenchDidMount(id self, SEL _cmd, id mountingManager, NSInteger rootTag)
{
  ((void (*)(id, SEL, id, NSInteger))gOriginalDidMount)(self, _cmd, mountingManager, rootTag);
  if (gMounted == 0) {
    for (UIWindow *window in UIApplication.sharedApplication.windows) {
      if (FindViewWithNativeID(window, @"bench-ready") != nil) {
        gMounted = NowEpochMs();
        gMountStart = gLastWillMount;
        break;
      }
    }
  }
}

static UIView *FindViewWithNativeID(UIView *root, NSString *nativeID)
{
  for (SEL selector : {NSSelectorFromString(@"nativeId"), NSSelectorFromString(@"nativeID")}) {
    if ([root respondsToSelector:selector]) {
      id value = ((id (*)(id, SEL))[root methodForSelector:selector])(root, selector);
      if ([value isKindOfClass:NSString.class] && [value isEqualToString:nativeID]) {
        return root;
      }
    }
  }
  if ([root.accessibilityIdentifier isEqualToString:nativeID]) {
    return root;
  }
  for (UIView *child in root.subviews) {
    if (UIView *found = FindViewWithNativeID(child, nativeID)) {
      return found;
    }
  }
  return nil;
}

@interface LoomBenchProbe : NSObject
+ (instancetype)shared;
- (void)endWindow;
- (void)maybeReportStartup;
@end

@implementation LoomBenchProbe {
  CADisplayLink *_link;
  BOOL _startupReported;
  double _readyMediaTime;
  CFRunLoopObserverRef _readyObserver;
  CFRunLoopObserverRef _preCommitObserver;
  double _lastTick;
  BOOL _windowActive;
  LoomCpuSnapshot _windowCpu;
  double _windowOpenedAt;
  double _windowStart;      // media time the window opens at (0: not scheduled)
  double _windowLengthSec;  // 0: until JS reports its result
  std::vector<double> _intervals;
  NSString *_scenario;
}

+ (instancetype)shared
{
  static LoomBenchProbe *probe;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ probe = [LoomBenchProbe new]; });
  return probe;
}

+ (void)load
{
  gMainThreadPort = pthread_mach_thread_np(pthread_self());
  gNativeLaunchEnd = NowEpochMs();
  if (Class presenter = NSClassFromString(@"RCTSurfacePresenter")) {
    Method method = class_getInstanceMethod(presenter, NSSelectorFromString(@"mountingManager:didMountComponentsWithRootTag:"));
    if (method != nullptr) {
      gOriginalDidMount = method_setImplementation(method, (IMP)BenchDidMount);
    }
    Method will = class_getInstanceMethod(presenter, NSSelectorFromString(@"mountingManager:willMountComponentsWithRootTag:"));
    if (will != nullptr) {
      gOriginalWillMount = method_setImplementation(will, (IMP)BenchWillMount);
    }
  }
  gPrewarm = [NSProcessInfo.processInfo.environment[@"ActivePrewarm"] isEqualToString:@"1"];
  NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
  [center addObserverForName:@"RCTInstanceDidLoadBundle" object:nil queue:nil usingBlock:^(NSNotification *note) {
    if (gRunJSBundleEnd == 0) { gRunJSBundleEnd = NowEpochMs(); }
  }];
  [center addObserverForName:@"RCTContentDidAppearNotification" object:nil queue:nil usingBlock:^(NSNotification *note) {
    if (gContentAppeared == 0) { gContentAppeared = NowEpochMs(); }
  }];
  [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidFinishLaunchingNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *note) {
                                                  gAppCreationEnd = NowEpochMs();
                                                  gForeground = UIApplication.sharedApplication.applicationState != UIApplicationStateBackground;
                                                  [[LoomBenchProbe shared] start];
                                                }];
}

- (void)start
{
  NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
  NSUInteger i = [args indexOfObject:@"-scenario"];
  _scenario = (i != NSNotFound && i + 1 < args.count) ? args[i + 1] : nil;
  // `-snapshot <seconds>`: write the key window to Documents/snapshot.png after that delay (iOS
  // has no screenshot command for devicectl); fetched with `devicectl device copy from`.
  NSUInteger snap = [args indexOfObject:@"-snapshot"];
  if (snap != NSNotFound && snap + 1 < args.count) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)([args[snap + 1] doubleValue] * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
      UIWindow *window = UIApplication.sharedApplication.windows.firstObject;
      UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithBounds:window.bounds];
      UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        [window drawViewHierarchyInRect:window.bounds afterScreenUpdates:YES];
      }];
      NSString *dir = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
      [UIImagePNGRepresentation(image) writeToFile:[dir stringByAppendingPathComponent:@"snapshot.png"] atomically:YES];
      BenchLine(@"snapshot written");
    });
  }
  if (_scenario == nil) {
    return;
  }
  // `displayed`: checked after every Core Animation commit (CA commits in its BeforeWaiting/Exit
  // observer at order 2000000) until the ready view is in the committed tree.
  __weak LoomBenchProbe *weakSelf = self;
  _readyObserver = CFRunLoopObserverCreateWithHandler(
      kCFAllocatorDefault, kCFRunLoopBeforeWaiting | kCFRunLoopExit, true, 2000001,
      ^(CFRunLoopObserverRef observer, CFRunLoopActivity activity) {
        [weakSelf checkReady];
      });
  CFRunLoopAddObserver(CFRunLoopGetMain(), _readyObserver, kCFRunLoopCommonModes);
  _preCommitObserver = CFRunLoopObserverCreateWithHandler(
      kCFAllocatorDefault, kCFRunLoopBeforeWaiting | kCFRunLoopExit, true, 1999999,
      ^(CFRunLoopObserverRef observer, CFRunLoopActivity activity) {
        gPreCommit = NowEpochMs();
      });
  CFRunLoopAddObserver(CFRunLoopGetMain(), _preCommitObserver, kCFRunLoopCommonModes);
  _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)scheduleWindowAfterReady
{
  NSString *base = [_scenario hasSuffix:@"-busy"] ? [_scenario substringToIndex:_scenario.length - 5] : _scenario;
  double length = -1;
  if ([base isEqualToString:@"anim"] || [base isEqualToString:@"js-anim"]) {
    length = 6.0;
  } else if ([base isEqualToString:@"scroll"]) {
    length = 7.5;
  } else if ([base isEqualToString:@"transition-heavy"] || [base isEqualToString:@"heavy-sync"] ||
             [base hasPrefix:@"burst"]) {
    length = 0; // ends when JS reports its result
  }
  if (length >= 0) {
    _windowStart = _readyMediaTime + 1.5;
    _windowLengthSec = length;
  }
}

- (void)checkReady
{
  if (_readyObserver == nullptr) {
    return;
  }
  for (UIWindow *window in UIApplication.sharedApplication.windows) {
    if (FindViewWithNativeID(window, @"bench-ready") != nil) {
      gDisplayed = NowEpochMs();
      _readyMediaTime = CACurrentMediaTime();
      CFRunLoopObserverInvalidate(_readyObserver);
      CFRunLoopObserverInvalidate(_preCommitObserver);
      CFRelease(_preCommitObserver);
      _preCommitObserver = nullptr;
      CFRelease(_readyObserver);
      _readyObserver = nullptr;
      [self scheduleWindowAfterReady];
      [self maybeReportStartup];
      return;
    }
  }
}

// Called on the main thread when `displayed` is reached and when JS marks `screenInteractive`.
- (void)maybeReportStartup
{
  if (_startupReported || gDisplayed == 0 || gScreenInteractive == 0) {
    return;
  }
  _startupReported = YES;
  const double t0 = ProcessStartEpochMs();
  auto rel = [&](double t) { return t > 0 ? t - t0 : -1.0; };
  BenchLine([NSString stringWithFormat:
      @"startup nativeLaunchEnd=%.1f appCreationEnd=%.1f runJSBundleEnd=%.1f contentAppeared=%.1f "
       "screenInteractive=%.1f mountStart=%.1f mounted=%.1f commitStart=%.1f displayed=%.1f prewarm=%d foreground=%d t0=%.2f",
      rel(gNativeLaunchEnd), rel(gAppCreationEnd), rel(gRunJSBundleEnd), rel(gContentAppeared),
      rel(gScreenInteractive), rel(gMountStart), rel(gMounted), rel(gPreCommit), rel(gDisplayed), gPrewarm, gForeground, t0]);
}

- (void)tick:(CADisplayLink *)link
{
  const double now = CACurrentMediaTime();
  if (_windowStart > 0 && !_windowActive && now >= _windowStart) {
    _windowActive = YES;
    _intervals.clear();
    _windowCpu = TakeCpuSnapshot();
    _windowOpenedAt = now;
    BenchLine(@"window start");
  } else if (_windowActive) {
    _intervals.push_back((now - _lastTick) * 1000.0);
    if (_windowLengthSec > 0 && now >= _windowStart + _windowLengthSec) {
      [self endWindow];
    }
  }
  _lastTick = now;
}

- (void)endWindow
{
  if (!_windowActive) {
    return;
  }
  _windowActive = NO;
  _windowStart = 0;
  // Printed before the frames line, so a host that has the frames line has this one too.
  const LoomCpuSnapshot end = TakeCpuSnapshot();
  BenchLine([NSString stringWithFormat:@"cpu mainMs=%.0f jsMs=%.0f workerMs=%.0f totalMs=%.0f windowMs=%.0f",
                                       end.mainMs - _windowCpu.mainMs,
                                       end.jsMs - _windowCpu.jsMs,
                                       end.workerMs - _windowCpu.workerMs,
                                       end.totalMs - _windowCpu.totalMs,
                                       (CACurrentMediaTime() - _windowOpenedAt) * 1000.0]);
#if LOOM_BENCH_RENDERER
  {
    uint64_t c[9] = {0};
    LoomTextDisplayCountersRead(c);
    bool sw[2] = {false, false};
    LoomTextDisplaySwitchesRead(sw);
    BenchLine([NSString stringWithFormat:@"text async=%d firstDraw=%d asyncDraw=%llu asyncSkipped=%llu syncFirst=%llu "
                                          "syncResize=%llu syncUnsafe=%llu syncDisabled=%llu "
                                          "wideColor=%llu latencyAvgMs=%.1f latencyMaxMs=%.1f",
                                         sw[0], sw[1], c[0], c[1], c[2], c[3], c[4], c[5], c[6],
                                         c[0] ? c[7] / 1000.0 / c[0] : 0.0, c[8] / 1000.0]);
  }
#endif
  std::vector<double> sorted = _intervals;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  if (n == 0) {
    BenchLine(@"frames n=0");
    return;
  }
  const double refresh = 1000.0 / MAX(1, UIScreen.mainScreen.maximumFramesPerSecond);
  auto q = [&](double p) { return sorted[(size_t)MIN((double)(n - 1), p * (n - 1) + 0.5)]; };
  size_t janky = 0, over32 = 0;
  for (double v : sorted) {
    if (v > refresh * 1.5) { janky++; }
    if (v >= 32.0) { over32++; }
  }
  // Frames the display could not deliver because the main thread was blocked show up as one long
  // interval; count them as the extra vsyncs they cost, so jank% is relative to the frames due.
  double totalMs = 0;
  for (double v : sorted) { totalMs += v; }
  BenchLine([NSString stringWithFormat:
      @"frames n=%zu due=%.0f jankPct=%.2f p50=%.1f p90=%.1f p95=%.1f p99=%.1f max=%.1f over32=%zu refreshMs=%.2f",
      n, totalMs / refresh, 100.0 * janky / n, q(0.50), q(0.90), q(0.95), q(0.99), sorted.back(), over32, refresh]);
}

@end

// The iOS counterpart of LoomBuildConfigModule.kt. The values are the defines the app target was
// built with (xcodebuild GCC_PREPROCESSOR_DEFINITIONS), set by run-ios.js from the same table that
// feeds `pod install`, so the smoke screen shows the configuration that is running.
@interface LoomBuildConfig : NSObject <RCTBridgeModule>
@end

@implementation LoomBuildConfig

RCT_EXPORT_MODULE()

+ (BOOL)requiresMainQueueSetup
{
  return NO;
}

RCT_EXPORT_METHOD(getSwitches
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject)
{
  resolve(@{
    @"rnUiThread" : @(LOOM_BENCH_JS_ON_MAIN),
    @"loom" : @(LOOM_BENCH_RENDERER),
    @"loomWorker" : @(LOOM_BENCH_WORKER),
    @"profile" : @NO,
    @"stock" : @(LOOM_BENCH_STOCK),
  });
}

// Runs synchronously on the calling thread, so what it reports is the thread JavaScript runs on.
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(jsThread)
{
  char name[64] = {0};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  NSDictionary *info = @{
    @"isMainThread" : @(pthread_main_np() != 0),
    @"threadName" : [NSString stringWithUTF8String:name],
    @"queue" : [NSString stringWithUTF8String:dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL) ?: ""],
  };
  BenchLine([NSString stringWithFormat:@"jsThread main=%d name=%@ queue=%@",
             pthread_main_np() != 0, info[@"threadName"], info[@"queue"]]);
  return info;
}

// `screenInteractive` (and any other JS milestone): synchronous, so the time is taken on the JS
// thread when the effect runs, not when a queued call reaches native code.
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(mark : (NSString *)name)
{
  const double now = NowEpochMs();
  if ([name isEqualToString:@"screenInteractive"] && gScreenInteractive == 0) {
    gScreenInteractive = now;
    dispatch_async(dispatch_get_main_queue(), ^{ [[LoomBenchProbe shared] maybeReportStartup]; });
  }
  return nil;
}

// Live malloc blocks by exact size (up to 8 KB; larger ones by power of two), via the zones'
// introspection enumerators. The callback must not allocate, so the table is static.
static uint32_t gHist[8192 + 1 + 24];
static kern_return_t HistReader(task_t, vm_address_t address, vm_size_t, void **ptr)
{
  *ptr = (void *)address;
  return KERN_SUCCESS;
}
static void HistRanges(task_t, void *, unsigned, vm_range_t *ranges, unsigned count)
{
  for (unsigned i = 0; i < count; i++) {
    size_t size = ranges[i].size;
    if (size <= 8192) {
      gHist[size]++;
    } else {
      unsigned bucket = 0;
      while ((size >> bucket) > 1) bucket++;
      gHist[8192 + (bucket < 24 ? bucket : 23)]++;
    }
  }
}

// Memory for the `memory` scenario: phys_footprint (what jetsam counts) and the bytes malloc has
// handed out and not had freed (size_in_use over all zones), both in KB.
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(memSample)
{
  task_vm_info_data_t vm;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  double footprint = -1;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) == KERN_SUCCESS) {
    footprint = vm.phys_footprint / 1024.0;
  }
  malloc_statistics_t stats;
  malloc_zone_statistics(NULL, &stats);
  NSMutableDictionary *result = [@{@"footprintKb" : @(footprint), @"heapKb" : @(stats.size_in_use / 1024.0)} mutableCopy];
  return result;
}

RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(memHist)
{
  memset(gHist, 0, sizeof(gHist));
  vm_address_t *zones = nullptr;
  unsigned zoneCount = 0;
  malloc_get_all_zones(mach_task_self(), HistReader, &zones, &zoneCount);
  for (unsigned i = 0; i < zoneCount; i++) {
    auto *zone = (malloc_zone_t *)zones[i];
    if (zone->introspect && zone->introspect->enumerator) {
      zone->introspect->enumerator(mach_task_self(), nullptr, MALLOC_PTR_IN_USE_RANGE_TYPE, (vm_address_t)zone, HistReader, HistRanges);
    }
  }
  NSMutableString *out = [NSMutableString new];
  for (size_t size = 0; size <= 8192; size++) {
    if (gHist[size] >= 800) [out appendFormat:@"%zu:%u ", size, gHist[size]];
  }
  return out;
}

RCT_EXPORT_METHOD(log : (NSString *)line)
{
  BenchLine([@"js " stringByAppendingString:line]);
  if ([line hasPrefix:@"[bench] result"]) {
    dispatch_async(dispatch_get_main_queue(), ^{ [[LoomBenchProbe shared] endWindow]; });
  }
}

@end
