module.exports = {
  presets: ['module:@react-native/babel-preset'],
  plugins: [
    // Unistyles links styled components to their native nodes; only files under `screens/`.
    ['react-native-unistyles/plugin', {root: 'screens'}],
    'react-native-worklets/plugin',
  ],
};
