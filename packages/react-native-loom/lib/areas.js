'use strict';

/**
 * Area tags group overrides by *why* they exist. They drive `loom status`
 * and the phase-2 plan: every area other than `renderer-core` is a seam we
 * want to shrink (move out of RN, or upstream).
 */
const RULES = [
  [/ReactCommon\/react\/renderer\/uimanager\/(UIThread|tests\/UIThread|tests\/TestUIManagerDelegate)/, 'renderer-core'],
  [/LoomViewApplier/, 'mounting'],
  [/(UIManagerDelegate\.h|SchedulerDelegate\.h|renderer\/scheduler\/|RCTScheduler\.|FabricUIManagerBinding|UIManagerBinding\.cpp|SurfaceHandlerBinding)/, 'delegate-plumbing'],
  [/(MountingManager|RCTMountingManager|RCTSurfacePresenter|RCTFabricSurface|FabricUIManager\.java|SurfaceMountingManager|ViewManager\.java|MainReactPackage)/, 'mounting'],
  [/(RuntimeScheduler|RCTMessageThread|RCTJSThreadManager|ReactInstance\.kt|JReactInstance|ReactFontManager|ComponentDescriptorRegistry)/, 'threading'],
  [/(textlayoutmanager|TextLayoutManager|PreparedLayout|ReactTextView|TextEffectRegistry|ParagraphEventEmitter|views\/text)/i, 'text'],
  [/(NativeDOM|domnativemodule|IntersectionObserver|DefaultTurboModules)/, 'dom'],
  [/(\.podspec$|\.cmake$|CMakeLists\.txt$|build\.gradle\.kts$|react_native_pods\.rb$|scripts\/|ReactBuildConfig|FeatureFlagsOverrides)/, 'build'],
  [/(LayoutAnimation)/, 'animation'],
  [/(ReactScrollView|ReactViewManager|ReactImageManager|JSTouchDispatcher|views\/)/, 'views'],
];

function classify(file) {
  for (const [re, area] of RULES) {
    if (re.test(file)) return area;
  }
  return 'misc';
}

module.exports = {classify};
