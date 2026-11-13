import UIKit
import React
import React_RCTAppDelegate
import ReactAppDependencyProvider

@main
class AppDelegate: UIResponder, UIApplicationDelegate {
  var window: UIWindow?

  var reactNativeDelegate: ReactNativeDelegate?
  var reactNativeFactory: RCTReactNativeFactory?

  func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    let delegate = ReactNativeDelegate()
    let factory = RCTReactNativeFactory(delegate: delegate)
    delegate.dependencyProvider = RCTAppDependencyProvider()

    reactNativeDelegate = delegate
    reactNativeFactory = factory

    window = UIWindow(frame: UIScreen.main.bounds)

    factory.startReactNative(
      withModuleName: "LoomTestApp",
      in: window,
      initialProperties: initialProperties(),
      launchOptions: launchOptions
    )

    return true
  }

  /// Bench pass-through: `xcrun simctl launch <udid> <bundle id> -scenario burst-raf`
  /// renders Bench.tsx (same as the Android `scenario` intent extra).
  private func initialProperties() -> [String: Any]? {
    let args = ProcessInfo.processInfo.arguments
    guard let i = args.firstIndex(of: "-scenario"), i + 1 < args.count else { return nil }
    // `-autoscroll`: Bench.tsx drives the scroll scenarios from JS (the benchmark host has no swipe).
    return ["scenario": args[i + 1], "autoscroll": args.contains("-autoscroll")]
  }
}

class ReactNativeDelegate: RCTDefaultReactNativeFactoryDelegate {
  override func bundleURL() -> URL? {
#if DEBUG
    RCTBundleURLProvider.sharedSettings().jsBundleURL(forBundleRoot: "index")
#else
    Bundle.main.url(forResource: "main", withExtension: "jsbundle")
#endif
  }
}
