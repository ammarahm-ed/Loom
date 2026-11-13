/**
 * Loom compat app: one self-check screen per library. Launch with a scenario:
 *   Android: adb shell am start -n com.loomcompatapp/.MainActivity --es scenario <lib>|all
 *   iOS:     xcrun simctl launch <udid> <bundle id> -scenario <lib>|all
 * Each screen reports one result (see report.ts). `all` mounts every non-touch screen in turn.
 *
 * @format
 */

import React, {useEffect, useState, useLayoutEffect} from 'react';
import {StatusBar, Text, View, useColorScheme} from 'react-native';
import {SafeAreaProvider} from 'react-native-safe-area-context';
import SafeArea from './screens/SafeArea';
import Navigation from './screens/Navigation';
import Gesture from './screens/Gesture';
import Reanimated from './screens/Reanimated';
import LayoutAnimations from './screens/LayoutAnimations';
import SharedTransitions from './screens/SharedTransitions';
import AnimationDemo from './screens/AnimationDemo';
import LayoutFuzz from './screens/LayoutFuzz';
import Unistyles from './screens/Unistyles';
import * as B from './screens/Basics';
import * as C from './screens/Composite';
import {Result, onReport, report} from './report';

const screens: Record<string, React.ComponentType> = {
  'react-native-safe-area-context': SafeArea,
  'react-native-screens': Navigation,
  'react-native-reanimated': Reanimated,
  'react-native-reanimated layout animations': LayoutAnimations,
  'react-native-reanimated shared transitions': SharedTransitions,
  'react-native-svg': B.SvgScreen,
  'react-native-pager-view': B.PagerScreen,
  '@shopify/flash-list': B.FlashListScreen,
  '@legendapp/list': B.LegendListScreen,
  '@shopify/react-native-skia': B.SkiaScreen,
  'lottie-react-native': B.LottieScreen,
  'react-native-webview': B.WebViewScreen,
  '@react-native-community/slider': B.SliderScreen,
  '@react-native-community/datetimepicker': B.DateTimeScreen,
  'react-native-linear-gradient': B.GradientScreen,
  'react-native-maps': B.MapsScreen,
  'react-native-video': B.VideoScreen,
  'react-native-vision-camera': B.CameraScreen,
  '@react-navigation/bottom-tabs': C.BottomTabsScreen,
  '@gorhom/bottom-sheet': C.BottomSheetScreen,
  'react-native-keyboard-controller': C.KeyboardScreen,
  'react-native-unistyles': Unistyles,
  // Needs real touches: run with maestro/gesture.yaml, not part of `all`.
  'react-native-gesture-handler': Gesture,
  // A looping visual demo, not a check.
  demo: AnimationDemo,
};
const TOUCH_ONLY = new Set(['react-native-gesture-handler', 'demo']);

function Empty() {
  return (
    <View nativeID="compat-ready" style={{flex: 1, justifyContent: 'center', alignItems: 'center'}}>
      <Text>Loom compat app: no scenario selected</Text>
    </View>
  );
}

// A screen that throws while rendering reports the error as its result instead of unmounting the app.
class Boundary extends React.Component<{lib: string; children: React.ReactNode}, {error?: string}> {
  state: {error?: string} = {};
  static getDerivedStateFromError(e: any) {
    return {error: String(e?.message ?? e).slice(0, 300)};
  }
  componentDidCatch(e: any) {
    report(this.props.lib, {error: false, message: String(e?.message ?? e).slice(0, 300)});
  }
  render() {
    return this.state.error ? <Text>{`${this.props.lib}: ${this.state.error}`}</Text> : this.props.children;
  }
}

// Mounts each screen until it reports (or times out), then shows a one-line-per-library summary.
function All({only}: {only?: string[]}) {
  const libs = only ?? Object.keys(screens).filter(l => !TOUCH_ONLY.has(l));
  const [index, setIndex] = useState(0);
  const [results, setResults] = useState<Result[]>([]);
  const lib = libs[index];
  // A layout effect: a screen may report from its own mount effect, which runs before any
  // passive effect of this parent, so the listener must already be registered by then.
  useLayoutEffect(() => {
    if (!lib) {
      return;
    }
    let advanced = false;
    const next = () => {
      if (!advanced) {
        advanced = true;
        setIndex(i => i + 1);
      }
    };
    const off = onReport(r => {
      clearTimeout(timeout);
      setResults(rs => [...rs, r]);
      setTimeout(next, 300);
    });
    const timeout: ReturnType<typeof setTimeout> = setTimeout(() => {
      report(lib, {timeout: false});
    }, 12000);
    return () => {
      off();
      clearTimeout(timeout);
    };
  }, [lib]);
  if (!lib) {
    const failed = results.filter(r => !r.pass);
    return (
      <View style={{flex: 1, padding: 12, paddingTop: 60}}>
        <Text nativeID="compat-summary" style={{fontSize: 14, fontWeight: 'bold'}}>
          {`done ${results.length - failed.length}/${results.length} pass`}
        </Text>
        {results.map(r => (
          <Text key={r.lib} style={{fontSize: 11}}>
            {`${r.pass ? 'PASS' : 'FAIL'} ${r.lib}`}
          </Text>
        ))}
      </View>
    );
  }
  const Screen = screens[lib];
  return (
    <View style={{flex: 1}} key={lib}>
      <Boundary lib={lib}>
        <Screen />
      </Boundary>
    </View>
  );
}

function App({scenario}: {scenario?: string}) {
  const isDarkMode = useColorScheme() === 'dark';
  let body: React.ReactNode;
  if (scenario === 'all') {
    body = <All />;
  } else if (scenario && scenario.startsWith('fuzz')) {
    // fuzz[:seed[:steps]]
    const [, seed, steps] = scenario.split(':');
    body = <LayoutFuzz seed={seed ? Number(seed) : undefined} steps={steps ? Number(steps) : undefined} />;
  } else if (scenario && scenario.includes(',')) {
    body = <All only={scenario.split(',')} />;
  } else {
    const Screen = (scenario && screens[scenario]) || Empty;
    body = (
      <Boundary lib={scenario ?? 'none'}>
        <Screen />
      </Boundary>
    );
  }
  return (
    <SafeAreaProvider>
      <StatusBar barStyle={isDarkMode ? 'light-content' : 'dark-content'} />
      {body}
    </SafeAreaProvider>
  );
}

export default App;
