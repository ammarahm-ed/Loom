/**
 * Libraries whose components are measured or laid out by their own native/C++ code. Each screen
 * waits for its components to report, then checks sizes and callbacks against what the library's
 * docs promise.
 */
import React, {useEffect, useRef, useState} from 'react';
import {Dimensions, Platform, Text, View} from 'react-native';
import Svg, {Circle, Rect, Text as SvgText} from 'react-native-svg';
import PagerView from 'react-native-pager-view';
import {FlashList} from '@shopify/flash-list';
import {LegendList} from '@legendapp/list/react-native';
import {Canvas, Circle as SkCircle, useCanvasRef} from '@shopify/react-native-skia';
import LottieView from 'lottie-react-native';
import {WebView} from 'react-native-webview';
import Slider from '@react-native-community/slider';
import DateTimePicker from '@react-native-community/datetimepicker';
import LinearGradient from 'react-native-linear-gradient';
import Video from 'react-native-video';
import {Camera, useCameraDevice, useCameraPermission} from 'react-native-vision-camera';
import {near, report, wait} from '../report';

type Box = {x: number; y: number; width: number; height: number};
function useBoxes() {
  const boxes = useRef<Record<string, Box>>({});
  const on = (key: string) => (e: any) => {
    boxes.current[key] = e.nativeEvent.layout;
  };
  return [boxes, on] as const;
}

export function SvgScreen() {
  const [b, on] = useBoxes();
  useEffect(() => {
    wait(1500).then(() =>
      report('react-native-svg', {
        fixedW: b.current.fixed?.width ?? -1,
        fixedH: b.current.fixed?.height ?? -1,
        fixedSize: near(b.current.fixed?.width, 200) && near(b.current.fixed?.height, 100),
        // An Svg with only height="100%" takes a square box from its viewBox (stock: 60 x 60 in a
        // 60-high row), and the next sibling starts right below the fixed 100-high Svg.
        flexW: b.current.flex?.width ?? -1,
        flexSquare: near(b.current.flex?.width, b.current.row?.height ?? -1),
        nextY: b.current.next?.y ?? -1,
        nextBelow: near(b.current.next?.y, 16 + 100),
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View style={{padding: 16}}>
      <Svg onLayout={on('fixed')} width={200} height={100} viewBox="0 0 100 50">
        <Rect x={0} y={0} width={100} height={50} fill="#cde" />
        <Circle cx={25} cy={25} r={20} fill="#69c" />
        <SvgText x={55} y={30} fontSize={12}>
          svg
        </SvgText>
      </Svg>
      <View onLayout={on('next')} style={{height: 10, backgroundColor: '#ccc'}} />
      <View onLayout={on('row')} style={{flexDirection: 'row', height: 60}}>
        <Svg onLayout={on('flex')} style={{flex: 1}} height="100%" viewBox="0 0 10 10">
          <Rect width={10} height={10} fill="#e96" />
        </Svg>
      </View>
    </View>
  );
}

export function PagerScreen() {
  const ref = useRef<PagerView>(null);
  const [b, on] = useBoxes();
  const selected = useRef<number[]>([]);
  useEffect(() => {
    (async () => {
      await wait(800);
      ref.current?.setPage(2);
      await wait(1200);
      report('react-native-pager-view', {
        pageW: b.current.page2?.width ?? b.current.page0?.width ?? -1,
        pageFillsWidth: near(b.current.page0?.width, Dimensions.get('window').width),
        selected: selected.current.join(','),
        reachedPage2: selected.current.includes(2),
      });
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <PagerView
      ref={ref}
      style={{flex: 1}}
      initialPage={0}
      onPageSelected={e => selected.current.push(e.nativeEvent.position)}>
      {[0, 1, 2].map(i => (
        <View key={i} onLayout={on(`page${i}`)} style={{flex: 1, backgroundColor: ['#cde', '#dec', '#edc'][i]}}>
          <Text>page {i}</Text>
        </View>
      ))}
    </PagerView>
  );
}

const DATA = Array.from({length: 1000}, (_, i) => ({id: String(i)}));

function ListScreen({lib, List}: {lib: string; List: any}) {
  const ref = useRef<any>(null);
  const rendered = useRef(new Set<number>());
  const heights = useRef<Record<number, number>>({});
  const viewable = useRef<number[]>([]);
  useEffect(() => {
    (async () => {
      await wait(1000);
      const initial = rendered.current.size;
      ref.current?.scrollToIndex({index: 500, animated: false});
      await wait(1500);
      report(lib, {
        initialRendered: initial,
        renderedSome: initial > 0 && initial < 200,
        row0H: heights.current[0] ?? -1,
        rowHeight: near(heights.current[0], 44),
        reached500: rendered.current.has(500),
        viewable: viewable.current.slice(0, 3).join(','),
        viewable500: viewable.current.some(i => i >= 495 && i <= 505),
      });
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <List
      ref={ref}
      data={DATA}
      style={{flex: 1}}
      estimatedItemSize={44}
      keyExtractor={(item: any) => item.id}
      onViewableItemsChanged={({viewableItems}: any) => {
        viewable.current = viewableItems.map((v: any) => v.index);
      }}
      renderItem={({item, index}: any) => {
        rendered.current.add(index);
        return (
          <View
            onLayout={e => (heights.current[index] = e.nativeEvent.layout.height)}
            style={{height: 44, borderBottomWidth: 1, borderColor: '#ddd', justifyContent: 'center'}}>
            <Text>row {item.id}</Text>
          </View>
        );
      }}
    />
  );
}
export const FlashListScreen = () => <ListScreen lib="@shopify/flash-list" List={FlashList} />;
export const LegendListScreen = () => <ListScreen lib="@legendapp/list" List={LegendList} />;

export function SkiaScreen() {
  const [b, on] = useBoxes();
  const canvas = useCanvasRef();
  useEffect(() => {
    wait(1500).then(() => {
      const image = canvas.current?.makeImageSnapshot();
      const pixels = image?.readPixels(150, 150, {width: 1, height: 1, colorType: 4, alphaType: 1}) as any;
      report('@shopify/react-native-skia', {
        canvasW: b.current.canvas?.width ?? -1,
        canvasSized: near(b.current.canvas?.width, 200) && near(b.current.canvas?.height, 100),
        snapshotW: image?.width() ?? -1,
        drew: (image?.width() ?? 0) > 0,
        centerRed: pixels ? pixels[0] : -1,
        circleDrawn: pixels ? pixels[0] > 200 && pixels[1] < 50 : false,
      });
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View style={{padding: 16}}>
      {/* Skia does not support onLayout on Canvas on native; the wrapper reports its size. */}
      <View onLayout={on('canvas')} style={{width: 200, height: 100}}>
        <Canvas ref={canvas} style={{flex: 1}}>
          <SkCircle cx={50} cy={50} r={40} color="red" />
        </Canvas>
      </View>
    </View>
  );
}

// A one-layer, one-second lottie animation: a square whose opacity goes 0 -> 100.
const LOTTIE = {
  v: '5.7.4', fr: 30, ip: 0, op: 30, w: 100, h: 100, nm: 'square', ddd: 0, assets: [],
  layers: [
    {
      ddd: 0, ind: 1, ty: 1, nm: 'solid', sr: 1, ip: 0, op: 30, st: 0, bm: 0, sc: '#3366cc', sw: 100, sh: 100,
      ks: {
        o: {a: 1, k: [{t: 0, s: [0], e: [100], i: {x: [0.5], y: [1]}, o: {x: [0.5], y: [0]}}, {t: 30, s: [100]}]},
        r: {a: 0, k: 0}, p: {a: 0, k: [50, 50, 0]}, a: {a: 0, k: [50, 50, 0]}, s: {a: 0, k: [100, 100, 100]},
      },
    },
  ],
};
export function LottieScreen() {
  const [b, on] = useBoxes();
  const finished = useRef(false);
  const loaded = useRef(false);
  useEffect(() => {
    wait(2500).then(() =>
      report('lottie-react-native', {
        w: b.current.lottie?.width ?? -1,
        sized: near(b.current.lottie?.width, 120),
        finished: finished.current,
        // Informational: Lottie's iOS view attaches its loaded callback after an inline JSON
        // animation has already loaded, so iOS never reports it for this source.
        loadedInfo: String(loaded.current),
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View style={{padding: 16}}>
      <LottieView
        source={LOTTIE as any}
        autoPlay
        loop={false}
        onLayout={on('lottie')}
        onAnimationLoaded={() => (loaded.current = true)}
        onAnimationFinish={() => (finished.current = true)}
        style={{width: 120, height: 120}}
      />
    </View>
  );
}

export function WebViewScreen() {
  const [b, on] = useBoxes();
  const msg = useRef('');
  const loaded = useRef(false);
  useEffect(() => {
    wait(4000).then(() =>
      report('react-native-webview', {
        loaded: loaded.current,
        message: msg.current,
        bodyWidthMatches: near(Number(msg.current), b.current.web?.width ?? -1, 2),
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View onLayout={on('web')} style={{height: 200, margin: 16}}>
      <WebView
        originWhitelist={['*']}
        source={{
          html: '<html><head><meta name="viewport" content="width=device-width"></head><body style="margin:0">hi<script>setTimeout(()=>window.ReactNativeWebView.postMessage(String(document.body.offsetWidth)),300)</script></body></html>',
        }}
        onLoadEnd={() => (loaded.current = true)}
        onMessage={e => (msg.current = e.nativeEvent.data)}
      />
    </View>
  );
}

export function SliderScreen() {
  const [b, on] = useBoxes();
  useEffect(() => {
    wait(1200).then(() =>
      report('@react-native-community/slider', {
        h: b.current.slider?.height ?? -1,
        hasHeight: (b.current.slider?.height ?? 0) > 10,
        w: b.current.slider?.width ?? -1,
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View style={{padding: 16}}>
      <Slider onLayout={on('slider')} value={0.5} />
    </View>
  );
}

export function DateTimeScreen() {
  const [b, on] = useBoxes();
  useEffect(() => {
    wait(1200).then(() =>
      report('@react-native-community/datetimepicker', {
        h: b.current.picker?.height ?? -1,
        // iOS sizes the picker from its native intrinsic size; Android has only a dialog.
        measured: Platform.OS === 'android' || (b.current.picker?.height ?? 0) > 20,
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View style={{padding: 16}}>
      {Platform.OS === 'ios' && (
        <View onLayout={on('picker')} style={{alignSelf: 'flex-start'}}>
          <DateTimePicker value={new Date(2026, 0, 1)} mode="date" display="compact" />
        </View>
      )}
    </View>
  );
}

export function GradientScreen() {
  const [b, on] = useBoxes();
  useEffect(() => {
    wait(800).then(() =>
      report('react-native-linear-gradient', {
        w: b.current.g?.width ?? -1,
        sized: near(b.current.g?.width, 150) && near(b.current.g?.height, 80),
        childY: b.current.child?.y ?? -1,
        childPadded: near(b.current.child?.y, 10),
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <View style={{padding: 16}}>
      <LinearGradient onLayout={on('g')} colors={['#f00', '#00f']} style={{width: 150, height: 80, padding: 10}}>
        <View onLayout={on('child')} style={{height: 10, backgroundColor: '#fff'}} />
      </LinearGradient>
    </View>
  );
}

export function MapsScreen() {
  return <MapsView />;
}
function Skipped({lib, why}: {lib: string; why: string}) {
  useEffect(() => {
    report(lib, {skipped: why});
  }, [lib, why]);
  return null;
}
function MapsView() {
  const MapView = require('react-native-maps').default;
  const [b, on] = useBoxes();
  const ready = useRef(false);
  useEffect(() => {
    wait(4000).then(() =>
      report('react-native-maps', {
        w: b.current.map?.width ?? -1,
        sized: near(b.current.map?.height, 200),
        // Android needs a Google Maps API key to reach "ready"; record it either way.
        mapReady: Platform.OS === 'android' ? String(ready.current) : ready.current,
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <MapView
      onLayout={on('map')}
      onMapReady={() => (ready.current = true)}
      style={{height: 200, margin: 16}}
      initialRegion={{latitude: 31.52, longitude: 74.35, latitudeDelta: 0.1, longitudeDelta: 0.1}}
    />
  );
}

export function VideoScreen() {
  const [b, on] = useBoxes();
  const loaded = useRef<any>(null);
  const error = useRef('');
  useEffect(() => {
    wait(6000).then(() =>
      report('react-native-video', {
        h: b.current.video?.height ?? -1,
        sized: near(b.current.video?.height, 180),
        loaded: loaded.current !== null,
        naturalW: loaded.current?.naturalSize?.width ?? -1,
        error: error.current || 'none',
      }),
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <Video
      onLayout={on('video')}
      source={{uri: 'https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/360/Big_Buck_Bunny_360_10s_1MB.mp4'}}
      muted
      paused={false}
      onLoad={d => (loaded.current = d)}
      onError={e => (error.current = JSON.stringify(e).slice(0, 80))}
      style={{height: 180, margin: 16}}
    />
  );
}

export function CameraScreen() {
  const device = useCameraDevice('back');
  const {hasPermission, requestPermission} = useCameraPermission();
  const [b, on] = useBoxes();
  useEffect(() => {
    if (!hasPermission) {
      requestPermission();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  useEffect(() => {
    const timer = setTimeout(
      () =>
        report('react-native-vision-camera', {
          permission: hasPermission,
          device: device ? device.id : 'none',
          viewW: b.current.cam?.width ?? -1,
          // A device with a camera renders the preview; it must lay out like any view.
          laidOut: !device || !hasPermission || (b.current.cam?.width ?? 0) > 0,
        }),
      6000,
    );
    return () => clearTimeout(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [hasPermission, device]);
  return (
    <View style={{padding: 16}}>
      <Text>camera</Text>
      {device && hasPermission && (
        <Camera onLayout={on('cam')} device={device} isActive style={{height: 200}} />
      )}
    </View>
  );
}
