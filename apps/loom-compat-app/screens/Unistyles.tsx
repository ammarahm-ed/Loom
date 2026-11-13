/**
 * react-native-unistyles 3. A theme switch updates styled views from C++ by committing new props
 * to the shadow tree, without re-rendering React:
 *   switched      the themed width changed after UnistylesRuntime.setTheme
 *   noRerender    the component did not re-render for it
 *   keptOnRender  a later React re-render of the component keeps the themed value
 *   switchedBack  switching back restores the first theme's width
 */
import './unistylesConfig';
import React, {useEffect, useRef, useState} from 'react';
import {Text, View} from 'react-native';
import {StyleSheet, UnistylesRuntime} from 'react-native-unistyles';
import {near, report, wait} from '../report';

const styles = StyleSheet.create(theme => ({
  box: {width: theme.box, height: 40, backgroundColor: theme.color},
}));

export default function Unistyles() {
  const box = useRef<View>(null);
  const renders = useRef(0);
  const [tick, setTick] = useState(0);
  renders.current++;
  const width = () =>
    new Promise<number>(r => (box.current ? box.current.measure((_x, _y, w) => r(w)) : r(-1)));
  useEffect(() => {
    (async () => {
      UnistylesRuntime.setTheme('light');
      await wait(800);
      const before = await width();
      const rendersBefore = renders.current;
      UnistylesRuntime.setTheme('dark');
      await wait(800);
      const after = await width();
      const rendersAfter = renders.current;
      setTick(t => t + 1);
      await wait(800);
      const afterRender = await width();
      UnistylesRuntime.setTheme('light');
      await wait(800);
      const back = await width();
      report('react-native-unistyles', {
        before,
        after,
        afterRender,
        back,
        switched: near(before, 100) && near(after, 200),
        noRerender: rendersAfter === rendersBefore,
        keptOnRender: near(afterRender, 200),
        switchedBack: near(back, 100),
      });
    })();
  }, []);
  return (
    <View style={{padding: 16}}>
      <View ref={box} style={styles.box} />
      <Text>{`render ${tick}`}</Text>
    </View>
  );
}
