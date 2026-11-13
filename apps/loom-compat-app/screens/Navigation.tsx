/**
 * react-native-screens + @react-navigation/native-stack: push, header, modal, pop, and layout.
 * A scripted run drives navigationRef through the flow and records each route stack. Layout is
 * measured with measureInWindow on each screen's content root, which screens' C++ code positions
 * (frame, header offset). Results are compared against stock once per platform.
 */
import React, {useEffect, useRef, useState} from 'react';
import {Dimensions, Text, View} from 'react-native';
import {NavigationContainer, createNavigationContainerRef} from '@react-navigation/native';
import {createNativeStackNavigator} from '@react-navigation/native-stack';
import {report} from '../report';

type Params = {Home: undefined; Detail: undefined; Transparent: undefined; Modal: undefined};
const Stack = createNativeStackNavigator<Params>();
const navRef = createNavigationContainerRef<Params>();
const probes: Record<string, React.RefObject<View | null>> = {};

function Probe({id, label}: {id: string; label: string}) {
  const ref = useRef<View>(null);
  probes[id] = ref;
  return (
    <View ref={ref} nativeID={id} style={{flex: 1, padding: 16}}>
      <Text>{label}</Text>
    </View>
  );
}

function measure(id: string): Promise<{x: number; y: number; w: number; h: number} | null> {
  return new Promise(resolve => {
    const view = probes[id]?.current;
    if (!view) {
      resolve(null);
      return;
    }
    view.measureInWindow((x, y, w, h) => resolve({x, y, w, h}));
  });
}

export default function Navigation() {
  const stacks = useRef<string[]>([]);
  const layout = useRef<Record<string, {x: number; y: number; w: number; h: number} | null>>({});
  const [result, setResult] = useState('running');
  const record = () => {
    const names = navRef.getRootState()?.routes.map(r => r.name).join('>') ?? '';
    if (stacks.current[stacks.current.length - 1] !== names) {
      stacks.current.push(names);
    }
  };
  const measureLater = (id: string) => async () => {
    layout.current[id] = await measure(id);
  };

  useEffect(() => {
    const steps: Array<[number, () => void]> = [
      [1500, () => navRef.isReady() && navRef.navigate('Detail')],
      [2500, measureLater('detail')],
      [3000, () => navRef.isReady() && navRef.navigate('Transparent')],
      [4000, measureLater('transparent')],
      [4500, () => navRef.isReady() && navRef.goBack()],
      [5000, () => navRef.isReady() && navRef.navigate('Modal')],
      [6000, () => navRef.isReady() && navRef.goBack()],
      [6500, () => navRef.isReady() && navRef.goBack()],
    ];
    const timers = steps.map(([ms, fn]) =>
      setTimeout(() => {
        fn();
        record();
      }, ms),
    );
    const done = setTimeout(() => {
      const win = Dimensions.get('window');
      const d = layout.current.detail;
      const t = layout.current.transparent;
      const checks = {
        stacks: stacks.current.join(' | '),
        pushed: stacks.current.some(s => s === 'Home>Detail'),
        transparentPushed: stacks.current.some(s => s === 'Home>Detail>Transparent'),
        modal: stacks.current.some(s => s === 'Home>Detail>Modal'),
        poppedToHome: stacks.current[stacks.current.length - 1] === 'Home',
        windowH: win.height,
        detailY: d?.y ?? -1,
        detailH: d?.h ?? -1,
        detailW: d?.w ?? -1,
        detailFillsToBottom: d ? Math.abs(d.y + d.h - win.height) < 1 : false,
        detailBelowHeader: d ? d.y > 0 : false,
        transparentY: t?.y ?? -1,
        transparentH: t?.h ?? -1,
        transparentAtTop: t ? Math.abs(t.y) < 1 : false,
      };
      report('react-native-screens+native-stack', checks);
      setResult(JSON.stringify(checks));
    }, 8000);
    return () => {
      timers.forEach(clearTimeout);
      clearTimeout(done);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <NavigationContainer ref={navRef} onStateChange={record}>
      <Stack.Navigator>
        <Stack.Screen name="Home">{() => <Probe id="home" label="home" />}</Stack.Screen>
        <Stack.Screen name="Detail">{() => <Probe id="detail" label="detail" />}</Stack.Screen>
        <Stack.Screen name="Transparent" options={{headerTransparent: true}}>
          {() => <Probe id="transparent" label="transparent" />}
        </Stack.Screen>
        <Stack.Screen name="Modal" options={{presentation: 'modal'}}>
          {() => <Probe id="modal" label="modal" />}
        </Stack.Screen>
      </Stack.Navigator>
      <View style={{position: 'absolute', bottom: 0, left: 0}}>
        <Text nativeID="compat-result" style={{fontSize: 9}}>{result}</Text>
      </View>
    </NavigationContainer>
  );
}
