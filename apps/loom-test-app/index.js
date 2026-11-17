/**
 * @format
 */

import { AppRegistry, NativeModules } from 'react-native';
import App from './App';
import { name as appName } from './app.json';

AppRegistry.registerComponent(appName, () => App);

// Ask the native module which thread this call lands on (it runs synchronously on the
// JS thread) and print it, so a run shows whether JavaScript is on the main thread (iOS and Android).
const jsThread = NativeModules.LoomBuildConfig?.jsThread?.();
if (jsThread) {
  NativeModules.LoomBuildConfig.log(`jsThread ${JSON.stringify(jsThread)}`);
}
