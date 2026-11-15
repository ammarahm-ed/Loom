/**
 * The example library screen (examples/loom-example-library): one native codebase that supports Fabric
 * and Loom. It measures a view, sets its opacity from native code and counts the frames the renderer
 * reported, then logs `[example] ...` lines so a run can be checked from the log.
 *
 * @format
 */
import {useEffect, useRef, useState} from 'react';
import {findNodeHandle, NativeModules, Text, View} from 'react-native';
import {
  frames,
  measure,
  renderer,
  setOpacity,
} from 'react-native-loom-example-library';

export default function LoomExample() {
  const box = useRef<View>(null);
  const [report, setReport] = useState('running');

  useEffect(() => {
    const timer = setTimeout(() => {
      const tag = findNodeHandle(box.current);
      if (tag == null) {
        setReport('no node');
        return;
      }
      const frame = measure(tag);
      const ok = setOpacity(tag, 0.5);
      setTimeout(() => {
        const text = JSON.stringify({
          renderer: renderer(),
          frame,
          setOpacity: ok,
          frames: frames(),
        });
        console.log('[example] ' + text);
        // iOS Release builds print no console.log; BenchProbe.mm forwards this to the device console (absent on Android).
        NativeModules.LoomBuildConfig?.log?.('[example] ' + text);
        setReport(text);
      }, 300);
    }, 500);
    return () => clearTimeout(timer);
  }, []);

  return (
    <View style={{flex: 1, padding: 24, backgroundColor: '#fff'}}>
      <Text testID="example-title" style={{fontSize: 18, marginBottom: 12}}>
        Loom example library
      </Text>
      <View
        ref={box}
        collapsable={false}
        testID="example-box"
        style={{
          width: 160,
          height: 80,
          marginLeft: 20,
          marginTop: 10,
          backgroundColor: '#2563eb',
        }}
      />
      <Text testID="example-report" style={{marginTop: 16, fontSize: 12}}>
        {report}
      </Text>
    </View>
  );
}
