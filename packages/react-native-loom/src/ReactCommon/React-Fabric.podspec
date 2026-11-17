# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require "json"

package = JSON.parse(File.read(File.join(__dir__, "..", "package.json")))
version = package['version']

source = { :git => 'https://github.com/facebook/react-native.git' }
if version == '1000.0.0'
  # This is an unpublished version, use the latest commit hash of the react-native repo, which we’re presumably in.
  source[:commit] = `git rev-parse HEAD`.strip if system("git rev-parse --git-dir > /dev/null 2>&1")
else
  source[:tag] = "v#{version}"
end

react_native_path = ".."

Pod::Spec.new do |s|
  s.name                   = "React-Fabric"
  s.version                = version
  s.summary                = "Fabric for React Native."
  s.homepage               = "https://reactnative.dev/"
  s.license                = package["license"]
  s.author                 = "Meta Platforms, Inc. and its affiliates"
  s.platforms              = min_supported_versions
  s.source                 = source
  s.source_files           = podspec_sources("dummyFile.cpp", "")
  direct_renderer_enabled = ENV["RCT_LOOM_RENDERER_IOS"] == "1"
  # Loom's worker thread is part of the renderer: on exactly when the renderer is.
  direct_renderer_worker_enabled = direct_renderer_enabled
  fabric_definitions = ["$(inherited)"]
  fabric_definitions << "LOOM_RENDERER=1" if direct_renderer_enabled
  fabric_definitions << "LOOM_WORKER=1" if direct_renderer_worker_enabled
  s.pod_target_xcconfig = { "USE_HEADERMAP" => "YES",
                            "CLANG_CXX_LANGUAGE_STANDARD" => rct_cxx_language_standard(),
                            "DEFINES_MODULE" => "YES",
                            "GCC_PREPROCESSOR_DEFINITIONS" => fabric_definitions.join(" ") }

  resolve_use_frameworks(s, header_mappings_dir: "./", module_name: "React_Fabric")

  s.dependency "React-jsiexecutor"
  s.dependency "RCTRequired"
  s.dependency "RCTTypeSafety"
  s.dependency "ReactCommon/turbomodule/core"
  s.dependency "React-jsi"
  s.dependency "React-logger"
  s.dependency "React-Core"
  s.dependency "React-debug"
  s.dependency "React-cxxstableapi"
  s.dependency "React-featureflags"
  s.dependency "React-runtimescheduler"
  s.dependency "React-cxxreact"
  s.dependency "React-bridging"

  add_dependency(s, "React-runtimeexecutor", :additional_framework_paths => ["platform/ios"])
  add_dependency(s, "React-rendererdebug")
  add_dependency(s, "React-graphics", :additional_framework_paths => ["react/renderer/graphics/platform/ios"])
  add_dependency(s, "React-utils", :additional_framework_paths => ["react/utils/platform/ios"])

  depend_on_js_engine(s)
  add_rn_third_party_dependencies(s)
  add_rncore_dependency(s)

  s.subspec "animated" do |ss|
    ss.dependency             "React-Fabric/animationbackend"
    ss.source_files         = podspec_sources("react/renderer/animated/**/*.{m,mm,cpp,h}", "react/renderer/animated/**/*.{h}")
    ss.exclude_files        = "react/renderer/animated/tests"
    ss.header_dir           = "react/renderer/animated"
    ss.header_mappings_dir  = "react/renderer/animated"
  end

  s.subspec "animations" do |ss|
    ss.source_files         = podspec_sources("react/renderer/animations/**/*.{m,mm,cpp,h}", "react/renderer/animations/**/*.{h}")
    ss.exclude_files        = "react/renderer/animations/tests"
    ss.header_dir           = "react/renderer/animations"
  end

  s.subspec "animationbackend" do |ss|
    ss.source_files         = podspec_sources("react/renderer/animationbackend/**/*.{m,mm,cpp,h}", "react/renderer/animationbackend/**/*.{h}")
    ss.header_dir           = "react/renderer/animationbackend"
  end

  s.subspec "attributedstring" do |ss|
    ss.source_files         = podspec_sources("react/renderer/attributedstring/**/*.{m,mm,cpp,h}", "react/renderer/attributedstring/**/*.{h}")
    ss.exclude_files        = "react/renderer/attributedstring/tests"
    ss.header_dir           = "react/renderer/attributedstring"
  end

  s.subspec "bridging" do |ss|
    ss.source_files         = podspec_sources("react/renderer/bridging/**/*.{m,mm,cpp,h}", "react/renderer/bridging/**/*.{h}")
    ss.exclude_files        = "react/renderer/bridging/tests"
    ss.header_dir           = "react/renderer/bridging"
  end

  s.subspec "core" do |ss|
    header_search_path = [
      "\"$(PODS_TARGET_SRCROOT)/ReactCommon\"",
      "\"$(PODS_ROOT)/Headers/Private/Yoga\"",
      "\"$(PODS_TARGET_SRCROOT)\""
    ]

    if ENV['USE_FRAMEWORKS']
      header_search_path = header_search_path + [
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/textlayoutmanager/platform/ios\"",
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/components/scrollview/platform/cxx\"",
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/components/scrollview/platform/ios\"",
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/components/legacyviewmanagerinterop/platform/ios\"",
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/components/text/platform/cxx\"",
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/components/textinput/platform/ios\"",
        "\"$(PODS_TARGET_SRCROOT)/react/renderer/components/view/platform/cxx\"",
      ]
    end

    ss.source_files         = podspec_sources("react/renderer/core/**/*.{m,mm,cpp,h}", "react/renderer/core/**/*.{h}")
    ss.exclude_files        = "react/renderer/core/tests"
    ss.header_dir           = "react/renderer/core"
    ss.pod_target_xcconfig  = {
      "HEADER_SEARCH_PATHS" => header_search_path.join(" ")
    }
  end

  s.subspec "componentregistry" do |ss|
    ss.source_files         = podspec_sources("react/renderer/componentregistry/*.{m,mm,cpp,h}", "react/renderer/componentregistry/*.{h}")
    ss.header_dir           = "react/renderer/componentregistry"
  end

  s.subspec "componentregistrynative" do |ss|
    ss.source_files         = podspec_sources("react/renderer/componentregistry/native/**/*.{m,mm,cpp,h}", "react/renderer/componentregistry/native/**/*.{h}")
    ss.header_dir           = "react/renderer/componentregistry/native"
  end

  s.subspec "components" do |ss|
    ss.subspec "root" do |sss|
      sss.source_files         = podspec_sources("react/renderer/components/root/**/*.{m,mm,cpp,h}", "react/renderer/components/root/**/*.{h}")
      sss.exclude_files        = "react/renderer/components/root/tests"
      sss.header_dir           = "react/renderer/components/root"
    end

    ss.subspec "view" do |sss|
      sss.dependency             "React-renderercss"
      sss.dependency             "Yoga"
      sss.source_files         = podspec_sources(["react/renderer/components/view/*.{m,mm,cpp,h}", "react/renderer/components/view/platform/cxx/**/*.{m,mm,cpp,h}"], ["react/renderer/components/view/*.{h}", "react/renderer/components/view/platform/cxx/**/*.{h}"])
      sss.header_dir           = "react/renderer/components/view"
    end

    ss.subspec "viewUmbrella" do |sss|
      sss.source_files         = "react/renderer/components/view/React/*.h"
      sss.header_dir           = ""
      sss.header_mappings_dir  = "react/renderer/components/view"
    end

    ss.subspec "scrollview" do |sss|
      sss.source_files         = podspec_sources("react/renderer/components/scrollview/**/*.{m,mm,cpp,h}", "react/renderer/components/scrollview/**/*.{h}")
      sss.header_dir           = "react/renderer/components/scrollview"
      sss.exclude_files        = "react/renderer/components/scrollview/tests", "react/renderer/components/scrollview/platform/android"
    end

    ss.subspec "legacyviewmanagerinterop" do |sss|
      sss.source_files         = podspec_sources("react/renderer/components/legacyviewmanagerinterop/**/*.{m,mm,cpp,h}", "react/renderer/components/legacyviewmanagerinterop/**/*.{h}")
      sss.exclude_files        = "react/renderer/components/legacyviewmanagerinterop/tests"
      sss.header_dir           = "react/renderer/components/legacyviewmanagerinterop"
    end
  end

  s.subspec "dom" do |ss|
    ss.dependency             "React-graphics"
    ss.source_files         = podspec_sources("react/renderer/dom/**/*.{m,mm,cpp,h}", "react/renderer/dom/**/*.{h}")
    ss.exclude_files        = "react/renderer/dom/tests"
    ss.header_dir           = "react/renderer/dom"
  end

  s.subspec "scheduler" do |ss|
    ss.source_files         = podspec_sources("react/renderer/scheduler/**/*.{m,mm,cpp,h}", "react/renderer/scheduler/**/*.h")
    ss.exclude_files        = "react/renderer/scheduler/tests"
    ss.header_dir           = "react/renderer/scheduler"

    ss.dependency             "React-Fabric/animationbackend"
    ss.dependency             "React-Fabric/viewtransition"
    ss.dependency             "React-performancecdpmetrics"
    ss.dependency             "React-performancetimeline"
    ss.dependency             "React-Fabric/observers/events"
  end

  s.subspec "imagemanager" do |ss|
    ss.source_files         = podspec_sources("react/renderer/imagemanager/*.{m,mm,cpp,h}", "react/renderer/imagemanager/*.h")
    ss.header_dir           = "react/renderer/imagemanager"
  end

  s.subspec "mounting" do |ss|
    ss.dependency             "React-jsinspectortracing"
    ss.source_files         = podspec_sources("react/renderer/mounting/**/*.{m,mm,cpp,h}", "react/renderer/mounting/**/*.h")
    ss.exclude_files        = "react/renderer/mounting/tests"
    ss.header_dir           = "react/renderer/mounting"
  end

  s.subspec "observers" do |ss|
    ss.subspec "events" do |sss|
      sss.source_files         = podspec_sources("react/renderer/observers/events/**/*.{m,mm,cpp,h}", "react/renderer/observers/events/**/*.h")
      sss.exclude_files        = "react/renderer/observers/events/tests"
      sss.header_dir           = "react/renderer/observers/events"
    end

    ss.subspec "intersection" do |sss|
      sss.source_files         = podspec_sources("react/renderer/observers/intersection/**/*.{m,mm,cpp,h}", "react/renderer/observers/intersection/**/*.h")
      sss.exclude_files        = "react/renderer/observers/intersection/tests"
      sss.header_dir           = "react/renderer/observers/intersection"
    end

    ss.subspec "mutation" do |sss|
      sss.source_files         = podspec_sources("react/renderer/observers/mutation/**/*.{m,mm,cpp,h}", "react/renderer/observers/mutation/**/*.h")
      sss.exclude_files        = "react/renderer/observers/mutation/tests"
      sss.header_dir           = "react/renderer/observers/mutation"
    end

    ss.subspec "resize" do |sss|
      sss.source_files         = podspec_sources("react/renderer/observers/resize/**/*.{m,mm,cpp,h}", "react/renderer/observers/resize/**/*.h")
      sss.exclude_files        = "react/renderer/observers/resize/tests"
      sss.header_dir           = "react/renderer/observers/resize"
    end
  end

  s.subspec "telemetry" do |ss|
    ss.source_files         = podspec_sources("react/renderer/telemetry/**/*.{m,mm,cpp,h}", "react/renderer/telemetry/**/*.h")
    ss.exclude_files        = "react/renderer/telemetry/tests"
    ss.header_dir           = "react/renderer/telemetry"

  end

  s.subspec "consistency" do |ss|
    ss.source_files         = podspec_sources("react/renderer/consistency/**/*.{m,mm,cpp,h}", "react/renderer/consistency/**/*.h")
    ss.header_dir           = "react/renderer/consistency"
  end

  s.subspec "uimanager" do |ss|
    ss.subspec "consistency" do |sss|
      sss.source_files         = podspec_sources("react/renderer/uimanager/consistency/*.{m,mm,cpp,h}", "react/renderer/uimanager/consistency/*.h")
      sss.header_dir           = "react/renderer/uimanager/consistency"
    end

    ss.dependency             "React-rendererconsistency"
    ss.dependency             "React-Fabric/loomRenderer"
    ss.source_files         = podspec_sources("react/renderer/uimanager/*.{m,mm,cpp,h}", "react/renderer/uimanager/*.h")
    ss.header_dir           = "react/renderer/uimanager"
  end

  # Loom: the renderer module (react/renderer/loom/{binding,core,mounting,worker,text,host,compat}).
  s.subspec "loomRenderer" do |ss|
    ss.dependency             "React-Fabric/core"
    ss.dependency             "React-Fabric/components/view"
    ss.dependency             "React-Fabric/componentregistry"
    ss.dependency             "React-Fabric/consistency"
    ss.source_files         = podspec_sources("react/renderer/loom/{binding,core,mounting,worker,text,host,compat}/**/*.{m,mm,cpp,h}", "react/renderer/loom/{binding,core,mounting,worker,text,host,compat}/**/*.h")
    ss.header_dir           = ""
    ss.header_mappings_dir  = "./"
  end

  # Loom: the library API (<react/loom/Loom.h>) and the build configuration header
  # (<react/renderer/LoomConfig.h>, which lives in loom/config so it can be published at that path).
  s.subspec "loom" do |ss|
    ss.dependency             "React-Fabric/animationbackend"
    ss.dependency             "React-Fabric/core"
    ss.dependency             "React-runtimescheduler"
    ss.source_files         = podspec_sources("react/loom/*.h", "react/loom/*.h")
    ss.header_dir           = "react/loom"
  end

  s.subspec "loomConfig" do |ss|
    ss.source_files         = podspec_sources("react/loom/config/*.h", "react/loom/config/*.h")
    ss.header_dir           = "react/renderer"
  end

  s.subspec "leakchecker" do |ss|
    ss.source_files         = podspec_sources("react/renderer/leakchecker/**/*.{cpp,h}", "react/renderer/leakchecker/**/*.h")
    ss.exclude_files        = "react/renderer/leakchecker/tests"
    ss.header_dir           = "react/renderer/leakchecker"
    ss.pod_target_xcconfig  = { "GCC_WARN_PEDANTIC" => "YES" }
  end

  s.subspec "viewtransition" do |ss|
    ss.source_files         = podspec_sources("react/renderer/viewtransition/**/*.{m,mm,cpp,h}", "react/renderer/viewtransition/**/*.h")
    ss.header_dir           = "react/renderer/viewtransition"
  end

  mark_as_react_native_build(s)
end
