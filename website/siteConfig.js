/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

const siteConfig = {
  title: 'facebook360_dep',
  tagline: 'Facebook360 Depth Estimation Pipeline',
  url: 'https://facebook.github.io',
  baseUrl: '/', // Base URL for your project

  // Used for publishing and more
  projectName: 'facebook360_dep',
  organizationName: 'facebook',

  // For no header links in the top nav bar -> headerLinks: [],
  headerLinks: [
    {doc: 'install', label: 'Docs'},
    {doc: 'rig', label: 'Tutorial'},
    {doc: 'api', label: 'API Reference'},
    {doc: 'roadmap', label: 'Roadmap'},
    {doc: 'faqs', label: 'Help'},
    {href: 'https://github.com/facebook/facebook360_dep', label: 'GitHub'},
  ],

  /* path to images for header/footer */
  headerIcon: 'img/favicon.ico',
  footerIcon: 'img/favicon.ico',
  favicon: 'img/favicon.ico',

  // Colors for website
  colors: {
    primaryColor: '#4267B2',
    secondaryColor: '#5890ff',
  },

  // Copyright info
  copyright: `Copyright © ${new Date().getFullYear()} Facebook Inc.`,

  highlight: {
    // Highlight.js theme to use for syntax highlighting in code blocks.
    theme: 'default',
  },

  // Add custom scripts here that would be placed in <script> tags.
  scripts: ['https://buttons.github.io/buttons.js'],

  // On page navigation for the current documentation page.
  onPageNav: 'separate',
  // No .html extensions for paths.
  cleanUrl: true,

  // Open Graph and Twitter card images.
  ogImage: 'img/favicon.ico',
  twitterImage: 'img/favicon.ico',

  // Show documentation's last contributor's name.
  // enableUpdateBy: true,

  // Show documentation's last update time.
  // enableUpdateTime: true,
};

module.exports = siteConfig;
