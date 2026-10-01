/**
 * Runtime exercise screen for the Resha direct renderer.
 *
 * The build matrix proves the switches compile and link; this screen is what
 * gets run on a device afterwards to check that the renderer actually mounts,
 * commits and tears down under load. It deliberately uses only core components
 * (no extra dependencies) and prints a `[resha-smoke]` line per phase so a
 * `adb logcat` capture is enough to tell how far it got.
 *
 * @format
 */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  Animated,
  FlatList,
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';

const ROW_COUNT = 120;
const BURST_STEPS = 120;

function log(message: string) {
  console.log(`[resha-smoke] ${message}`);
}

function Row({ index }: { index: number }) {
  const [taps, setTaps] = useState(0);
  return (
    <Pressable
      onPress={() => setTaps(value => value + 1)}
      style={({ pressed }) => [styles.row, pressed && styles.rowPressed]}>
      <Text style={styles.rowText}>
        row {index} — taps {taps}
      </Text>
    </Pressable>
  );
}

export default function ReshaSmokeTest() {
  const [count, setCount] = useState(0);
  const [ticks, setTicks] = useState(0);
  const [ticking, setTicking] = useState(false);
  const [rows, setRows] = useState(ROW_COUNT);
  const [busy, setBusy] = useState(false);
  const anim = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    log('mounted');
    return () => log('unmounted');
  }, []);

  // 60 Hz state updates: the renderer has to keep up with a commit per frame.
  useEffect(() => {
    if (!ticking) {
      return;
    }
    log('ticker started');
    const id = setInterval(() => setTicks(value => value + 1), 16);
    return () => {
      clearInterval(id);
      log('ticker stopped');
    };
  }, [ticking]);

  // A looping native-driver animation, to check it is not starved by commits.
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(anim, {
          toValue: 1,
          duration: 800,
          useNativeDriver: true,
        }),
        Animated.timing(anim, {
          toValue: 0,
          duration: 800,
          useNativeDriver: true,
        }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [anim]);

  // Many commits back to back, without yielding to the event loop between them.
  const burst = useCallback(() => {
    setBusy(true);
    log(`burst start (${BURST_STEPS} steps)`);
    let step = 0;
    const pump = () => {
      step += 1;
      setCount(step);
      if (step < BURST_STEPS) {
        requestAnimationFrame(pump);
      } else {
        setBusy(false);
        log('burst done');
      }
    };
    requestAnimationFrame(pump);
  }, []);

  const remount = useCallback(() => {
    log('remount');
    setRows(0);
    requestAnimationFrame(() => setRows(ROW_COUNT));
  }, []);

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Resha direct renderer smoke test</Text>

      <Text style={styles.metric}>count {count}</Text>
      <View style={styles.rowButtons}>
        <Pressable
          style={styles.button}
          onPress={() => setCount(value => value + 1)}>
          <Text style={styles.buttonText}>+1</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={burst} disabled={busy}>
          <Text style={styles.buttonText}>{busy ? 'bursting…' : 'burst'}</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={remount}>
          <Text style={styles.buttonText}>remount list</Text>
        </Pressable>
      </View>

      <Text style={styles.metric}>ticks {ticks}</Text>
      <View style={styles.rowButtons}>
        <Pressable style={styles.button} onPress={() => setTicking(v => !v)}>
          <Text style={styles.buttonText}>{ticking ? 'stop 60 Hz' : 'start 60 Hz'}</Text>
        </Pressable>
      </View>

      <Animated.View
        style={[
          styles.animated,
          {
            transform: [
              {
                translateX: anim.interpolate({
                  inputRange: [0, 1],
                  outputRange: [0, 220],
                }),
              },
            ],
          },
        ]}
      />

      <Text style={styles.metric}>list rows {rows}</Text>
      <View style={styles.listBox}>
        <FlatList
          data={Array.from({ length: rows }, (_, index) => index)}
          keyExtractor={index => String(index)}
          renderItem={({ item }) => <Row index={item} />}
        />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    padding: 16,
    paddingTop: 60,
  },
  title: {
    fontSize: 18,
    fontWeight: '600',
    marginBottom: 12,
  },
  metric: {
    fontSize: 15,
    marginTop: 8,
  },
  rowButtons: {
    flexDirection: 'row',
    gap: 8,
    marginTop: 8,
  },
  button: {
    backgroundColor: '#1f6feb',
    borderRadius: 6,
    paddingHorizontal: 14,
    paddingVertical: 8,
  },
  buttonText: {
    color: 'white',
    fontWeight: '600',
  },
  animated: {
    width: 60,
    height: 24,
    marginTop: 16,
    borderRadius: 4,
    backgroundColor: '#d29922',
  },
  listBox: {
    flex: 1,
    marginTop: 8,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: '#8883',
  },
  row: {
    paddingHorizontal: 12,
    paddingVertical: 10,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: '#8883',
  },
  rowPressed: {
    backgroundColor: '#1f6feb33',
  },
  rowText: {
    fontSize: 14,
  },
});