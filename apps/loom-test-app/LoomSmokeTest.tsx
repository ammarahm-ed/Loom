/**
 * Runtime exercise screen for the Loom renderer.
 *
 * The build matrix proves the switches compile and link; this screen is what
 * gets run on a device afterwards to check that the renderer actually mounts,
 * commits and tears down under load. It deliberately uses only core components
 * (no extra dependencies) and prints a `[loom-smoke]` line per phase so a
 * `adb logcat` capture is enough to tell how far it got.
 *
 * @format
 */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  Animated,
  FlatList,
  NativeModules,
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';

const ROW_COUNT = 120;
const BURST_STEPS = 120;

type Switches = {
  rnUiThread: boolean;
  loom: boolean;
  loomWorker: boolean;
  profile: boolean;
};

function log(message: string) {
  console.log(`[loom-smoke] ${message}`);
}

function onOff(value: boolean) {
  return value ? 'ON' : 'OFF';
}

function Row({ index }: { index: number }) {
  const [taps, setTaps] = useState(0);
  return (
    <Pressable
      onPress={event => {
        log(
          `press row ${index} at ${Math.round(
            event.nativeEvent.pageX,
          )},${Math.round(event.nativeEvent.pageY)}`,
        );
        setTaps(value => value + 1);
      }}
      style={({ pressed }) => [styles.row, pressed && styles.rowPressed]}>
      <Text style={styles.rowText}>
        row {index} — taps {taps}
      </Text>
    </Pressable>
  );
}

export default function LoomSmokeTest() {
  const [count, setCount] = useState(0);
  const [ticks, setTicks] = useState(0);
  const [ticking, setTicking] = useState(false);
  const [rows, setRows] = useState(ROW_COUNT);
  const [busy, setBusy] = useState(false);
  const [switches, setSwitches] = useState<Switches | null>(null);
  const [switchesError, setSwitchesError] = useState<string | null>(null);
  const anim = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    log('mounted');
    return () => log('unmounted');
  }, []);

  // Which configuration this binary was compiled with, straight from the native BuildConfig, so a
  // screenshot is enough to tell a UI thread build from a stock Fabric one.
  useEffect(() => {
    const module = NativeModules.LoomBuildConfig;
    if (!module?.getSwitches) {
      log('build switches unavailable: LoomBuildConfig native module not registered');
      setSwitchesError('native module missing');
      return;
    }
    module
      .getSwitches()
      .then((value: Switches) => {
        setSwitches(value);
        log(
          `build switches rnUiThread=${value.rnUiThread} loom=${value.loom} worker=${value.loomWorker} profile=${value.profile}`,
        );
      })
      .catch((error: Error) => {
        log(`build switches error ${error.message}`);
        setSwitchesError(error.message);
      });
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

  const allOn =
    switches !== null &&
    switches.rnUiThread &&
    switches.loom &&
    switches.loomWorker;
  const allOff =
    switches !== null &&
    !switches.rnUiThread &&
    !switches.loom &&
    !switches.loomWorker;
  const bannerText = switches
    ? `UI THREAD ${onOff(switches.rnUiThread)}\nRENDERER ${onOff(
        switches.loom,
      )}\nWORKER ${onOff(switches.loomWorker)}`
    : switchesError
      ? `SWITCHES UNAVAILABLE (${switchesError})`
      : 'READING BUILD CONFIG…';
  const bannerStyle = allOn
    ? styles.bannerOn
    : allOff
      ? styles.bannerOff
      : styles.bannerMixed;
  const bannerDetail = switches
    ? `rnUiThread=${switches.rnUiThread} loom=${switches.loom} loomWorker=${switches.loomWorker} profile=${switches.profile}`
    : null;

  return (
    <View style={styles.container}>
      <View style={[styles.banner, bannerStyle]}>
        <Text style={styles.bannerText}>{bannerText}</Text>
      </View>
      {bannerDetail ? (
        <Text style={styles.bannerDetail}>{bannerDetail}</Text>
      ) : null}

      <Text style={styles.title}>Loom renderer smoke test</Text>

      <Text style={styles.metric}>count {count}</Text>
      <View style={styles.rowButtons}>
        <Pressable
          style={styles.button}
          onPress={() => {
            log('press +1');
            setCount(value => value + 1);
          }}>
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
    paddingTop: 40,
  },
  banner: {
    borderRadius: 6,
    paddingHorizontal: 12,
    paddingVertical: 10,
    marginBottom: 6,
  },
  bannerOn: {
    backgroundColor: '#1a7f37',
  },
  bannerOff: {
    backgroundColor: '#57606a',
  },
  bannerMixed: {
    backgroundColor: '#9a6700',
  },
  bannerText: {
    color: 'white',
    fontSize: 15,
    fontWeight: '700',
    letterSpacing: 0.4,
  },
  bannerDetail: {
    fontSize: 12,
    color: '#57606a',
    marginBottom: 8,
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