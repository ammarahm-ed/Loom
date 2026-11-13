/**
 * react-native-reanimated shared element transitions (ENABLE_SHARED_ELEMENT_TRANSITIONS) across a
 * native-stack push and pop. Each screen's content is a SharedTransitionBoundary; the Home box sits
 * in a ScrollView scrolled by SCROLL, under the native header. A probing transition records the
 * source and target frames Reanimated animates between, and they are compared with measureInWindow
 * of the two views:
 *   push            the push transition ran to its end (finished, about its duration)
 *   pushFrom/To     it started on the Home box and ended on the Detail box
 *   pop             Android: the pop transition ran from the Detail box back to the Home box.
 *                   iOS drives a pop with the native transition's progress (no Reanimated
 *                   animation runs), so there it only checks the Home box is where it was.
 */
import React, {useEffect, useRef} from 'react';
import {Platform, ScrollView, Text, View} from 'react-native';
import {NavigationContainer, createNavigationContainerRef} from '@react-navigation/native';
import {createNativeStackNavigator} from '@react-navigation/native-stack';
import Animated, {ReduceMotion, SharedTransition, SharedTransitionBoundary} from 'react-native-reanimated';
import {runOnJS} from 'react-native-worklets';
import {report, wait} from '../report';

const DURATION = 700;
const SCROLL = 60;

type Params = {Home: undefined; Detail: undefined};
const Stack = createNativeStackNavigator<Params>();
const navRef = createNavigationContainerRef<Params>();

type Values = Record<string, number | string>;
type Frame = {x: number; y: number; w: number; h: number};
type Transition = {source: Values; target: Values; started: number; ended?: number; finished?: boolean};

const transitions: Transition[] = [];
function started(source: Values, target: Values) {
  transitions.push({source, target, started: Date.now()});
}
// A transition that is restarted ends (cancelled) before the one that replaces it.
function ended(finished: boolean) {
  const first = transitions.find(t => t.ended === undefined);
  if (first) {
    first.ended = Date.now();
    first.finished = finished;
  }
}

// Reanimated's own SharedTransition, with the values it is started with recorded.
const base = SharedTransition.duration(DURATION).reduceMotion(ReduceMotion.Never).withCallback((finished: boolean) => {
  'worklet';
  runOnJS(ended)(finished);
});
const probe = {
  build: () => {
    const animation = base.build();
    return (values: {source: Values; target: Values}) => {
      'worklet';
      runOnJS(started)(values.source, values.target);
      return animation(values as never);
    };
  },
};

const refs: Record<string, React.RefObject<View | null>> = {};
function Box({id, size, color}: {id: string; size: number; color: string}) {
  const ref = useRef<View>(null);
  refs[id] = ref;
  return (
    <Animated.View
      ref={ref as never}
      sharedTransitionTag="box"
      sharedTransitionStyle={probe as never}
      style={{width: size, height: size, borderRadius: 12, backgroundColor: color}}
    />
  );
}

function measure(id: string): Promise<Frame | null> {
  return new Promise(resolve => {
    const view = refs[id]?.current;
    if (!view) {
      resolve(null);
      return;
    }
    view.measureInWindow((x, y, w, h) => resolve({x, y, w, h}));
  });
}

function Home() {
  const scroll = useRef<ScrollView>(null);
  useEffect(() => {
    setTimeout(() => scroll.current?.scrollTo({y: SCROLL, animated: false}), 300);
  }, []);
  return (
    <SharedTransitionBoundary isActive>
      <ScrollView ref={scroll} style={{flex: 1}}>
        <View style={{height: 200, padding: 16}}>
          <Text>Shared transitions: home</Text>
        </View>
        <View style={{paddingLeft: 40}}>
          <Box id="home" size={80} color="#2a9d8f" />
        </View>
        <View style={{height: 1200}} />
      </ScrollView>
    </SharedTransitionBoundary>
  );
}

function Detail() {
  return (
    <SharedTransitionBoundary isActive>
      <View style={{flex: 1, paddingTop: 120, alignItems: 'flex-end', paddingRight: 24}}>
        <Box id="detail" size={200} color="#e76f51" />
      </View>
    </SharedTransitionBoundary>
  );
}

// The frame a transition reports for one side (only the keys that differ are given).
function matches(values: Values | undefined, frame: Frame | null, tol = 1.5) {
  if (!values || !frame) {
    return false;
  }
  const pairs: Array<[string, number]> = [
    ['originX', frame.x],
    ['originY', frame.y],
    ['width', frame.w],
    ['height', frame.h],
  ];
  return pairs.every(([key, value]) => values[key] === undefined || Math.abs(Number(values[key]) - value) <= tol);
}

export default function SharedTransitions() {
  useEffect(() => {
    (async () => {
      await wait(1500);
      const home = await measure('home');
      navRef.isReady() && navRef.navigate('Detail');
      await wait(DURATION + 900);
      const detail = await measure('detail');
      const popAt = Date.now();
      navRef.isReady() && navRef.goBack();
      await wait(DURATION + 1200);
      const homeAgain = await measure('home');
      // A transition retargeted on its way (its screen laid out again) restarts: the last one counts.
      const pushes = transitions.filter(t => t.started < popAt);
      const push = pushes[pushes.length - 1];
      const pop = transitions.filter(t => t.started >= popAt).pop();
      const fmt = (v?: Values) => (v ? JSON.stringify(v).slice(0, 120) : 'none');
      const checks: Record<string, boolean | number | string> = {
        home: JSON.stringify(home),
        detail: JSON.stringify(detail),
        pushSource: fmt(push?.source),
        pushTarget: fmt(push?.target),
        transitions: transitions.length,
        popTransitions: transitions.filter(t => t.started >= popAt).length,
        push: !!push?.finished && push.ended! - pushes[0].started > DURATION * 0.6,
        pushFrom: matches(pushes[0]?.source, home),
        pushTo: matches(push?.target, detail),
        homeKept: !!home && !!homeAgain && Math.abs(home.y - homeAgain.y) < 1.5,
      };
      if (Platform.OS === 'android') {
        checks.popSource = fmt(pop?.source);
        checks.popTarget = fmt(pop?.target);
        checks.pop = !!pop?.finished && matches(pop.source, detail) && matches(pop.target, home);
      }
      report('react-native-reanimated shared transitions', checks);
    })();
  }, []);

  return (
    <NavigationContainer ref={navRef}>
      <Stack.Navigator>
        <Stack.Screen name="Home" component={Home} />
        <Stack.Screen name="Detail" component={Detail} />
      </Stack.Navigator>
    </NavigationContainer>
  );
}
