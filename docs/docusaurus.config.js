// @ts-check

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'ESP32-TapLink',
  tagline: 'USB NCM-to-WiFi AP network router for ESP32-S3',
  favicon: 'img/favicon.ico',

  url: 'https://davidliyutong.github.io',
  baseUrl: '/esp32-taplink/',

  organizationName: 'davidliyutong',
  projectName: 'esp32-taplink',

  onBrokenLinks: 'throw',

  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'warn',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          sidebarPath: './sidebars.js',
          editUrl: 'https://github.com/davidliyutong/esp32-taplink/tree/main/docs/',
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      navbar: {
        title: 'ESP32-TapLink',
        items: [
          {
            type: 'docSidebar',
            sidebarId: 'docsSidebar',
            position: 'left',
            label: 'Docs',
          },
          {
            href: 'https://github.com/davidliyutong/esp32-taplink',
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              { label: 'Getting Started', to: '/docs/getting-started' },
              { label: 'Architecture', to: '/docs/architecture' },
              { label: 'Configuration', to: '/docs/configuration' },
            ],
          },
          {
            title: 'More',
            items: [
              {
                label: 'GitHub',
                href: 'https://github.com/davidliyutong/esp32-taplink',
              },
            ],
          },
        ],
        copyright: `Copyright © ${new Date().getFullYear()} ESP32-TapLink. Built with Docusaurus.`,
      },
      prism: {
        theme: require('prism-react-renderer').themes.github,
        darkTheme: require('prism-react-renderer').themes.dracula,
        additionalLanguages: ['bash', 'c'],
      },
    }),
};

module.exports = config;
