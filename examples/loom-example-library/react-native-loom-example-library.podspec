require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "react-native-loom-example-library"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.license      = package["license"]
  s.authors      = "Loom"
  s.homepage     = "https://github.com/ammarahm-ed/react-native-loom"
  s.platforms    = { :ios => "15.1" }
  s.source       = { :path => "." }
  s.source_files = "cpp/*.{h,cpp}", "ios/*.{h,mm}"
  s.pod_target_xcconfig = { "CLANG_CXX_LANGUAGE_STANDARD" => "c++20", "HEADER_SEARCH_PATHS" => "\"$(PODS_TARGET_SRCROOT)/cpp\"" }
  # React Native's headers, whichever renderer: Fabric, or Loom (which ships <react/loom/Loom.h> with the same pods).
  install_modules_dependencies(s)
end
