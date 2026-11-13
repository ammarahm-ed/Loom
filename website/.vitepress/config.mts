import {defineConfig} from 'vitepress';
import {withMermaid} from 'vitepress-plugin-mermaid';

export default withMermaid(
  defineConfig({
  title: 'Loom',
  description: 'A faster renderer for React Native: instant updates, unchanged libraries, and JavaScript on the thread you choose.',
  cleanUrls: true,
  // Mermaid pulls in CommonJS dependencies (e.g. fastdom) that Vite must pre-bundle in dev.
  vite: {optimizeDeps: {include: ['mermaid']}},
  // Size diagram boxes with the same font the page draws them in, so labels aren't clipped.
  mermaid: {
    fontFamily: 'Inter, ui-sans-serif, system-ui, -apple-system, sans-serif',
    themeVariables: {fontFamily: 'Inter, ui-sans-serif, system-ui, -apple-system, sans-serif'},
    flowchart: {padding: 18, htmlLabels: false},
  },
  lastUpdated: true,
  head: [
    ['link', {rel: 'icon', type: 'image/png', href: '/favicon.png'}],
    ['meta', {property: 'og:image', content: '/social-preview.png'}],
    ['meta', {name: 'theme-color', content: '#61DAFB'}],
  ],
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: 'Loom',
    nav: [
      {text: 'Guide', link: '/guide/what-is-loom'},
      {text: 'Reference', link: '/reference/configuration'},
      {text: 'Architecture', link: '/architecture/overview'},
      {text: 'Benchmarks', link: '/benchmarks'},
    ],
    sidebar: {
      '/guide/': [
        {
          text: 'Introduction',
          items: [
            {text: 'What is Loom?', link: '/guide/what-is-loom'},
            {text: 'Getting started', link: '/guide/getting-started'},
            {text: 'Choosing a mode', link: '/guide/modes'},
          ],
        },
        {
          text: 'Guides',
          items: [
            {text: 'Instant updates', link: '/guide/instant-updates'},
            {text: 'Large updates and transitions', link: '/guide/transitions'},
            {text: 'Asynchronous text on iOS', link: '/guide/async-text'},
            {text: 'Finding main-thread jank', link: '/guide/jank-reporter'},
            {text: 'Supported libraries', link: '/guide/compatibility'},
            {text: 'Reanimated', link: '/guide/reanimated'},
            {text: 'Upgrading React Native', link: '/guide/upgrading'},
          ],
        },
      ],
      '/reference/': [
        {
          text: 'Reference',
          items: [
            {text: 'Configuration', link: '/reference/configuration'},
            {text: 'CLI', link: '/reference/cli'},
          ],
        },
      ],
      '/architecture/': [
        {
          text: 'Architecture',
          items: [
            {text: 'Overview', link: '/architecture/overview'},
            {text: 'Overrides model', link: '/architecture/overrides'},
          ],
        },
      ],
    },
    search: {provider: 'local'},
    footer: {
      message: 'Released under the MIT License.',
      copyright: 'Loom is not affiliated with Meta. React Native is a trademark of Meta Platforms, Inc.',
    },
  },
  }),
);
