/**
 * Frame timelines: threads as rows, time left to right, screen refreshes (vsync) as
 * vertical lines. Times are in milliseconds at 60 Hz (one frame = 16.7 ms).
 */
export type Tone = 'brand' | 'green' | 'purple' | 'yellow' | 'indigo' | 'red' | 'gray';

export interface Block {
  row: number;
  start: number;
  end: number;
  label?: string;
  tone?: Tone;
}

export interface Arrow {
  fromRow: number;
  toRow: number;
  at: number;
  /** End time when the arrow is diagonal (a handoff that takes time). */
  to?: number;
}

/** What the screen showed at a refresh. */
export type FrameResult = 'ok' | 'late' | 'stale' | 'none';

export interface TimelineSpec {
  title: string;
  rows: string[];
  /** Number of frames drawn on the axis. */
  frames: number;
  blocks: Block[];
  arrows?: Arrow[];
  /** One entry per refresh line, from the first: what that frame showed. */
  results?: {result: FrameResult; label: string}[];
}

export const FRAME_MS = 1000 / 60;

export const timelines: Record<string, TimelineSpec> = {
  'main-tap': {
    title:
      'Main-thread mode, a tap: the touch, the JavaScript handler, React\u2019s commit and applying the views all run on the main thread before the next refresh, so the change is in the very next frame.',
    rows: ['Main thread'],
    frames: 2,
    blocks: [
      {row: 0, start: 2, end: 3.6, label: 'touch', tone: 'gray'},
      {row: 0, start: 3.6, end: 9, label: 'handler + render', tone: 'indigo'},
      {row: 0, start: 9, end: 11.4, label: 'apply', tone: 'green'},
      {row: 0, start: 16.7, end: 19.2, label: 'draw', tone: 'gray'},
    ],
    results: [
      {result: 'ok', label: 'tap visible'},
      {result: 'none', label: ''},
    ],
  },

  'bg-tap': {
    title:
      'Background mode, the same tap: the touch is forwarded to the JS thread, the commit is laid out, and the views are applied on the main thread at its next opportunity. The change usually lands one frame later.',
    rows: ['Main thread', 'JS thread', 'Loom worker'],
    frames: 3,
    blocks: [
      {row: 0, start: 2, end: 3.5, label: 'touch', tone: 'gray'},
      {row: 1, start: 4.5, end: 10, label: 'JS + render', tone: 'indigo'},
      {row: 2, start: 10.5, end: 14, label: 'layout', tone: 'brand'},
      {row: 0, start: 16.7, end: 19, label: 'draw', tone: 'gray'},
      {row: 0, start: 19.5, end: 22, label: 'apply', tone: 'green'},
      {row: 0, start: 33.4, end: 35.7, label: 'draw', tone: 'gray'},
    ],
    arrows: [
      {fromRow: 0, toRow: 1, at: 3.5, to: 4.5},
      {fromRow: 1, toRow: 2, at: 10, to: 10.5},
      {fromRow: 2, toRow: 0, at: 14, to: 19.5},
    ],
    results: [
      {result: 'stale', label: 'old content'},
      {result: 'ok', label: 'tap visible'},
    ],
  },

  'main-long': {
    title:
      'Main-thread mode, an urgent 30 ms task: the main thread cannot draw until JavaScript returns, so one refresh is missed. The screen freezes for a frame (jank), but nothing on it is ever out of date.',
    rows: ['Main thread'],
    frames: 4,
    blocks: [
      {row: 0, start: 3, end: 33, label: 'urgent JS: 30 ms handler or render', tone: 'red'},
      {row: 0, start: 33, end: 35, label: 'apply', tone: 'green'},
      {row: 0, start: 35, end: 37.5, label: 'draw', tone: 'gray'},
      {row: 0, start: 50.1, end: 52.4, label: 'draw', tone: 'gray'},
    ],
    results: [
      {result: 'late', label: 'missed: jank'},
      {result: 'late', label: 'drawn late'},
      {result: 'ok', label: 'on time'},
    ],
  },

  'bg-long': {
    title:
      'Background mode, the same 30 ms task: the main thread keeps drawing on time, so scrolling and native animations stay smooth, but the frames show old content until JavaScript finishes. In a fast-scrolling list that shows as blank cells.',
    rows: ['Main thread', 'JS thread'],
    frames: 4,
    blocks: [
      {row: 1, start: 3, end: 33, label: 'urgent JS: 30 ms handler or render', tone: 'red'},
      {row: 0, start: 16.7, end: 19, label: 'draw', tone: 'gray'},
      {row: 0, start: 33.4, end: 35.7, label: 'draw', tone: 'gray'},
      {row: 0, start: 36, end: 38, label: 'apply', tone: 'green'},
      {row: 0, start: 50.1, end: 52.4, label: 'draw', tone: 'gray'},
    ],
    arrows: [{fromRow: 1, toRow: 0, at: 33, to: 36}],
    results: [
      {result: 'stale', label: 'smooth, stale'},
      {result: 'stale', label: 'smooth, stale'},
      {result: 'ok', label: 'up to date'},
    ],
  },

  'main-sliced': {
    title:
      'Main-thread mode, a 30 ms startTransition render: Loom runs it in slices of about 5 ms, stops a slice when the refresh is near, and resumes after the frame is drawn. Every frame is on time and the result lands a few frames later.',
    rows: ['Main thread'],
    frames: 4,
    blocks: [
      {row: 0, start: 1, end: 6, label: 'slice', tone: 'indigo'},
      {row: 0, start: 6.3, end: 11.3, label: 'slice', tone: 'indigo'},
      {row: 0, start: 16.7, end: 19, label: 'draw', tone: 'gray'},
      {row: 0, start: 19.3, end: 24.3, label: 'slice', tone: 'indigo'},
      {row: 0, start: 24.6, end: 29.6, label: 'slice', tone: 'indigo'},
      {row: 0, start: 33.4, end: 35.7, label: 'draw', tone: 'gray'},
      {row: 0, start: 36, end: 41, label: 'slice', tone: 'indigo'},
      {row: 0, start: 41.3, end: 46.3, label: 'last slice', tone: 'indigo'},
      {row: 0, start: 46.5, end: 48.5, label: 'apply', tone: 'green'},
      {row: 0, start: 50.1, end: 52.4, label: 'draw', tone: 'gray'},
    ],
    results: [
      {result: 'ok', label: 'on time'},
      {result: 'ok', label: 'on time'},
      {result: 'ok', label: 'result visible'},
    ],
  },

  'main-workers': {
    title:
      'Main-thread mode with heavy work moved off: the 30 ms JSON parse runs in a JavaScript worker (react-native-workers) and Loom’s worker lays out the large commit. The main thread only renders and applies, and every frame is on time.',
    rows: ['Main thread', 'JS worker', 'Loom worker'],
    frames: 4,
    blocks: [
      {row: 0, start: 2, end: 3.5, label: 'post', tone: 'gray'},
      {row: 1, start: 4, end: 34, label: 'parse + sort 5 MB of JSON (30 ms)', tone: 'purple'},
      {row: 0, start: 16.7, end: 19, label: 'draw', tone: 'gray'},
      {row: 0, start: 34.5, end: 39, label: 'render', tone: 'indigo'},
      {row: 2, start: 39.3, end: 44, label: 'layout', tone: 'brand'},
      {row: 0, start: 44.5, end: 46.5, label: 'apply', tone: 'green'},
      {row: 0, start: 33.4, end: 34.3, tone: 'gray'},
      {row: 0, start: 50.1, end: 52.4, label: 'draw', tone: 'gray'},
    ],
    arrows: [
      {fromRow: 0, toRow: 1, at: 3.5, to: 4},
      {fromRow: 1, toRow: 0, at: 34, to: 34.5},
      {fromRow: 0, toRow: 2, at: 39, to: 39.3},
      {fromRow: 2, toRow: 0, at: 44, to: 44.5},
    ],
    results: [
      {result: 'ok', label: 'on time'},
      {result: 'ok', label: 'on time'},
      {result: 'ok', label: 'data visible'},
    ],
  },
};
