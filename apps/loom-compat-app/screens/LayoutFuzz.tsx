/**
 * Seeded fuzzer for Reanimated layout animations on Loom. A random tree of Animated and plain views
 * is mutated every 30-250 ms (insert, remove, re-insert the same key, reorder, move to another
 * parent, remove a parent while children exit, resize to trigger layout transitions) with random
 * entering / exiting / layout animations of random durations. Then it settles and checks:
 *   alive         the run completed (a native assertion or crash never reports)
 *   settled       every animation that ended reported finished or cancelled, none still running
 *   placed        every final view's window frame matches its layout frame (Android reads the native
 *                 view position here, so a frame left behind by an animation shows)
 * Launch with scenario `fuzz` or `fuzz:<seed>:<steps>`.
 */
import React, {useEffect, useMemo, useRef, useState} from 'react';
import {Text, View} from 'react-native';
import Animated, {
  FadeIn,
  FadeOut,
  FadingTransition,
  LinearTransition,
  SlideInLeft,
  SlideOutRight,
  ZoomIn,
  ZoomOut,
} from 'react-native-reanimated';
import {runOnJS} from 'react-native-worklets';
import {report, wait} from '../report';

type FuzzNode = {
  id: number;
  animated: boolean;
  enter: number; // index into ENTERING, -1 none
  exit: number;
  layout: number;
  duration: number;
  size: number;
  row: boolean;
  children: FuzzNode[];
};

const ENTERING = [FadeIn, ZoomIn, SlideInLeft];
const EXITING = [FadeOut, ZoomOut, SlideOutRight];
const LAYOUT = [LinearTransition, FadingTransition];

function mulberry32(seed: number) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const counters = {ended: 0, finished: 0, cancelled: 0};
function onEnded(finished: boolean) {
  counters.ended++;
  if (finished) {
    counters.finished++;
  } else {
    counters.cancelled++;
  }
}

function FuzzView({node, refs}: {node: FuzzNode; refs: Map<number, View | null>}) {
  const style = {
    padding: 2,
    minWidth: node.size,
    minHeight: node.size,
    margin: 2,
    flexDirection: node.row ? ('row' as const) : ('column' as const),
    flexWrap: 'wrap' as const,
    backgroundColor: node.animated ? `hsl(${(node.id * 47) % 360}, 60%, 60%)` : '#ddd',
  };
  const children = node.children.map(child => <FuzzView key={child.id} node={child} refs={refs} />);
  const setRef = (v: any) => refs.set(node.id, v);
  if (!node.animated) {
    return (
      <View ref={setRef} collapsable={node.id % 3 !== 0} style={style}>
        {children}
      </View>
    );
  }
  return <AnimatedFuzzView node={node} style={style} setRef={setRef} children={children} />;
}

/** Builds a node's animation configs once, as an app would, so re-renders do not re-register them. */
function AnimatedFuzzView({
  node,
  style,
  setRef,
  children,
}: {
  node: FuzzNode;
  style: object;
  setRef: (v: any) => void;
  children: React.ReactNode;
}) {
  const {entering, exiting, layout} = useMemo(() => {
    const cb = (finished: boolean) => {
      'worklet';
      runOnJS(onEnded)(finished);
    };
    return {
      entering: node.enter >= 0 ? ENTERING[node.enter].duration(node.duration).withCallback(cb) : undefined,
      exiting: node.exit >= 0 ? EXITING[node.exit].duration(node.duration).withCallback(cb) : undefined,
      layout: node.layout >= 0 ? LAYOUT[node.layout].duration(node.duration).withCallback(cb) : undefined,
    };
  }, [node.enter, node.exit, node.layout, node.duration]);
  return (
    <Animated.View ref={setRef} entering={entering} exiting={exiting} layout={layout} style={style}>
      {children}
    </Animated.View>
  );
}

export default function LayoutFuzz({seed: seedArg, steps: stepsArg}: {seed?: number; steps?: number}) {
  const seed = seedArg ?? Date.now() % 100000;
  const steps = stepsArg ?? 400;
  const random = useRef(mulberry32(seed)).current;
  const nextId = useRef(1);
  const refs = useRef(new Map<number, View | null>()).current;
  const [tree, setTree] = useState<FuzzNode[]>([]);
  const [step, setStep] = useState(0);

  const pick = <T,>(items: T[]) => items[Math.floor(random() * items.length)];
  const makeNode = (depth: number): FuzzNode => {
    const node: FuzzNode = {
      id: nextId.current++,
      animated: random() < 0.75,
      enter: random() < 0.7 ? Math.floor(random() * ENTERING.length) : -1,
      exit: random() < 0.7 ? Math.floor(random() * EXITING.length) : -1,
      layout: random() < 0.5 ? Math.floor(random() * LAYOUT.length) : -1,
      duration: 50 + Math.floor(random() * 550),
      size: 14 + Math.floor(random() * 30),
      row: random() < 0.5,
      children: [],
    };
    if (depth < 3) {
      const count = Math.floor(random() * 3);
      for (let i = 0; i < count; i++) {
        node.children.push(makeNode(depth + 1));
      }
    }
    return node;
  };

  // Flat list of [parent children array, index] slots for random picks.
  const slots = (nodes: FuzzNode[], out: {list: FuzzNode[]; node: FuzzNode}[] = []) => {
    for (const node of nodes) {
      out.push({list: nodes, node});
      slots(node.children, out);
    }
    return out;
  };
  const containers = (nodes: FuzzNode[], out: FuzzNode[][] = [nodes]) => {
    for (const node of nodes) {
      out.push(node.children);
      containers(node.children, out);
    }
    return out;
  };
  const clone = (nodes: FuzzNode[]): FuzzNode[] => nodes.map(n => ({...n, children: clone(n.children)}));
  const contains = (node: FuzzNode, list: FuzzNode[]): boolean =>
    node.children === list || node.children.some(child => contains(child, list));

  const removed = useRef<FuzzNode[]>([]);

  const mutate = (current: FuzzNode[]): FuzzNode[] => {
    const next = clone(current);
    const all = slots(next);
    // Keep the tree around 60 views: past that, removals win.
    const op = all.length > 60 ? 0.25 + random() * 0.2 : random();
    if (all.length < 4 || op < 0.25) {
      // insert a new subtree somewhere
      const list = pick(containers(next));
      list.splice(Math.floor(random() * (list.length + 1)), 0, makeNode(1));
    } else if (op < 0.45) {
      // remove a subtree (remember it for a later re-insert with the same keys)
      const {list, node} = pick(all);
      list.splice(list.indexOf(node), 1);
      removed.current.push(node);
    } else if (op < 0.55 && removed.current.length > 0) {
      // re-insert a removed subtree with the same keys (often while it still exits)
      const node = removed.current.splice(Math.floor(random() * removed.current.length), 1)[0];
      if (!slots(next).some(s => s.node.id === node.id)) {
        const list = pick(containers(next));
        list.splice(Math.floor(random() * (list.length + 1)), 0, node);
      }
    } else if (op < 0.7) {
      // reorder within a parent
      const list = pick(containers(next).filter(l => l.length > 1));
      if (list) {
        const [moved] = list.splice(Math.floor(random() * list.length), 1);
        list.splice(Math.floor(random() * (list.length + 1)), 0, moved);
      }
    } else if (op < 0.8) {
      // move a node to another parent
      const {list, node} = pick(all);
      // Keep nesting realistic (deeply nested wrap containers make Yoga re-measure exponentially).
      const depthOf = (n: FuzzNode): number => 1 + Math.max(0, ...n.children.map(depthOf));
      const depthTo = (l: FuzzNode[], nodes: FuzzNode[], d = 0): number => {
        if (nodes === l) {
          return d;
        }
        for (const n of nodes) {
          const found = depthTo(l, n.children, d + 1);
          if (found >= 0) {
            return found;
          }
        }
        return -1;
      };
      const targets = containers(next).filter(
        l => l !== list && !contains(node, l) && depthTo(l, next) + depthOf(node) <= 6,
      );
      if (targets.length > 0) {
        list.splice(list.indexOf(node), 1);
        const target = pick(targets);
        target.splice(Math.floor(random() * (target.length + 1)), 0, node);
      }
    } else if (op < 0.92) {
      // resize / flip direction: layout transitions on it and its siblings
      const {node} = pick(all);
      node.size = 14 + Math.floor(random() * 30);
      node.row = !node.row;
    } else {
      // clear a whole branch at once
      const {node} = pick(all);
      removed.current.push(...node.children);
      node.children = [];
    }
    return next;
  };

  useEffect(() => {
    let cancelled = false;
    (async () => {
      await wait(500);
      let current: FuzzNode[] = [makeNode(1), makeNode(1)];
      setTree(current);
      for (let i = 0; i < steps && !cancelled; i++) {
        await wait(30 + Math.floor(random() * 220));
        current = mutate(current);
        setTree(current);
        setStep(i + 1);
      }
      // Settle: the longest animation is 600 ms; wait for a full quiet second (at most 6 s).
      const settleStart = Date.now();
      let settled = false;
      while (Date.now() - settleStart < 6000) {
        const before = counters.ended;
        await wait(1000);
        if (counters.ended === before) {
          settled = true;
          break;
        }
      }
      const settleMs = Date.now() - settleStart;
      // Every final view: window frame vs layout frame.
      const finalIds = slots(current).map(s => s.node.id);
      let misplaced = 0;
      let measured = 0;
      for (const id of finalIds) {
        const view = refs.get(id);
        if (!view) {
          continue;
        }
        const [win, page] = await Promise.all([
          new Promise<number[]>(r => view.measureInWindow((x, y, w, h) => r([x, y, w, h]))),
          new Promise<number[]>(r => view.measure((_x, _y, w, h, px, py) => r([px, py, w, h]))),
        ]);
        measured++;
        if (win.some((value, k) => Math.abs(value - page[k]) > 1.5)) {
          misplaced++;
        }
      }
      report(`fuzz seed ${seed}`, {
        steps,
        alive: true,
        ended: counters.ended,
        finished: counters.finished,
        cancelled: counters.cancelled,
        settled,
        settleMs,
        views: finalIds.length,
        measured,
        misplaced,
        placed: misplaced === 0,
      });
    })();
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <View style={{flex: 1, paddingTop: 60, paddingHorizontal: 8}}>
      <Text>{`fuzz seed ${seed} step ${step}/${steps}`}</Text>
      <View style={{flex: 1, flexDirection: 'row', flexWrap: 'wrap', alignContent: 'flex-start'}}>
        {tree.map(node => (
          <FuzzView key={node.id} node={node} refs={refs} />
        ))}
      </View>
    </View>
  );
}
