/**
 * react-native-reanimated layout animations. Each animation reports through `withCallback` when it
 * ends; a check passes when it finished (not cancelled) and took about its duration, which a skipped
 * or unstarted animation cannot do:
 *   entering        `entering` on a mounted view
 *   exiting         `exiting` on an unmounted view
 *   nestedExiting   `exiting` on a child of an unmounted plain view (the parent stays until it ends)
 *   layout          `layout` transition when the view moves
 *   siblingDuringExit  a sibling inserted while a view exits mounts (index past the held view)
 */
import React, {useEffect, useRef, useState} from 'react';
import {View} from 'react-native';
import Animated, {FadeIn, FadeOut, LinearTransition} from 'react-native-reanimated';
import {runOnJS} from 'react-native-worklets';
import {report, wait} from '../report';

const DURATION = 800;

type Ended = {finished: boolean; ms: number};

export default function LayoutAnimations() {
  const started = useRef<Record<string, number>>({});
  const ended = useRef<Record<string, Ended>>({});
  const siblingLayout = useRef(false);
  const [entering, setEntering] = useState(false);
  const [exiting, setExiting] = useState(true);
  const [nested, setNested] = useState(true);
  const [spacer, setSpacer] = useState(false);
  const [sibling, setSibling] = useState(false);

  const end = (name: string, finished: boolean) => {
    ended.current[name] = {finished, ms: Date.now() - (started.current[name] ?? Date.now())};
  };
  const callback = (name: string) => (finished: boolean) => {
    'worklet';
    runOnJS(end)(name, finished);
  };

  useEffect(() => {
    (async () => {
      await wait(800);
      started.current.entering = Date.now();
      setEntering(true);
      await wait(DURATION + 600);
      started.current.exiting = Date.now();
      setExiting(false);
      await wait(100);
      setSibling(true);
      await wait(DURATION + 600);
      started.current.nestedExiting = Date.now();
      setNested(false);
      await wait(DURATION + 600);
      started.current.layout = Date.now();
      setSpacer(true);
      await wait(DURATION + 800);
      const r = ended.current;
      const timed = (name: string) =>
        !!r[name]?.finished && r[name].ms > DURATION * 0.6 && r[name].ms < DURATION * 3;
      report('react-native-reanimated layout animations', {
        enteringMs: r.entering?.ms ?? -1,
        exitingMs: r.exiting?.ms ?? -1,
        nestedExitingMs: r.nestedExiting?.ms ?? -1,
        layoutMs: r.layout?.ms ?? -1,
        entering: timed('entering'),
        exiting: timed('exiting'),
        nestedExiting: timed('nestedExiting'),
        layout: timed('layout'),
        siblingDuringExit: siblingLayout.current,
      });
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <View style={{flex: 1, padding: 16}}>
      {spacer && <View style={{height: 60}} />}
      <Animated.View
        layout={LinearTransition.duration(DURATION).withCallback(callback('layout'))}
        style={{height: 30, backgroundColor: '#999'}}
      />
      {entering && (
        <Animated.View
          entering={FadeIn.duration(DURATION).withCallback(callback('entering'))}
          style={{height: 30, backgroundColor: '#69c'}}
        />
      )}
      <View style={{height: 40}}>
        {exiting && (
          <Animated.View
            exiting={FadeOut.duration(DURATION).withCallback(callback('exiting'))}
            style={{height: 30, backgroundColor: '#c69'}}
          />
        )}
        {sibling && (
          <View onLayout={() => (siblingLayout.current = true)} style={{height: 10, backgroundColor: '#6c9'}} />
        )}
      </View>
      {nested && (
        <View style={{padding: 4, backgroundColor: '#eee'}}>
          <View style={{height: 10, backgroundColor: '#ccc'}} />
          <Animated.View
            exiting={FadeOut.duration(DURATION).withCallback(callback('nestedExiting'))}
            style={{height: 30, backgroundColor: '#9c6'}}
          />
        </View>
      )}
    </View>
  );
}
