# Loom iOS integration. In the app's Podfile, replace
#
#   require Pod::Executable.execute_command('node', ['-p',
#     'require.resolve("react-native/scripts/react_native_pods.rb", {paths: [process.argv[1]]})', __dir__]).strip
#
# with
#
#   require_relative '../node_modules/react-native-loom/integration/ios/loom_pods'
#   require File.join(Loom.react_native_path(__dir__), 'scripts/react_native_pods')
#
# and pass `:path => Loom.react_native_path(__dir__)` to `use_react_native!`.
# (react-native.config.js `reactNativePath` does the same for autolinking; see docs.)

require 'json'
require 'open3'

module Loom
  LOOM_BIN = File.expand_path('../../bin/loom.js', __dir__)

  # The public switches, with the same names and meaning as android/gradle.properties:
  #
  #   loom.renderer          true | false       Loom, or plain React Native (Fabric)            (default: true)
  #   loom.js                main | background  where JavaScript runs                          (default: background)
  #   loom.worker            true | false       Loom's worker thread (prebuilt: always true)   (default: true)
  #   loom.compileFromSource true | false       build Loom from source instead of the prebuilt (default: false)
  #   loom.releaseProfiling  true | false       Release with DevTools and tracing kept         (default: false)
  #
  # Set them in ios/Podfile.properties.json (`{"loom.renderer": "true", ...}`) or, for CI, in the
  # environment with env(1): `env loom.renderer=true pod install`. The environment wins.
  def self.setting(ios_dir, key)
    return ENV[key] if ENV[key] && !ENV[key].empty?
    file = File.join(ios_dir, 'Podfile.properties.json')
    return nil unless File.exist?(file)
    value = JSON.parse(File.read(file))[key]
    value.nil? ? nil : value.to_s
  end

  def self.boolean(ios_dir, key, default)
    value = setting(ios_dir, key)
    return default if value.nil?
    raise "Loom: #{key} must be true or false, got '#{value}'" unless %w[true false].include?(value)
    value == 'true'
  end

  # Applies the switches to the environment the Loom podspecs, Package.swift and rncore.rb read, and
  # returns :stock (plain React Native), :prebuilt (Loom's published core) or :source.
  def self.configure(ios_dir)
    renderer = boolean(ios_dir, 'loom.renderer', true)
    compile = boolean(ios_dir, 'loom.compileFromSource', false)
    js = setting(ios_dir, 'loom.js') || 'background'
    raise "Loom: loom.js must be main or background, got '#{js}'" unless %w[main background].include?(js)
    worker = boolean(ios_dir, 'loom.worker', true)

    # The internal variables are derived, never set by apps.
    %w[RCT_LOOM_RENDERER_IOS RCT_LOOM_WORKER_IOS RCT_LOOM_JS_IOS RCT_LOOM_PROFILING_IOS].each { |k| ENV.delete(k) }
    if renderer
      ENV['RCT_LOOM_RENDERER_IOS'] = '1'
      ENV['RCT_LOOM_JS_IOS'] = js
      # A prebuilt has the worker built in; loom.worker=false only exists in a source build.
      if !compile && !worker
        Pod::UI.warn 'Loom: loom.worker=false only takes effect when compiling from source; the prebuilt has the worker built in. Set loom.compileFromSource=true to build without it.' if defined?(Pod::UI)
        worker = true
      end
      ENV['RCT_LOOM_WORKER_IOS'] = worker ? '1' : '0'
    end

    # Release with DevTools and tracing: the release-profiling core of the prebuilt, or the debugger
    # macros in Release pods of a source build (scripts/cocoapods/utils.rb). Without the renderer it
    # applies to the source build only (React Native's own prebuilt has no such flavor).
    ENV['RCT_LOOM_PROFILING_IOS'] = '1' if boolean(ios_dir, 'loom.releaseProfiling', false)

    return :stock if !renderer && !compile
    ENV['RCT_USE_PREBUILT_RNCORE'] = compile ? '0' : '1'
    compile ? :source : :prebuilt
  end

  # Returns the absolute react-native path to build with. `ios_dir` is the directory containing the
  # Podfile; the app root is its parent unless LOOM_APP_ROOT is set. Plain React Native (renderer off,
  # no source build) returns the stock package, so React Native's own prebuilt artifacts are used;
  # everything else materializes react-native + overrides.
  def self.react_native_path(ios_dir)
    @path ||= begin
      app_root = ENV['LOOM_APP_ROOT'] || File.expand_path('..', ios_dir)
      mode = configure(ios_dir)
      unless ENV['LOOM_SKIP_MATERIALIZE'] == '1'
        out, status = Open3.capture2e('node', LOOM_BIN, 'materialize', chdir: app_root)
        raise "loom materialize failed:\n#{out}" unless status.success?
        Pod::UI.puts(out.strip) if defined?(Pod::UI)
      end
      dir = File.join(app_root, 'node_modules', '.loom', 'react-native')
      raise "Loom: #{dir} missing; run `npx loom materialize`" unless File.exist?(File.join(dir, '.loom-stamp.json'))
      Pod::UI.puts "Loom: #{mode}" if defined?(Pod::UI)
      if mode == :stock
        stock, status = Open3.capture2('node', '-p', "require('path').dirname(require.resolve('react-native/package.json', {paths: [process.argv[1]]}))", app_root)
        raise 'Loom: cannot resolve react-native' unless status.success?
        stock.strip
      else
        dir
      end
    end
  end
end

# Reanimated animates through React Native's shared animation backend, which Loom implements on
# its own renderer (no shadow tree). Reanimated compiles that choice in from static feature flags
# (`ReanimatedUtils.get_static_feature_flags`, read by its podspec), so switch them:
# USE_ANIMATION_BACKEND on, and FORCE_REACT_RENDER_FOR_SETTLED_ANIMATIONS (which cannot be
# combined with it) off. Prepending works before Reanimated's helper is loaded.
module ReanimatedUtils; end

module Loom
  def self.reanimated_static_flags(flags)
    return flags unless flags.include?('USE_ANIMATION_BACKEND') # no backend in this Reanimated
    flags
      .gsub(/\[USE_ANIMATION_BACKEND:[a-z]+\]/, '[USE_ANIMATION_BACKEND:true]')
      .gsub(/\[FORCE_REACT_RENDER_FOR_SETTLED_ANIMATIONS:[a-z]+\]/,
            '[FORCE_REACT_RENDER_FOR_SETTLED_ANIMATIONS:false]')
  end

  module ReanimatedStaticFlags
    def get_static_feature_flags
      Loom.reanimated_static_flags(super)
    end
  end
end

ReanimatedUtils.singleton_class.prepend(Loom::ReanimatedStaticFlags)

# Reanimated adapter (adapters/reanimated): for a supported Reanimated, the RNReanimated pod leaves
# out its ReanimatedModuleProxy.cpp, which the autolinked RNLoomReanimated pod compiles in its
# adapted form. node_modules is not modified.
module Loom
  def self.reanimated_adapter_supported?
    return @reanimated_adapter_supported unless @reanimated_adapter_supported.nil?
    @reanimated_adapter_supported = begin
      app_root = ENV['LOOM_APP_ROOT'] || File.expand_path('..', Pod::Config.instance.installation_root.to_s)
      package = `node --print "require.resolve('react-native-reanimated/package.json', {paths: [process.argv[1]]})" "#{app_root}" 2>/dev/null`.strip
      supported = JSON.parse(File.read(File.expand_path('../../adapters/reanimated/supported.json', __dir__)))['versions']
      !package.empty? && File.exist?(package) && supported.include?(JSON.parse(File.read(package))['version'])
    rescue StandardError
      false
    end
  end

  module ReanimatedSourceExclusion
    def initialize(parent = nil, name = nil, *args, **kwargs, &block)
      super
      if parent && parent.name == 'RNReanimated' && name == 'common' && Loom.reanimated_adapter_supported?
        self.exclude_files = 'Common/cpp/reanimated/NativeModules/ReanimatedModuleProxy.cpp'
      end
    end
  end
end

Pod::Specification.prepend(Loom::ReanimatedSourceExclusion) if defined?(Pod::Specification)
