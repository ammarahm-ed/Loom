/**
 * A looping visual demo of Reanimated on Loom (not part of `all`): running animations (transform,
 * opacity, a layout prop) and layout animations (entering, exiting, layout transitions).
 */
import React, {useEffect, useState} from 'react';
import {Text, View} from 'react-native';
import Animated, {
  Easing,
  FadeInDown,
  FadeOutUp,
  LinearTransition,
  ZoomIn,
  ZoomOut,
  useAnimatedStyle,
  useSharedValue,
  withRepeat,
  withTiming,
} from 'react-native-reanimated';

const COLORS = ['#e76f51', '#f4a261', '#e9c46a', '#2a9d8f', '#264653', '#8ab17d'];

export default function AnimationDemo() {
  const progress = useSharedValue(0);
  useEffect(() => {
    progress.value = withRepeat(withTiming(1, {duration: 1200, easing: Easing.inOut(Easing.quad)}), -1, true);
  }, [progress]);

  const moving = useAnimatedStyle(() => ({
    transform: [{translateX: progress.value * 220}, {rotate: `${progress.value * 180}deg`}],
  }));
  const pulsing = useAnimatedStyle(() => ({opacity: 0.15 + progress.value * 0.85}));
  const growing = useAnimatedStyle(() => ({width: 60 + progress.value * 260}));

  // Layout animations: the list gains, loses and reorders items every 900 ms.
  const [items, setItems] = useState([0, 1, 2]);
  const [next, setNext] = useState(3);
  useEffect(() => {
    const id = setInterval(() => {
      setItems(current => {
        const copy = [...current];
        if (copy.length >= 5 || (copy.length > 2 && Math.random() < 0.5)) {
          copy.splice(Math.floor(Math.random() * copy.length), 1);
        } else {
          copy.splice(Math.floor(Math.random() * (copy.length + 1)), 0, -1);
        }
        return copy;
      });
      setNext(n => n + 1);
    }, 900);
    return () => clearInterval(id);
  }, []);
  // Give newly inserted placeholders a fresh key.
  const keyed = items.map(item => (item === -1 ? next : item));
  if (keyed.some((item, index) => item !== items[index])) {
    setItems(keyed);
  }

  const [showGroup, setShowGroup] = useState(true);
  useEffect(() => {
    const id = setInterval(() => setShowGroup(show => !show), 1600);
    return () => clearInterval(id);
  }, []);

  return (
    <View style={{flex: 1, padding: 20, paddingTop: 80, gap: 18, backgroundColor: '#fafafa'}}>
      <Text style={{fontSize: 18, fontWeight: '600'}}>Reanimated on Loom</Text>

      <Text>transform (translate + rotate)</Text>
      <Animated.View style={[{width: 50, height: 50, borderRadius: 8, backgroundColor: '#e76f51'}, moving]} />

      <Text>opacity</Text>
      <Animated.View style={[{width: 50, height: 50, borderRadius: 25, backgroundColor: '#2a9d8f'}, pulsing]} />

      <Text>layout prop (width)</Text>
      <Animated.View style={[{height: 24, borderRadius: 6, backgroundColor: '#264653'}, growing]} />

      <Text>entering / exiting / layout transition</Text>
      <View style={{flexDirection: 'row', gap: 10, minHeight: 56}}>
        {items.map(item => (
          <Animated.View
            key={item}
            entering={ZoomIn.duration(400)}
            exiting={ZoomOut.duration(400)}
            layout={LinearTransition.duration(400)}
            style={{
              width: 50,
              height: 50,
              borderRadius: 10,
              backgroundColor: COLORS[item % COLORS.length],
              alignItems: 'center',
              justifyContent: 'center',
            }}>
            <Text style={{color: 'white', fontWeight: '600'}}>{item}</Text>
          </Animated.View>
        ))}
      </View>

      <Text>nested exit (plain parent removed)</Text>
      <View style={{minHeight: 70}}>
        {showGroup && (
          <View style={{flexDirection: 'row', gap: 10}}>
            <Animated.View
              entering={FadeInDown.duration(500)}
              exiting={FadeOutUp.duration(500)}
              style={{width: 120, height: 50, borderRadius: 10, backgroundColor: '#f4a261'}}
            />
            <Animated.View
              entering={FadeInDown.duration(500).delay(150)}
              exiting={FadeOutUp.duration(500)}
              style={{width: 120, height: 50, borderRadius: 10, backgroundColor: '#e9c46a'}}
            />
          </View>
        )}
      </View>
    </View>
  );
}
