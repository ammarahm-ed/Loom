import {defineConfig} from 'vitepress';

// Served from https://ammarahm-ed.github.io/loom/ on GitHub Pages; `/` for local dev and preview.
const base = process.env.LOOM_DOCS_BASE ?? '/';

export default defineConfig({
  base,
  title: 'Loom',
  description: 'A faster renderer for React Native: instant updates, unchanged libraries, and JavaScript on the thread you choose.',
  cleanUrls: true,
  lastUpdated: true,
  head: [
    ['link', {rel: 'icon', type: 'image/png', href: `${base}favicon.png`}],
    ['meta', {property: 'og:image', content: `${base}social-preview.png`}],
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
            {text: 'Supporting Loom in your library', link: '/guide/supporting-loom'},
            {text: 'Implementing a platform', link: '/guide/platforms'},
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
            {text: 'Maintaining Loom', link: '/architecture/maintaining'},
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
});