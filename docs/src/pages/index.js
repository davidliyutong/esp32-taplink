import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';

function HomepageHeader() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header
      className={clsx('hero hero--primary')}
      style={{textAlign: 'center', padding: '4rem 0'}}>
      <div className="container">
        <h1 className="hero__title">{siteConfig.title}</h1>
        <p className="hero__subtitle">{siteConfig.tagline}</p>
        <div style={{display: 'flex', gap: '1rem', justifyContent: 'center'}}>
          <Link
            className="button button--secondary button--lg"
            to="/docs/getting-started">
            Getting Started
          </Link>
          <Link
            className="button button--outline button--secondary button--lg"
            href="https://github.com/davidliyutong/esp32-taplink">
            GitHub
          </Link>
        </div>
      </div>
    </header>
  );
}

const features = [
  {
    title: 'Plug and Play',
    description:
      'Shows up as a standard USB Ethernet adapter — driverless on macOS and Linux, CDC-NCM on Windows.',
  },
  {
    title: 'Dual-Subnet Router',
    description:
      'Independent DHCP pools and IP forwarding between USB and WiFi sides, with automatic static route injection.',
  },
  {
    title: 'Web Management',
    description:
      'Built-in dashboard with DHCP lease table, WiFi/network settings, port forwarding, and live diagnostics.',
  },
];

export default function Home() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout title="Home" description={siteConfig.tagline}>
      <HomepageHeader />
      <main style={{padding: '2rem 0'}}>
        <div className="container">
          <div className="row">
            {features.map((f, idx) => (
              <div key={idx} className={clsx('col col--4')} style={{marginBottom: '2rem'}}>
                <h3>{f.title}</h3>
                <p>{f.description}</p>
              </div>
            ))}
          </div>
          <div style={{textAlign: 'center', marginTop: '1rem'}}>
            <pre style={{
              display: 'inline-block',
              textAlign: 'left',
              fontSize: '0.85rem',
              padding: '1.5rem',
            }}>
{`┌──────────┐     USB NCM     ┌───────────┐     WiFi AP     ┌────────────┐
│   Host   │────────────────▶│  ESP32-S3 │◀────────────────│  Clients   │
│  (DHCP)  │  192.168.5.0/24 │  (router) │ 192.168.4.0/24  │   (DHCP)   │
└──────────┘                 └───────────┘                 └────────────┘`}
            </pre>
          </div>
        </div>
      </main>
    </Layout>
  );
}
