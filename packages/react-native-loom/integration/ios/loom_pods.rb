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
