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

  # Materializes react-native + overrides and returns the absolute path.
  # `ios_dir` is the directory containing the Podfile; the app root is its parent
  # unless LOOM_APP_ROOT is set.
  def self.react_native_path(ios_dir)
    @path ||= begin
      app_root = ENV['LOOM_APP_ROOT'] || File.expand_path('..', ios_dir)
      unless ENV['LOOM_SKIP_MATERIALIZE'] == '1'
        out, status = Open3.capture2e('node', LOOM_BIN, 'materialize', chdir: app_root)
        raise "loom materialize failed:\n#{out}" unless status.success?
        Pod::UI.puts(out.strip) if defined?(Pod::UI)
      end
      dir = File.join(app_root, 'node_modules', '.loom', 'react-native')
      raise "Loom: #{dir} missing; run `npx loom materialize`" unless File.exist?(File.join(dir, '.loom-stamp.json'))
      # Overrides patch React-Core sources; prebuilt RN core binaries cannot contain them.
      ENV['RCT_USE_PREBUILT_RNCORE'] = '0'
      dir
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
