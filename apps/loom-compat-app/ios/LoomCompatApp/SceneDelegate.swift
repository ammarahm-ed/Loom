import React
import React_RCTAppDelegate
import ReactAppDependencyProvider
import UIKit

class SceneDelegate: RCTDefaultReactNativeFactoryDelegate, UIWindowSceneDelegate {
  var window: UIWindow?
  var reactNativeFactory: RCTReactNativeFactory?

  func scene(
    _ scene: UIScene,
    willConnectTo session: UISceneSession,
    options connectionOptions: UIScene.ConnectionOptions
  ) {
    guard let windowScene = scene as? UIWindowScene else {
      return
    }

    dependencyProvider = RCTAppDependencyProvider()
    reactNativeFactory = RCTReactNativeFactory(delegate: self)
    window = UIWindow(windowScene: windowScene)
    
    #if DEBUG
    let devMenuConfiguration = RCTDevMenuConfiguration(
      devMenuEnabled: true,
      shakeGestureEnabled: true,
      keyboardShortcutsEnabled: true
    )
    reactNativeFactory?.devMenuConfiguration = devMenuConfiguration
    #endif
    
    // `xcrun simctl launch <udid> <bundle id> -scenario <lib>|all` picks the compat screen.
    let args = ProcessInfo.processInfo.arguments
    var props: [String: Any]? = nil
    if let i = args.firstIndex(of: "-scenario"), i + 1 < args.count {
      props = ["scenario": args[i + 1]]
    }
    reactNativeFactory?.startReactNative(
      withModuleName: "LoomCompatApp",
      in: window,
      initialProperties: props,
      connectionOptions: connectionOptions
    )

   
  }

  func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
    RCTLinkingManager.scene(scene, openURLContexts: URLContexts)
  }

  func scene(_ scene: UIScene, continue userActivity: NSUserActivity) {
    RCTLinkingManager.scene(scene, continue: userActivity)
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
