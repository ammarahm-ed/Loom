import {TurboModuleRegistry} from 'react-native';

export type Frame = {x: number; y: number; width: number; height: number};

interface Native {
  renderer(): 'loom' | 'fabric' | null;
  measure(tag: number): Frame | null;
  setOpacity(tag: number, opacity: number): boolean;
  frames(): number;
}

// A C++ TurboModule registered from native code (cpp/LoomExample.cpp): the same on Android and iOS.
const native = TurboModuleRegistry.get<Native>('LoomExample' as never) as unknown as Native | null;

/** Which renderer answered: 'loom' or 'fabric' (null when the native module is not linked). */
export function renderer(): 'loom' | 'fabric' | null {
  return native ? native.renderer() : null;
}

/** The node's frame in its surface, from the renderer's layout. `tag` is `findNodeHandle(ref)`. */
export function measure(tag: number): Frame | null {
  return native ? native.measure(tag) : null;
}

/** Sets a node's opacity from native code, without a React render. True when the node was found. */
export function setOpacity(tag: number, opacity: number): boolean {
  return native ? native.setOpacity(tag, opacity) : false;
}

/** Frames (Loom, `onFrameApplied`) or mounts (Fabric, a mount hook) the renderer reported so far. */
export function frames(): number {
  return native ? native.frames() : 0;
}
