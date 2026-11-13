/**
 * react-native-reanimated + react-native-worklets. Each check exercises a different path from the
 * UI runtime into the renderer:
 *   width       withTiming on a layout prop (width): Reanimated commits it to the shadow tree, so
 *               onLayout must report the final width
 *   measureW    measure() from a worklet reads the same layout on the UI runtime
 *   opacityRead a non-layout prop animated on the UI thread, read back from the shared value
 *   scrollY     scrollTo() from a worklet, read back through useAnimatedScrollHandler
 *   entered     an entering layout animation mounts the view (onLayout fires)
 *   exited      an exiting layout animation completes: the sibling below moves up
 */
import React, {useEffect, useRef, useState} from 'react';
import {Text, View} from 'react-native';
import Animated, {
  FadeIn,
  FadeOut,
  measure,
  scrollTo,
  useAnimatedRef,
  useAnimatedScrollHandler,
  useAnimatedStyle,
  useSharedValue,
  withTiming,
  ReduceMotion,
} from 'react-native-reanimated';
import {runOnJS, runOnUI} from 'react-native-worklets';
import {near, report, wait} from '../report';

export default function Reanimated() {
  const width = useSharedValue(100);
  const opacity = useSharedValue(1);
  const scrollY = useSharedValue(0);
  const boxRef = useAnimatedRef<Animated.View>();
  const scrollRef = useAnimatedRef<Animated.ScrollView>();
  const layouts = useRef<Record<string, {y: number; width: number}>>({});
  const measured = useRef(-1);
  const content = useRef<View>(null);
  const contentY = () =>
    new Promise<number>(r => (content.current ? content.current.measureInWindow((_x, y) => r(y)) : r(-1)));
  const [showExiting, setShowExiting] = useState(true);
  const [showEntering, setShowEntering] = useState(false);

  const boxStyle = useAnimatedStyle(() => ({width: width.value, opacity: opacity.value}));
  const onScroll = useAnimatedScrollHandler(e => {
    scrollY.value = e.contentOffset.y;
  });
  const on = (key: string) => (e: any) => {
    layouts.current[key] = e.nativeEvent.layout;
  };

  useEffect(() => {
    const setMeasured = (w: number) => {
      measured.current = w;
    };
    (async () => {
      await wait(800);
      const contentBefore = await contentY();
      width.value = withTiming(220, {duration: 300, reduceMotion: ReduceMotion.Never});
      opacity.value = withTiming(0.5, {duration: 300, reduceMotion: ReduceMotion.Never});
      setShowEntering(true);
      runOnUI(() => {
        scrollTo(scrollRef, 0, 300, false);
      })();
      await wait(400);
      const siblingBefore = layouts.current.sibling?.y ?? -1;
      setShowExiting(false);
      await wait(1200);
      runOnUI(() => {
        const m = measure(boxRef);
        runOnJS(setMeasured)(m ? m.width : -2);
      })();
      await wait(300);
      const contentAfter = await contentY();
      const l = layouts.current;
      report('react-native-reanimated', {
        width: l.box?.width ?? -1,
        widthAnimated: near(l.box?.width, 220),
        measureW: measured.current,
        measureMatches: near(measured.current, 220),
        opacityRead: opacity.value,
        scrollY: Math.round(scrollY.value),
        scrolled: near(scrollY.value, 300, 2),
        // The view itself moved (scrollTo ran), independent of the scroll handler.
        contentMoved: Math.round(contentBefore - contentAfter),
        entered: l.entering !== undefined,
        siblingBefore,
        siblingAfter: l.sibling?.y ?? -1,
        exited: (l.sibling?.y ?? 1e9) < siblingBefore,
      });
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <View style={{flex: 1, padding: 16}}>
      <Animated.View
        ref={boxRef}
        onLayout={on('box')}
        style={[{height: 40, backgroundColor: '#69c'}, boxStyle]}
      />
      {showEntering && (
        <Animated.View entering={FadeIn.reduceMotion(ReduceMotion.Never)} onLayout={on('entering')} style={{height: 30, backgroundColor: '#9c6'}} />
      )}
      {showExiting && (
        <Animated.View exiting={FadeOut.duration(200).reduceMotion(ReduceMotion.Never)} style={{height: 50, backgroundColor: '#c69'}} />
      )}
      <View onLayout={on('sibling')} style={{height: 20, backgroundColor: '#ccc'}} />
      <Animated.ScrollView ref={scrollRef} onScroll={onScroll} scrollEventThrottle={16} style={{height: 200}}>
        <View ref={content} style={{height: 2000}}>
          <Text>scroll</Text>
        </View>
      </Animated.ScrollView>
    </View>
  );
}
