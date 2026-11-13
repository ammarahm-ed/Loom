/**
 * react-native-gesture-handler (v3): GestureDetector with a Tap and a Pan, inside a ScrollView.
 * Driven by Maestro (maestro/gesture.yaml) with real touches. Each completed gesture reports one
 * [compat] line; the on-screen text carries the same counters so iOS can be read from a screenshot.
 */
import React, {useState} from 'react';
import {ScrollView, StyleSheet, Text, View} from 'react-native';
import {Gesture, GestureDetector, GestureHandlerRootView} from 'react-native-gesture-handler';
import {report} from '../report';

export default function Gestures() {
  const [taps, setTaps] = useState(0);
  const [panEnds, setPanEnds] = useState(0);
  const [panX, setPanX] = useState(0);

  const tap = Gesture.Tap()
    .runOnJS(true)
    .onEnd(() => setTaps(t => t + 1));

  const pan = Gesture.Pan()
    .runOnJS(true)
    .onEnd(e => {
      setPanX(e.translationX);
      setPanEnds(n => n + 1);
    });

  // Report after every completed gesture so the logs carry the latest state.
  React.useEffect(() => {
    if (taps === 0 && panEnds === 0) {
      return;
    }
    report('react-native-gesture-handler', {
      taps,
      panEnds,
      panX: Math.round(panX),
      pass: taps >= 1 && panEnds >= 1 && Math.abs(panX) > 50,
    });
  }, [taps, panEnds, panX]);

  return (
    <GestureHandlerRootView style={styles.root}>
      <ScrollView contentContainerStyle={styles.content}>
        <GestureDetector gesture={tap}>
          <View nativeID="tap-box" style={styles.tapBox}>
            <Text>tap me</Text>
          </View>
        </GestureDetector>
        <GestureDetector gesture={pan}>
          <View nativeID="pan-box" style={styles.panBox}>
            <Text>swipe me</Text>
          </View>
        </GestureDetector>
        <Text nativeID="gesture-result" style={styles.result}>
          {`taps: ${taps} panEnds: ${panEnds} panX: ${Math.round(panX)}`}
        </Text>
        <View style={styles.filler} />
      </ScrollView>
    </GestureHandlerRootView>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1},
  content: {padding: 16},
  tapBox: {height: 90, backgroundColor: '#cdf', justifyContent: 'center', alignItems: 'center'},
  panBox: {height: 160, marginTop: 16, backgroundColor: '#fdc', justifyContent: 'center', alignItems: 'center'},
  result: {marginTop: 16, fontSize: 16},
  filler: {height: 1200},
});
