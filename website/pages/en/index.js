/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

const React = require('react');

const CompLibrary = require('../../core/CompLibrary.js');

const MarkdownBlock = CompLibrary.MarkdownBlock; /* Used to read markdown */
const Container = CompLibrary.Container;
const GridBlock = CompLibrary.GridBlock;

class HomeSplash extends React.Component {
  render() {
    const {siteConfig, language = ''} = this.props;
    const {baseUrl, docsUrl} = siteConfig;
    const docsPart = `${docsUrl ? `${docsUrl}/` : ''}`;
    const langPart = `${language ? `${language}/` : ''}`;
    const docUrl = doc => `${baseUrl}${docsPart}${langPart}${doc}`;

    const SplashContainer = props => (
      <div className="homeContainer">
        <div className="homeSplashFade">
          <div className="wrapper homeWrapper">{props.children}</div>
        </div>
      </div>
    );

    const Logo = props => (
      <div className="splashLogo">
        <img src={props.img_src} alt="Project Logo" width="70%"/>
      </div>
    );

    const PromoSection = props => (
      <div className="section promoSection">
        <div className="promoRow">
          <div className="pluginRowBlock">{props.children}</div>
        </div>
      </div>
    );

    const Button = props => (
      <div className="pluginWrapper buttonWrapper">
        <a className="button" href={props.href} target={props.target}>
          {props.children}
        </a>
      </div>
    );

    return (
      <SplashContainer>
        <Logo img_src={`${baseUrl}img/dep_logo.png`} />
        <div className="inner">
          <PromoSection>
            <Button href="https://github.com/facebook/facebook360_dep">Start Hacking</Button>
            <Button href={siteConfig.baseUrl + "docs/install"}>Tutorial</Button>
          </PromoSection>
        </div>
      </SplashContainer>
    );
  }
}

class Index extends React.Component {
  render() {
    const {config: siteConfig, language = ''} = this.props;
    const {baseUrl} = siteConfig;

    const Block = props => (
      <Container
        padding={['bottom', 'top']}
        id={props.id}
        background={props.background}>
        <GridBlock
          align="center"
          contents={props.children}
          layout={props.layout}
        />
      </Container>
    );

    const Features = props => (
      <Block layout="threeColumn" background="light">
        {[
          {
            content: `Built to cater to all users, with a streamlined user interface
            for producers interested in directly diving into creating 6DoF content and
            robust internals for developers.`,
            imageAlign: 'top',
            title: '<h2>Accessible</h2>',
          },
          {
            content: `Built on a rendering pipeline composed of loosely coupled components to
            enable tinkering with any of its pieces.`,
            imageAlign: 'top',
            title: '<h2>Pluggable</h2>',
          },
          {
            content: `Designed without any constraints imposed on the camera systems used
            to capture content, allowing rapid prototyping of new camera arrangements and
            lens types.`,
            imageAlign: 'top',
            title: '<h2>Flexible</h2>',
          }
        ]}
      </Block>
    );

    return (
      <div>
        <HomeSplash siteConfig={siteConfig} language={language} />
        <Features />
      </div>
    );
  }
}

module.exports = Index;
