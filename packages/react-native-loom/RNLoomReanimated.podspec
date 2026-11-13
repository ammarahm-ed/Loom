# Loom adapter for react-native-reanimated (see adapters/reanimated/README.md). Autolinked from
# react-native-loom when a supported Reanimated is installed; the RNReanimated
# pod then leaves out its own ReanimatedModuleProxy.cpp (integration/ios/loom_pods.rb).
require "json"

app_root = File.expand_path("..", Pod::Config.instance.installation_root.to_s)
reanimated_package = `node --print "require.resolve('react-native-reanimated/package.json', {paths: [process.argv[1]]})" "#{app_root}"`.strip
reanimated_root = File.dirname(reanimated_package)
reanimated_version = JSON.parse(File.read(reanimated_package))["version"]
require File.join(reanimated_root, "scripts", "reanimated_utils")
# Reanimated's helper reads its flags file relative to its own package directory.
feature_flags = "-DREANIMATED_FEATURE_FLAGS=\"#{Dir.chdir(reanimated_root) { ReanimatedUtils.get_static_feature_flags() }}\""
loom_package = JSON.parse(File.read(File.join(__dir__, "package.json")))
adapter = "adapters/reanimated"

Pod::Spec.new do |s|
  s.name         = "RNLoomReanimated"
  s.version      = loom_package["version"]
  s.summary      = "Loom adapter for react-native-reanimated"
  s.homepage     = "https://github.com/streetwriters/react-native-loom"
  s.license      = "MIT"
  s.author       = "Loom"
  s.platforms    = { :ios => "15.1" }
  s.source       = { :git => "https://github.com/streetwriters/react-native-loom.git" }
  s.source_files = "#{adapter}/cpp/*.{h,cpp}", "#{adapter}/patched/#{reanimated_version}/ReanimatedModuleProxy.cpp"
  s.dependency "RNReanimated"
  s.dependency "RNWorklets"
  s.compiler_flags = "-Wno-documentation"
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "HEADER_SEARCH_PATHS" => [
      "\"$(PODS_TARGET_SRCROOT)/#{adapter}/cpp\"",
      "\"#{reanimated_root}/Common/cpp\"",
      "\"#{reanimated_root}/apple\"",
      "\"#{reanimated_root}/Common/NativeView\"",
      "\"$(PODS_ROOT)/Headers/Public/RNWorklets\"",
      "\"$(PODS_ROOT)/Headers/Public/RNReanimated\"",
      "\"$(PODS_ROOT)/RCT-Folly\"",
      "\"$(PODS_ROOT)/boost\"",
      "\"$(PODS_ROOT)/DoubleConversion\"",
      "\"$(PODS_ROOT)/Headers/Public/React-hermes\"",
      "\"$(PODS_ROOT)/Headers/Public/hermes-engine\"",
    ].join(" "),
    "OTHER_CFLAGS" => "$(inherited) -DREANIMATED_VERSION=#{reanimated_version} #{feature_flags}",
  }
  install_modules_dependencies(s)
  s.dependency "React-jsi"
  s.dependency "React-hermes"
end
