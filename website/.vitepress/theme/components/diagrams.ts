import type {Spec} from './layout';

export const specs: Record<string, Spec> = {
  sits: {
    title: 'Where Loom sits: React calls LoomBinding, the Loom core lays out and batches, LoomHost mounts native views; libraries reach the core through the Fabric compatibility layer.',
    lanes: ['Renderer path', 'Libraries'],
    cols: 5,
    nodes: [
      {id: 'app', lane: 0, col: 0, title: 'App and React', sub: 'components, hooks, state'},
      {id: 'bind', lane: 0, col: 1, title: 'LoomBinding', sub: 'the host functions React calls', tone: 'brand'},
      {id: 'core', lane: 0, col: 2, title: 'Loom core', sub: 'node tree, Yoga, worker, mount batches', tone: 'brand'},
      {id: 'host', lane: 0, col: 3, title: 'LoomHost', sub: 'iOS, Android, headless', tone: 'green'},
      {id: 'views', lane: 0, col: 4, title: 'Native views'},
      {id: 'libs', lane: 1, col: 1, title: 'Libraries', sub: 'C++ code that uses Fabric internals'},
      {id: 'compat', lane: 1, col: 2, title: 'Compatibility layer', sub: 'emulates Fabric contracts', tone: 'purple'},
    ],
    edges: [
      {from: 'app', to: 'bind'},
      {from: 'bind', to: 'core'},
      {from: 'core', to: 'host'},
      {from: 'host', to: 'views'},
      {from: 'libs', to: 'compat', dashed: true},
      {from: 'compat', to: 'core', dashed: true},
    ],
  },

  pipeline: {
    title: 'From a React commit to mounted views: Fabric clones shadow nodes, lays out and diffs two trees; Loom updates nodes in place, lays out and applies one mount batch.',
    lanes: ['Fabric', 'Loom'],
    cols: 5,
    nodes: [
      {id: 'f0', lane: 0, col: 0, title: 'React commit'},
      {id: 'f1', lane: 0, col: 1, title: 'Clone nodes', sub: 'immutable shadow tree'},
      {id: 'f2', lane: 0, col: 2, title: 'Layout', sub: 'Yoga and text'},
      {id: 'f3', lane: 0, col: 3, title: 'Diff trees', sub: 'old against new'},
      {id: 'f4', lane: 0, col: 4, title: 'Mount', sub: 'mutations for the UI thread'},
      {id: 'l0', lane: 1, col: 0, title: 'React commit'},
      {id: 'l1', lane: 1, col: 1, title: 'Update in place', sub: 'changed nodes only', tone: 'brand'},
      {id: 'l2', lane: 1, col: 2, title: 'Layout', sub: 'Yoga and text', tone: 'brand'},
      {id: 'l3', lane: 1, col: 3, title: 'Mount batch', sub: 'typed view operations', tone: 'brand'},
      {id: 'l4', lane: 1, col: 4, title: 'Apply', sub: 'one call on the UI thread', tone: 'brand'},
    ],
    edges: [
      {from: 'f0', to: 'f1'}, {from: 'f1', to: 'f2'}, {from: 'f2', to: 'f3'}, {from: 'f3', to: 'f4'},
      {from: 'l0', to: 'l1'}, {from: 'l1', to: 'l2'}, {from: 'l2', to: 'l3'}, {from: 'l3', to: 'l4'},
    ],
  },

  jsmain: {
    title: 'JavaScript on the main thread: input, JavaScript and view application run on the main thread; large commits detour through the worker for layout, text and diff.',
    lanes: ['Main thread', 'Worker thread'],
    cols: 5,
    nodes: [
      {id: 'in', lane: 0, col: 0, title: 'Input'},
      {id: 'js', lane: 0, col: 1, title: 'JavaScript', sub: 'React renders and commits', tone: 'indigo'},
      {id: 'apply', lane: 0, col: 3, title: 'Apply views', sub: 'LoomHost::applyBatch', tone: 'green'},
      {id: 'draw', lane: 0, col: 4, title: 'Draw'},
      {id: 'w', lane: 1, col: 2, title: 'Layout, text, diff', sub: 'large commits only', tone: 'brand'},
    ],
    edges: [
      {from: 'in', to: 'js'},
      {from: 'js', to: 'apply', label: 'small commits run inline', dashed: true},
      {from: 'js', to: 'w'},
      {from: 'w', to: 'apply'},
      {from: 'apply', to: 'draw'},
    ],
  },

  jsbg: {
    title: 'JavaScript on its own thread: events cross from the main thread to JavaScript, commits go to the worker, and view operations return to the main thread.',
    lanes: ['JS thread', 'Worker thread', 'Main thread'],
    cols: 5,
    nodes: [
      {id: 'js', lane: 0, col: 1, title: 'JavaScript', sub: 'React renders and commits', tone: 'indigo'},
      {id: 'w', lane: 1, col: 2, title: 'Layout, text, diff', sub: 'large commits; small ones run inline', tone: 'brand'},
      {id: 'in', lane: 2, col: 0, title: 'Input'},
      {id: 'apply', lane: 2, col: 3, title: 'Apply views', sub: 'LoomHost::applyBatch', tone: 'green'},
      {id: 'draw', lane: 2, col: 4, title: 'Draw'},
    ],
    edges: [
      {from: 'in', to: 'js', dashed: true},
      {from: 'js', to: 'w'},
      {from: 'w', to: 'apply'},
      {from: 'apply', to: 'draw'},
    ],
  },

  tapmain: {
    title: 'A tap with JavaScript on the main thread: onPress runs and the views are applied during input handling, so the next frame shows the change.',
    lanes: ['Touch and display', 'Main thread'],
    cols: 4,
    nodes: [
      {id: 't', lane: 0, col: 0, title: 'Touch', sub: 'delivered to the app'},
      {id: 'd', lane: 0, col: 3, title: 'Next frame', sub: 'shows the change'},
      {id: 'p', lane: 1, col: 1, title: 'onPress runs', sub: 'React commits', tone: 'indigo'},
      {id: 'a', lane: 1, col: 2, title: 'Apply views', sub: 'Loom applies the changes', tone: 'green'},
    ],
    edges: [{from: 't', to: 'p'}, {from: 'p', to: 'a'}, {from: 'a', to: 'd'}],
  },

  tapbg: {
    title: 'A tap with JavaScript on its own thread: the event crosses to the JS thread and the view changes cross back, so the change usually lands a frame later.',
    lanes: ['Touch and display', 'Main thread', 'JS thread'],
    cols: 5,
    nodes: [
      {id: 't', lane: 0, col: 0, title: 'Touch', sub: 'delivered to the app'},
      {id: 'd', lane: 0, col: 4, title: 'A frame later', sub: 'shows the change'},
      {id: 'e', lane: 1, col: 1, title: 'Input handling', sub: 'event posted to JS'},
      {id: 'a', lane: 1, col: 3, title: 'Apply views', sub: 'Loom applies the changes', tone: 'green'},
      {id: 'p', lane: 2, col: 2, title: 'onPress runs', sub: 'React commits', tone: 'indigo'},
    ],
    edges: [{from: 't', to: 'e'}, {from: 'e', to: 'p'}, {from: 'p', to: 'a'}, {from: 'a', to: 'd'}],
  },

  architecture: {
    title: 'Architecture: React calls LoomBinding on the JS thread, LoomWorker prepares, lays out and diffs into a LoomMountBatch, and the UI thread replays it into LoomHost applyBatch and native views. The host feeds events, frame hooks, text services and platform traits.',
    lanes: ['Host inputs', 'JS thread', 'Worker thread', 'UI thread', 'Host services'],
    cols: 5,
    nodes: [
      {id: 'ev', lane: 0, col: 0, title: 'Native events', sub: 'LoomEventManager', tone: 'green'},
      {id: 'fr', lane: 0, col: 1, title: 'Frame hooks', sub: 'FrameClock, FrameCallbackScope', tone: 'green'},
      {id: 'react', lane: 1, col: 0, title: 'React', sub: 'completeRoot', tone: 'indigo'},
      {id: 'bind', lane: 1, col: 1, title: 'LoomBinding', sub: 'JSI host functions', tone: 'brand'},
      {id: 'worker', lane: 2, col: 1, span: 2, title: 'LoomWorker', sub: 'prepare, Yoga layout, diff', tone: 'brand'},
      {id: 'batch', lane: 2, col: 3, title: 'LoomMountBatch', sub: 'ordered view operations'},
      {id: 'host', lane: 3, col: 3, title: 'LoomHost', sub: 'applyBatch', tone: 'green'},
      {id: 'views', lane: 3, col: 4, title: 'Native views'},
      {id: 'txt', lane: 4, col: 1, title: 'LoomTextServices', sub: 'measureText, prepareText', tone: 'green'},
      {id: 'traits', lane: 4, col: 2, title: 'Platform traits', sub: 'LoomPlatformTraits, set once', tone: 'green'},
    ],
    edges: [
      {from: 'ev', to: 'react'},
      {from: 'fr', to: 'bind', label: 'flushAnimationFrames'},
      {from: 'react', to: 'bind'},
      {from: 'bind', to: 'worker', label: 'submit job'},
      {from: 'worker', to: 'batch'},
      {from: 'batch', to: 'host', label: 'uiExecutor, in order'},
      {from: 'host', to: 'views'},
      {from: 'worker', to: 'txt', label: 'measureText'},
      {from: 'traits', to: 'worker', dashed: true},
    ],
  },

  commit: {
    title: 'One commit in time: completeRoot and publishWorkerCommit on the JS thread, compute on the worker, effects replayed on the UI thread, then LoomHost applyBatch.',
    lanes: ['JS thread', 'Worker thread', 'UI thread'],
    cols: 4,
    nodes: [
      {id: 'cr', lane: 0, col: 0, title: 'completeRoot', sub: 'React commits the tree', tone: 'indigo'},
      {id: 'pub', lane: 0, col: 1, title: 'publishWorkerCommit', sub: 'freeze changed nodes', tone: 'brand'},
      {id: 'comp', lane: 1, col: 1, title: 'Compute', sub: 'reconcile, prepare, Yoga layout, diff', tone: 'brand'},
      {id: 'rec', lane: 1, col: 2, title: 'Effects recorded', sub: 'applyBatch deferred with its LoomMountBatch'},
      {id: 'rep', lane: 2, col: 2, title: 'Replay in order', sub: 'job applied, then the effects'},
      {id: 'ab', lane: 2, col: 3, title: 'LoomHost::applyBatch', sub: 'six phases to native views', tone: 'green'},
    ],
    edges: [
      {from: 'cr', to: 'pub'},
      {from: 'pub', to: 'comp', label: 'submit job'},
      {from: 'comp', to: 'rec'},
      {from: 'rec', to: 'rep', label: 'uiExecutor'},
      {from: 'rep', to: 'ab'},
    ],
  },

  animprops: {
    title: 'Animated props with the animation backend: paint-only props go straight to the view; layout props go through Loom layout and are mounted in the same frame.',
    lanes: ['Reanimated', 'Paint props only', 'Layout props'],
    cols: 5,
    nodes: [
      {id: 'style', lane: 0, col: 0, title: 'Animated style', sub: 'worklet on the UI runtime', tone: 'purple'},
      {id: 'be', lane: 0, col: 1, title: 'AnimationBackend', sub: 'applySurfaceUpdates', tone: 'purple'},
      {id: 'sync', lane: 1, col: 2, title: 'UIManager', sub: 'synchronouslyUpdateAnimatedProps, one batched write'},
      {id: 'lane', lane: 2, col: 2, title: 'Animation lane', sub: 'buffered per frame, keyed by tag', tone: 'brand'},
      {id: 'ren', lane: 2, col: 3, title: 'LoomRenderer', sub: 'applyAnimationFrame: patch, Yoga layout', tone: 'brand'},
      {id: 'host', lane: 2, col: 4, title: 'LoomHost', sub: 'applyBatch, animationFrame = true', tone: 'green'},
      {id: 'view', lane: 1, col: 4, title: 'Native view'},
    ],
    edges: [
      {from: 'style', to: 'be'},
      {from: 'be', to: 'sync'},
      {from: 'be', to: 'lane'},
      {from: 'sync', to: 'view', label: 'straight to the view'},
      {from: 'lane', to: 'ren'},
      {from: 'ren', to: 'host'},
      {from: 'host', to: 'view'},
    ],
  },

  layoutanim: {
    title: 'Layout animations: the adapter sees each LoomMountBatch before the platform applies it, starts Reanimated animations, and appends their frames as further batches.',
    lanes: ['Loom', 'Adapter', 'Reanimated', 'Platform'],
    cols: 5,
    nodes: [
      {id: 'batch', lane: 0, col: 0, title: 'LoomMountBatch', sub: 'from a React commit', tone: 'brand'},
      {id: 'pb', lane: 1, col: 0, title: 'processBatch', sub: 'in the adapter proxy, before the platform', tone: 'yellow'},
      {id: 'anim', lane: 2, col: 1, title: 'Animation', sub: 'Reanimated worklet on the UI runtime', tone: 'purple'},
      {id: 'frame', lane: 1, col: 2, title: 'frame', sub: 'appends updates and due removals', tone: 'yellow'},
      {id: 'host', lane: 3, col: 3, title: 'LoomHost', sub: 'applyBatch on the UI thread', tone: 'green'},
      {id: 'views', lane: 3, col: 4, title: 'Native views'},
    ],
    edges: [
      {from: 'batch', to: 'pb', label: 'before apply'},
      {from: 'pb', to: 'anim', mid: 0.5},
      {from: 'anim', to: 'frame', label: ''},
      {from: 'pb', to: 'host', vfirst: true, dashed: true, label: 'batch with the adapter\'s operations'},
      {from: 'frame', to: 'host'},
      {from: 'host', to: 'views'},
    ],
  },

  shared: {
    title: 'Shared element transitions: when the top boundary changes, matching views are paired by tag, hidden, and a copy at the surface root animates between them.',
    lanes: ['Adapter', 'iOS native pop'],
    cols: 5,
    nodes: [
      {id: 's0', lane: 0, col: 0, title: 'Top boundary changes', sub: 'seen in processBatch', tone: 'yellow'},
      {id: 's1', lane: 0, col: 1, title: 'Pair views', sub: 'same sharedTransitionTag', tone: 'yellow'},
      {id: 's2', lane: 0, col: 2, title: 'Hide both, add a copy', sub: 'copy mounted at the surface root', tone: 'yellow'},
      {id: 's3', lane: 0, col: 3, title: 'SharedTransition', sub: 'Reanimated animates the copy', tone: 'purple'},
      {id: 's4', lane: 0, col: 4, title: 'Copy removed', sub: 'the target view shows'},
      {id: 'pop', lane: 1, col: 3, title: 'Transition progress', sub: 'onTransitionProgress, onGestureCancel', dashed: true},
    ],
    edges: [
      {from: 's0', to: 's1'}, {from: 's1', to: 's2'}, {from: 's2', to: 's3'}, {from: 's3', to: 's4'},
      {from: 'pop', to: 's3', dashed: true},
    ],
  },
};
