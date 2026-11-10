/**
 * Loom integration test.
 *
 * The point of the out-of-tree model is that the app resolves `react-native` from
 * the materialized tree (pristine react-native + overrides) and not from
 * node_modules/react-native. If the Jest mapping in jest.config.js regresses, or
 * `loom materialize` did not run, this fails instead of silently testing stock
 * React Native.
 */

test('react-native resolves to the materialized tree', () => {
  expect(require.resolve('react-native')).toContain('node_modules/.loom/react-native/');
});

test('an overridden JS module resolves to the materialized copy', () => {
  expect(
    require.resolve('react-native/Libraries/LayoutAnimation/LayoutAnimation'),
  ).toContain('node_modules/.loom/react-native/Libraries/LayoutAnimation/LayoutAnimation.js');
});

test('the app does not fall back to the pristine react-native', () => {
  expect(require.resolve('react-native')).not.toMatch(/node_modules\/react-native\//);
});