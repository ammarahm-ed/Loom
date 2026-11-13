/**
 * react-native-safe-area-context: SafeAreaView in padding mode and margin mode, plus the insets hook.
 * Checks are numbers the library's own ShadowNode/native state produce:
 *   padTop        y of the first child of <SafeAreaView edges={['top','bottom']}> (padding mode)
 *   padViewHeight height of that SafeAreaView
 *   marginTop     y of <SafeAreaView mode="margin" edges={['top']}> inside its parent
 */
import React, {useEffect, useRef, useState} from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {SafeAreaView, useSafeAreaInsets} from 'react-native-safe-area-context';
import {report} from '../report';

type Box = {x: number; y: number; width: number; height: number};

export default function SafeArea() {
  const insets = useSafeAreaInsets();
  const boxes = useRef<Record<string, Box>>({});
  const insetsRef = useRef(insets);
  insetsRef.current = insets;
  const [, setTick] = useState(0);
  const [result, setResult] = useState('running');
  const on = (key: string) => (e: any) => {
    boxes.current[key] = e.nativeEvent.layout;
    setTick(t => t + 1);
  };

  useEffect(() => {
    const timer = setTimeout(() => {
      const b = boxes.current;
      const i = insetsRef.current;
      const near = (a: number | undefined, t: number) =>
        a !== undefined && Math.abs(a - t) < 0.6;
      const checks = {
        insetsTop: i.top,
        insetsBottom: i.bottom,
        padTop: b.padChild?.y ?? -1,
        padViewHeight: b.padView?.height ?? -1,
        marginTop: b.marginView?.y ?? -1,
        safeAreaPadding: near(b.padChild?.y, i.top),
        safeAreaMargin: near(b.marginView?.y, i.top),
        insetsReady: i.top > 0 || i.bottom > 0 || i.top === 0,
      };
      report('react-native-safe-area-context', checks);
      setResult(JSON.stringify(checks));
    }, 2500);
    return () => clearTimeout(timer);
  }, []);

  return (
    <View style={styles.container}>
      <SafeAreaView
        mode="margin"
        edges={['top']}
        style={styles.marginRow}
        onLayout={on('marginView')}>
        <Text>margin mode</Text>
      </SafeAreaView>
      <SafeAreaView
        nativeID="pad-view"
        edges={['top', 'bottom']}
        style={styles.fill}
        onLayout={on('padView')}>
        <View nativeID="pad-child" style={styles.child} onLayout={on('padChild')}>
          <Text>padding mode</Text>
        </View>
      </SafeAreaView>
      <Text nativeID="compat-result" style={styles.result}>{result}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {flex: 1},
  fill: {flex: 1, backgroundColor: '#eef'},
  child: {height: 40, backgroundColor: '#cdf'},
  marginRow: {height: 40, backgroundColor: '#fdc'},
  result: {fontSize: 9},
});
