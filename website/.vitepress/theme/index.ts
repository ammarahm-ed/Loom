import DefaultTheme from 'vitepress/theme';
import type {Theme} from 'vitepress';
import LoomDiagram from './components/LoomDiagram.vue';
import FrameTimeline from './components/FrameTimeline.vue';
import './custom.css';

export default {
  extends: DefaultTheme,
  enhanceApp({app}) {
    app.component('LoomDiagram', LoomDiagram);
    app.component('FrameTimeline', FrameTimeline);
  },
} satisfies Theme;
