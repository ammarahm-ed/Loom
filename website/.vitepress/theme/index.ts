import DefaultTheme from 'vitepress/theme';
import type {Theme} from 'vitepress';
import LoomDiagram from './components/LoomDiagram.vue';
import './custom.css';

export default {
  extends: DefaultTheme,
  enhanceApp({app}) {
    app.component('LoomDiagram', LoomDiagram);
  },
} satisfies Theme;
