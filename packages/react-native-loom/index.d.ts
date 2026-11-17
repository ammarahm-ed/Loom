/**
 * Loom's runtime settings. Read and write them while the app runs; a change
 * applies to work that starts afterwards (the next text draw, the next commit).
 *
 * Under stock React Native (Fabric) `Loom` still imports: `renderer` is
 * 'fabric', the other settings report their defaults, and writes do nothing
 * (with a development warning).
 */
export interface LoomSettings {
  /** iOS. Draw paragraph text on a background queue. Default true. */
  asyncText: boolean;
  /**
   * iOS. A view's first drawing is asynchronous too. Default true. Only
   * matters before the first paragraph mounts: set it at the top of index.js.
   */
  asyncTextFirstDraw: boolean;
  /** iOS. A one-off resize of drawn text is redrawn in the background. Default true. */
  asyncTextResize: boolean;
  /**
   * Create host views for a large commit while layout runs. Default true on
   * iOS and Android. Consulted per commit, but views already created stay as
   * they are: set it at the top of index.js to cover the first screen.
   */
  earlyCreate: boolean;
  /** Measure the paragraphs of a large commit in parallel. Default true on iOS, false on Android. */
  parallelText: boolean;
  /** Report main-thread tasks over `jankBudgetMs` (debug builds). Default true. */
  jankReporter: boolean;
  /** Milliseconds a main-thread task may take before it is reported (minimum 1). Default 50. */
  jankBudgetMs: number;
  /** Print worker and commit timing (builds with profiling compiled in). Default false. */
  profileWorker: boolean;
}

export interface LoomBuildInfo {
  /** 'loom' under the Loom renderer, 'fabric' under stock React Native. */
  readonly renderer: 'loom' | 'fabric';
  /** JavaScript runs on the thread that owns native views (the app's `loom.js` startup setting is `main`). */
  readonly jsOnMainThread: boolean;
  /** Layout runs on a worker thread (LOOM_WORKER). */
  readonly worker: boolean;
  /** The react-native-loom package version. */
  readonly version: string;
}

export const Loom: LoomSettings & LoomBuildInfo;
