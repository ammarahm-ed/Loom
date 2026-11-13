/**
 * Fabric compatibility checks: third-party components whose C++ ShadowNodes carry their own layout
 * or state logic, used exactly as their docs show. Launched as `-scenario compat` (iOS) or
 * `--es scenario compat` (Android); prints one `[bench] result {...}` line after the native side
 * has had time to publish its state (safe-area insets arrive asynchronously).
 *
 * Every number is something stock Fabric produces from the library's own C++ code:
 *   padTop     y of the first child of <SafeAreaView edges={['top','bottom']}> (padding mode);
 *              RNCSafeAreaViewShadowNode::adjustLayoutWithState turns the native inset state into
 *              padding, so it must equal the provider's insets.top
 *   padHeight  that SafeAreaView's inner height = its height - insets.top - insets.bottom
 *   marginTop  y of <SafeAreaView mode="margin" edges={['top']}> inside its parent (margin mode)
 *   switchW/H  size of an unsized <Switch />, which only its ShadowNode's measureContent knows
 *
 * @format
 */

import React, { useEffect, useRef, useState } from 'react';
import { NativeModules, StyleSheet, Switch, Text, View } from 'react-native';
import {
  SafeAreaView,
  useSafeAreaInsets,
} from 'react-native-safe-area-context';

type Box = { x: number; y: number; width: number; height: number };

function report(data: Record<string, number | string | boolean>) {
  const line = `[bench] result ${JSON.stringify({ scenario: 'compat', ...data })}`;
  console.log(line);
  NativeModules.LoomBuildConfig?.log?.(line);
}

const near = (a: number | undefined, b: number) =>
  a !== undefined && Math.abs(a - b) < 0.6;

export default function Compat() {
  const insets = useSafeAreaInsets();
  const boxes = useRef<Record<string, Box>>({});
  const [, setTick] = useState(0);
  const on = (key: string) => (e: any) => {
    boxes.current[key] = e.nativeEvent.layout;
    setTick(t => t + 1);
  };
  const insetsRef = useRef(insets);
  insetsRef.current = insets;

  useEffect(() => {
    const timer = setTimeout(() => {
      const b = boxes.current;
      const i = insetsRef.current;
      const padTop = b.padChild?.y;
      const padHeight =
        b.padView !== undefined && b.padChild !== undefined
          ? b.padView.height
          : undefined;
      const marginTop = b.marginView?.y;
      const result = {
        insetsTop: i.top,
        insetsBottom: i.bottom,
        padTop: padTop ?? -1,
        padViewHeight: padHeight ?? -1,
        padChildBottom:
          b.padChild && b.padView
            ? b.padView.height - (b.padChild.y + b.padChild.height)
            : -1,
        marginTop: marginTop ?? -1,
        switchW: b.switch?.width ?? -1,
        switchH: b.switch?.height ?? -1,
        safeAreaPadding: near(padTop, i.top),
        safeAreaMargin: near(marginTop, i.top),
        switchMeasured: (b.switch?.width ?? 0) > 0 && (b.switch?.height ?? 0) > 0,
      };
      report(result);
    }, 2500);
    return () => clearTimeout(timer);
  }, []);

  return (
    <View nativeID="bench-ready" style={styles.container}>
      <View style={styles.row}>
        <SafeAreaView
          edges={['top', 'bottom']}
          style={styles.half}
          onLayout={on('padView')}>
          <View style={styles.child} onLayout={on('padChild')}>
            <Text>padding</Text>
          </View>
          <View style={styles.filler} />
        </SafeAreaView>
        <View style={styles.half}>
          <SafeAreaView
            mode="margin"
            edges={['top']}
            style={styles.marginBox}
            onLayout={on('marginView')}>
            <Text>margin</Text>
          </SafeAreaView>
          <View style={styles.switchRow}>
            <View onLayout={on('switch')}>
              <Switch value />
            </View>
          </View>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: 'white' },
  row: { flex: 1, flexDirection: 'row' },
  half: { flex: 1, backgroundColor: '#eef' },
  child: { height: 40, backgroundColor: '#9cf' },
  filler: { flex: 1, backgroundColor: '#cfc' },
  marginBox: { height: 50, backgroundColor: '#fc9' },
  switchRow: { flexDirection: 'row', padding: 8 },
});
