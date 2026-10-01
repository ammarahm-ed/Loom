/**
 * Test app for the Resha direct renderer.
 *
 * The build matrix proves the switches compile and link; this screen is the
 * runtime half — it is what gets installed on a device to check the renderer
 * mounts, commits and tears down under load. See ReshaSmokeTest.tsx.
 *
 * @format
 */

import { StatusBar, useColorScheme } from 'react-native';
import { SafeAreaProvider } from 'react-native-safe-area-context';
import ReshaSmokeTest from './ReshaSmokeTest';

function App() {
  const isDarkMode = useColorScheme() === 'dark';

  return (
    <SafeAreaProvider>
      <StatusBar barStyle={isDarkMode ? 'light-content' : 'dark-content'} />
      <ReshaSmokeTest />
    </SafeAreaProvider>
  );
}

export default App;