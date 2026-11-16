import React
import React_RCTAppDelegate
import ReactAppDependencyProvider
import UIKit

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

    #if DEBUG
    factory.devMenuConfiguration = RCTDevMenuConfiguration(
      devMenuEnabled: true,
      shakeGestureEnabled: true,
      keyboardShortcutsEnabled: true
    )
    #endif

    window = UIWindow(frame: UIScreen.main.bounds)

    // `xcrun simctl launch <udid> <bundle id> -scenario <lib>|all` picks the compat screen.
    let args = ProcessInfo.processInfo.arguments
    var props: [String: Any]? = nil
    if let i = args.firstIndex(of: "-scenario"), i + 1 < args.count {
      props = ["scenario": args[i + 1]]
    }
    factory.startReactNative(
      withModuleName: "LoomCompatApp",
      in: window,
      initialProperties: props,
      launchOptions: launchOptions
    )

    return true
  }

  func application(
    _ app: UIApplication,
    open url: URL,
    options: [UIApplication.OpenURLOptionsKey: Any] = [:]
  ) -> Bool {
    RCTLinkingManager.application(app, open: url, options: options)
  }

  func application(
    _ application: UIApplication,
    continue userActivity: NSUserActivity,
    restorationHandler: @escaping ([UIUserActivityRestoring]?) -> Void
  ) -> Bool {
    RCTLinkingManager.application(
      application,
      continue: userActivity,
      restorationHandler: restorationHandler
    )
  }
}

class ReactNativeDelegate: RCTDefaultReactNativeFactoryDelegate {
  override func sourceURL(for bridge: RCTBridge) -> URL? {
    self.bundleURL()
  }

  override func bundleURL() -> URL? {
    #if DEBUG
    // On a device, React Native's build step writes the Mac's address to ip.txt. This app's Metro
    // runs on 8088 (8081 is often taken by another project's dev server).
    if let path = Bundle.main.path(forResource: "ip", ofType: "txt"),
       let ip = try? String(contentsOfFile: path, encoding: .utf8)
         .trimmingCharacters(in: .whitespacesAndNewlines),
       !ip.isEmpty {
      RCTBundleURLProvider.sharedSettings().jsLocation = "\(ip):8088"
    } else {
      // Simulator: the Mac is localhost.
      RCTBundleURLProvider.sharedSettings().jsLocation = "localhost:8088"
    }
    return RCTBundleURLProvider.sharedSettings().jsBundleURL(forBundleRoot: "index")
    #else
    return Bundle.main.url(forResource: "main", withExtension: "jsbundle")
    #endif
  }
}
