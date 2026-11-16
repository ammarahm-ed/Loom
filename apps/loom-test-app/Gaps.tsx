/**
 * Repros for renderer gaps the desktop port found. Each case runs on its own
 * and logs `[gaps] <case> PASS|FAIL <detail>`, so a run is checked from the log.
 *
 * @format
 */
import React, {Activity, Suspense, use, useEffect, useRef, useState} from 'react';
import {
  Appearance,
  useColorScheme,
  experimental_LayoutConformance as LayoutConformance,
  NativeModules,
  PlatformColor,
  Text,
  View,
} from 'react-native';

function log(line: string) {
  console.log('[gaps] ' + line);
  NativeModules.LoomBuildConfig?.log?.('[gaps] ' + line);
}

const wait = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));

function measure(view: View | null): Promise<{w: number; h: number}> {
  return new Promise(resolve => {
    if (view == null) {
      resolve({w: -1, h: -1});
      return;
    }
    view.measure((_x, _y, w, h) => resolve({w, h}));
  });
}

// react-freeze: a Suspense boundary whose child suspends forever while frozen.
const never = new Promise<void>(() => {});
function Suspender({freeze, children}: {freeze: boolean; children: React.ReactNode}) {
  if (freeze) {
    use(never);
  }
  return <>{children}</>;
}
function Freeze({freeze, children}: {freeze: boolean; children: React.ReactNode}) {
  return (
    <Suspense fallback={null}>
      <Suspender freeze={freeze}>{children}</Suspender>
    </Suspense>
  );
}

function HideCycle() {
  const [visible, setVisible] = useState(true);
  const [tick, setTick] = useState(0);
  const activity = useRef<View>(null);
  const freeze = useRef<View>(null);
  useEffect(() => {
    (async () => {
      await wait(400);
      const before = [await measure(activity.current), await measure(freeze.current)];
      setVisible(false);
      await wait(400);
      setTick(1); // a props update while hidden
      await wait(400);
      setVisible(true);
      await wait(600);
      const after = [await measure(activity.current), await measure(freeze.current)];
      const ok = after.every((m, i) => m.w > 0 && m.h === before[i].h);
      // measure() reads layout, which display: none keeps, so it cannot see
      // the bug: the screenshot after this line shows whether both boxes are back.
      log(`hide-cycle DONE sizesKept=${ok} before=${JSON.stringify(before)} after=${JSON.stringify(after)}`);
    })();
  }, []);
  const box = (ref: React.RefObject<View | null>, name: string) => (
    <View ref={ref} collapsable={false} style={{height: 24, backgroundColor: tick ? '#0a0' : '#a00'}}>
      <Text>{`${name} ${tick}`}</Text>
    </View>
  );
  return (
    <View>
      <Activity mode={visible ? 'visible' : 'hidden'}>{box(activity, 'activity')}</Activity>
      <Freeze freeze={!visible}>{box(freeze, 'freeze')}</Freeze>
    </View>
  );
}

const INNER: [string, object][] = [
  ['flex', {flex: 1}],
  ['points', {flexGrow: 1, flexShrink: 1, flexBasis: 0}],
  ['percent', {flexGrow: 1, flexShrink: 1, flexBasis: '0%'}],
  ['auto', {flexGrow: 1, flexShrink: 1, flexBasis: 'auto'}],
];

function FlexBasis({strict}: {strict: boolean}) {
  const widths = useRef<Record<string, number>>({});
  useEffect(() => {
    const timer = setTimeout(() => {
      const got = INNER.map(([id]) => widths.current[id]);
      // Yoga sizes these the same in both modes (React Native's own result,
      // not the browser's), so the widths are reported, and the errata the
      // mode selects are checked on an absolute child without insets.
      const left = widths.current.absolute;
      const ok = left === (strict ? 10 : 0);
      log(`conformance-${strict ? 'strict' : 'classic'} ${ok ? 'PASS' : 'FAIL'} absoluteLeft=${left} rowItems=${JSON.stringify(got)}`);
    }, 800);
    return () => clearTimeout(timer);
  }, [strict]);
  const rows = (
    <View style={{width: 400}}>
      {INNER.map(([id, inner]) => (
        <View key={id} style={{flexDirection: 'row'}}>
          <View style={{flexDirection: 'row', opacity: 1}}>
            <View
              collapsable={false}
              onLayout={e => (widths.current[id] = e.nativeEvent.layout.width)}
              style={{flexDirection: 'row', alignItems: 'center', paddingHorizontal: 4}}>
              <View style={{...inner, flexDirection: 'row', alignItems: 'center'}}>
                <View style={{width: 13, height: 13, backgroundColor: '#3b82f6'}} />
                <View style={{width: 140, height: 14, marginLeft: 6, backgroundColor: '#999'}} />
              </View>
            </View>
          </View>
        </View>
      ))}
      <View style={{width: 100, height: 40, padding: 10}}>
        <View
          collapsable={false}
          onLayout={e => (widths.current.absolute = e.nativeEvent.layout.x)}
          style={{position: 'absolute', width: 20, height: 20, backgroundColor: '#0a0'}}
        />
      </View>
    </View>
  );
  return strict ? <LayoutConformance mode="strict">{rows}</LayoutConformance> : rows;
}

// Checked from a screenshot: the swatch is at the top left of the screen.
// The app is Light in Info.plist, so dark comes from Appearance.setColorScheme.
// The swatches remount per scheme as in the desktop fixture; the first resolves
// its colour in render from Appearance, as the desktop PlatformColor does.
function Colors() {
  const scheme = useColorScheme();
  useEffect(() => {
    Appearance.setColorScheme('dark');
  }, []);
  useEffect(() => {
    const timer = setTimeout(
      () => log(`colors READY scheme=${scheme} atRender=${Appearance.getColorScheme()}`),
      800,
    );
    return () => clearTimeout(timer);
  }, [scheme]);
  const jsDark = Appearance.getColorScheme() === 'dark';
  return (
    <View key={String(scheme)} style={{flexDirection: 'row'}}>
      <View style={{width: 120, height: 120, backgroundColor: jsDark ? '#ff453a' : '#ff3b30'}} />
      <View testID="color-platform" style={{width: 120, height: 120, backgroundColor: PlatformColor('systemRedColor')}} />
      <View testID="color-fallback" style={{width: 120, height: 120, backgroundColor: PlatformColor('noSuchColorZzz', 'systemBlue')}} />
    </View>
  );
}

export default function Gaps({scenario}: {scenario: string}) {
  return (
    <View style={{flex: 1, paddingTop: 80, backgroundColor: '#fff'}}>
      {scenario === 'gaps-hide' ? <HideCycle /> : null}
      {scenario === 'gaps-basis' ? (
        <>
          <FlexBasis strict={false} />
          <FlexBasis strict />
        </>
      ) : null}
      {scenario === 'gaps-colors' ? <Colors /> : null}
    </View>
  );
}
