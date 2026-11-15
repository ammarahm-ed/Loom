/**
 * Test app for the Loom renderer.
 *
 * The build matrix proves the switches compile and link; this screen is the
 * runtime half — it is what gets installed on a device to check the renderer
 * mounts, commits and tears down under load. See LoomSmokeTest.tsx.
 *
 * @format
 */

import { StatusBar, useColorScheme } from 'react-native';
import { SafeAreaProvider } from 'react-native-safe-area-context';
import Bench from './Bench';
import Compat from './Compat';
import LoomExample from './LoomExample';
import LoomSmokeTest from './LoomSmokeTest';

// `scenario` comes from the launch intent (see MainActivity) when bench/run.js drives the app.
function App({
  scenario,
  autoscroll,
}: {
  scenario?: string;
  autoscroll?: boolean;
}) {
  const isDarkMode = useColorScheme() === 'dark';

  return (
    <SafeAreaProvider>
      <StatusBar barStyle={isDarkMode ? 'light-content' : 'dark-content'} />
      {scenario === 'compat' ? (
        <Compat />
      ) : scenario === 'example' ? (
        <LoomExample />
      ) : scenario ? (
        <Bench scenario={scenario} autoscroll={autoscroll} />
      ) : (
        <LoomSmokeTest />
      )}
    </SafeAreaProvider>
  );
}

export default App;