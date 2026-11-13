/**
 * Libraries built on screens / reanimated / gesture-handler together: bottom tabs, bottom sheet,
 * keyboard-controller. Driven programmatically; touch-only behaviour is covered by Maestro flows.
 */
import React, {useEffect, useRef} from 'react';
import {Dimensions, Text, TextInput, View} from 'react-native';
import {NavigationContainer, createNavigationContainerRef} from '@react-navigation/native';
import {createBottomTabNavigator} from '@react-navigation/bottom-tabs';
import BottomSheet, {BottomSheetView} from '@gorhom/bottom-sheet';
import {GestureHandlerRootView} from 'react-native-gesture-handler';
import {KeyboardAvoidingView, KeyboardProvider, useKeyboardHandler} from 'react-native-keyboard-controller';
import {runOnJS} from 'react-native-worklets';
import {near, report, wait} from '../report';

type Box = {x: number; y: number; width: number; height: number};

const Tabs = createBottomTabNavigator();
const tabsRef = createNavigationContainerRef<any>();

export function BottomTabsScreen() {
  const boxes = useRef<Record<string, Box>>({});
  const views = useRef<Record<string, View | null>>({});
  const measureIn = (id: string) =>
    new Promise<Box | null>(r =>
      views.current[id] ? views.current[id]!.measureInWindow((x, y, width, height) => r({x, y, width, height})) : r(null),
    );
  useEffect(() => {
    (async () => {
      await wait(1200);
      const first = await measureIn('one');
      tabsRef.navigate('Two');
      await wait(1000);
      const second = await measureIn('two');
      const win = Dimensions.get('window');
      report('@react-navigation/bottom-tabs', {
        oneY: first?.y ?? -1,
        oneH: first?.height ?? -1,
        twoH: second?.height ?? -1,
        route: tabsRef.getCurrentRoute()?.name ?? '',
        switched: tabsRef.getCurrentRoute()?.name === 'Two',
        // Content ends where the tab bar starts: above the bottom of the window, not under it.
        contentAboveTabBar: !!first && first.y + first.height < win.height - 40,
        sameHeight: !!first && !!second && near(first.height, second.height),
      });
    })();
  }, []);
  const Page = (id: string) => () => (
    <View ref={v => (views.current[id] = v)} style={{flex: 1, backgroundColor: '#eef'}}>
      <Text>{id}</Text>
    </View>
  );
  return (
    <NavigationContainer ref={tabsRef}>
      <Tabs.Navigator>
        <Tabs.Screen name="One" component={Page('one')} />
        <Tabs.Screen name="Two" component={Page('two')} />
      </Tabs.Navigator>
    </NavigationContainer>
  );
}

export function BottomSheetScreen() {
  const sheet = useRef<BottomSheet>(null);
  const indexes = useRef<number[]>([]);
  const content = useRef<View>(null);
  useEffect(() => {
    (async () => {
      await wait(1200);
      sheet.current?.snapToIndex(1);
      await wait(1500);
      const box = await new Promise<Box | null>(r =>
        content.current ? content.current.measureInWindow((x, y, width, height) => r({x, y, width, height})) : r(null),
      );
      const win = Dimensions.get('window');
      report('@gorhom/bottom-sheet', {
        indexes: indexes.current.join(','),
        snapped: indexes.current.includes(1),
        contentY: Math.round(box?.y ?? -1),
        // Snap point 50%: the sheet's content sits in the lower half of the window.
        inLowerHalf: !!box && box.y > win.height / 2 - 60 && box.y < win.height,
      });
    })();
  }, []);
  return (
    <GestureHandlerRootView style={{flex: 1}}>
      <View style={{flex: 1, backgroundColor: '#ddd'}}>
        <BottomSheet ref={sheet} index={0} snapPoints={['25%', '50%']} onChange={i => indexes.current.push(i)}>
          <BottomSheetView>
            <View ref={content} style={{height: 40}}>
              <Text>sheet</Text>
            </View>
          </BottomSheetView>
        </BottomSheet>
      </View>
    </GestureHandlerRootView>
  );
}

function KeyboardInner() {
  const input = useRef<TextInput>(null);
  const heights = useRef<number[]>([]);
  const footer = useRef<Record<string, number>>({});
  const record = (h: number) => heights.current.push(h);
  useKeyboardHandler(
    {
      onEnd: e => {
        'worklet';
        runOnJS(record)(e.height);
      },
    },
    [],
  );
  useEffect(() => {
    (async () => {
      await wait(1000);
      const before = footer.current.y;
      input.current?.focus();
      await wait(1500);
      const during = footer.current.y;
      input.current?.blur();
      await wait(1000);
      report('react-native-keyboard-controller', {
        keyboardHeights: heights.current.map(Math.round).join(','),
        keyboardShown: heights.current.some(h => h > 100),
        footerBefore: before ?? -1,
        footerDuring: during ?? -1,
        // KeyboardAvoidingView (padding) pushes the footer up while the keyboard is open.
        avoided: before !== undefined && during !== undefined && during < before - 100,
        footerAfter: footer.current.y ?? -1,
        restored: near(footer.current.y, before ?? -1, 2),
      });
    })();
  }, []);
  return (
    <KeyboardAvoidingView behavior="padding" style={{flex: 1}}>
      <View style={{flex: 1, padding: 16}}>
        <TextInput ref={input} placeholder="type" style={{borderWidth: 1, height: 40}} />
      </View>
      <View
        onLayout={e => (footer.current.y = e.nativeEvent.layout.y)}
        style={{height: 50, backgroundColor: '#9c6'}}
      />
    </KeyboardAvoidingView>
  );
}
export function KeyboardScreen() {
  return (
    <KeyboardProvider>
      <KeyboardInner />
    </KeyboardProvider>
  );
}
