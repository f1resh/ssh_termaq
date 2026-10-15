/**
 * App information + third-party notices, shared by the About dialog and the Settings › About page.
 */
import { ExternalLink, FolderOpen } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button } from '@/components/ui';
import { Logo } from '@/components/welcome/Logo';
import { APP_NAME } from '@shared/defaults';
import type { AppInfo } from '@shared/types';
import { Badge, Spinner } from '../common/controls';
import { useAsync } from '../common/hooks';
import { formatBuildId, formatBuildTime } from './buildInfo';

export interface ThirdPartyLicense {
  name: string;
  license: string;
  url: string;
  usage: string;
}

export const THIRD_PARTY_LICENSES: ThirdPartyLicense[] = [
  { name: 'libssh', license: 'LGPL-2.1', url: 'https://www.libssh.org/', usage: 'SSH protocol, SFTP, port forwarding' },
  { name: 'OpenSSL', license: 'Apache-2.0', url: 'https://www.openssl.org/', usage: 'Cryptography' },
  { name: 'zlib', license: 'Zlib', url: 'https://zlib.net/', usage: 'SSH compression' },
  { name: 'nlohmann/json', license: 'MIT', url: 'https://github.com/nlohmann/json', usage: 'JSON for the native host' },
  { name: 'Microsoft Edge WebView2 SDK', license: 'BSD-3-Clause', url: 'https://developer.microsoft.com/microsoft-edge/webview2/', usage: 'UI host' },
  { name: 'Windows Implementation Libraries (WIL)', license: 'MIT', url: 'https://github.com/microsoft/wil', usage: 'Win32 RAII helpers' },
  { name: 'xterm.js', license: 'MIT', url: 'https://xtermjs.org/', usage: 'Terminal emulator' },
  { name: 'React', license: 'MIT', url: 'https://react.dev/', usage: 'User interface' },
  { name: 'zustand', license: 'MIT', url: 'https://github.com/pmndrs/zustand', usage: 'UI state' },
  { name: 'Lucide', license: 'ISC', url: 'https://lucide.dev/', usage: 'Icons' }
];

export function AboutContent({ compact }: { compact?: boolean }) {
  const { data: info, loading } = useAsync<AppInfo>(() => api.app.info(), []);
  const buildId = info && formatBuildId(info);
  const buildTime = formatBuildTime(info?.buildTime);
  return (
    <div className={compact ? 'about about--compact' : 'about'}>
      <div className="about__hero">
        <Logo size={56} className="about__logo" />
        <div>
          <div className="about__name">{info?.name || APP_NAME}</div>
          <div className="about__version">
            {loading ? <Spinner size={12} /> : <>Version {info?.version ?? 'unknown'}{info?.build ? ` (build ${info.build})` : ''}</>}
            {info?.portable && (
              <Badge tone="accent" className="about__badge">
                Portable
              </Badge>
            )}
          </div>
          <div className="about__slogan">Your ultimate destination</div>
          <div className="about__tagline">
            A modern SSH, Telnet, Rlogin, Raw and Serial client for Windows. The name comes from <i>terminus ad quem</i>, Latin
            for “the end point toward which”.
          </div>
        </div>
      </div>

      {info && (
        <dl className="about__facts">
          {buildId && (
            <>
              <dt>Build</dt>
              <dd className="mono-text" title="Build number (commits so far) and commit; -dirty: built with uncommitted changes">
                {buildId}
              </dd>
            </>
          )}
          {buildTime && (
            <>
              <dt>Built</dt>
              <dd>{buildTime}</dd>
            </>
          )}
          {info.chrome && (
            <>
              <dt>Web runtime</dt>
              <dd className="mono-text">{info.chrome}</dd>
            </>
          )}
          {info.platform && (
            <>
              <dt>Platform</dt>
              <dd>{info.platform}</dd>
            </>
          )}
          <dt>Data folder</dt>
          <dd className="about__path">
            <span className="mono-text" title={info.userDataDir}>
              {info.userDataDir}
            </span>
            <Button size="sm" variant="ghost" icon={<FolderOpen size={13} />} onClick={() => void api.app.openPath(info.userDataDir)}>
              Open
            </Button>
          </dd>
        </dl>
      )}

      <h4 className="about__heading">Third-party software</h4>
      <ul className="about__licenses">
        {THIRD_PARTY_LICENSES.map((l) => (
          <li key={l.name}>
            <div className="about__lic-main">
              <span className="about__lic-name">{l.name}</span>
              <span className="about__lic-usage">{l.usage}</span>
            </div>
            <Badge>{l.license}</Badge>
            <button type="button" className="icon-btn icon-btn--sm" title={l.url} aria-label={`Open ${l.name} website`} onClick={() => void api.app.openExternal(l.url)}>
              <ExternalLink size={13} />
            </button>
          </li>
        ))}
      </ul>
      <p className="about__note">
        Session import and feature set are modelled on PuTTY by Simon Tatham (MIT licence). SSHTermaq is not affiliated with the PuTTY project.
      </p>
    </div>
  );
}
