// SSHTermaq site: download links from the latest GitHub release, the visitor's own system first, videos that play
// only while visible. Everything works without it: the buttons then lead to the Releases page.
(() => {
  const RELEASES = 'https://github.com/f1resh/sshtermaq/releases/latest';
  const API = 'https://api.github.com/repos/f1resh/sshtermaq/releases/latest';

  // Release files, by the end of their name (the name carries the version: SSHTermaq-1.0.0-setup.exe).
  const ASSETS = {
    'win-setup': /-setup\.exe$/,
    'win-zip': /-win-x64\.zip$/,
    'mac-arm64-dmg': /-mac-arm64\.dmg$/,
    'mac-arm64-zip': /-mac-arm64\.zip$/,
    'mac-x64-dmg': /-mac-x64\.dmg$/,
    'mac-x64-zip': /-mac-x64\.zip$/,
  };
  const LABELS = {
    'win-setup': ['Download for Windows', 'Installer · Windows 10 and 11'],
    'mac-arm64-dmg': ['Download for Mac', 'Apple silicon · macOS 12+'],
    'mac-x64-dmg': ['Download for Mac', 'Intel · macOS 12+'],
  };

  const megabytes = (bytes) => `${(bytes / 1048576).toFixed(1)} MB`;

  // ---- which system is this? ------------------------------------------------------------------------------------
  async function detect() {
    const uaData = navigator.userAgentData;
    const platform = (uaData && uaData.platform) || navigator.platform || '';
    const ua = navigator.userAgent;
    if (/win/i.test(platform) || /Windows/.test(ua)) return 'win-setup';
    if (!/mac/i.test(platform) && !/Mac OS X/.test(ua)) return null;
    if (/iPhone|iPad/.test(ua) || navigator.maxTouchPoints > 1) return null;  // iPadOS reports itself as a Mac
    // Chromium browsers say which processor; Safari and Firefox don't, so ask the graphics driver.
    try {
      if (uaData && uaData.getHighEntropyValues) {
        const { architecture } = await uaData.getHighEntropyValues(['architecture']);
        if (architecture) return architecture === 'arm' ? 'mac-arm64-dmg' : 'mac-x64-dmg';
      }
    } catch { /* not allowed: fall through */ }
    try {
      const gl = document.createElement('canvas').getContext('webgl');
      const info = gl && gl.getExtension('WEBGL_debug_renderer_info');
      const renderer = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : '';
      if (/Intel|AMD|Radeon/i.test(renderer)) return 'mac-x64-dmg';
    } catch { /* no WebGL */ }
    return 'mac-arm64-dmg';  // every Mac sold since 2023
  }

  // ---- links ----------------------------------------------------------------------------------------------------
  async function release() {
    try {
      const response = await fetch(API, { headers: { Accept: 'application/vnd.github+json' } });
      if (!response.ok) return null;
      return await response.json();
    } catch {
      return null;
    }
  }

  async function main() {
    const [mine, latest] = await Promise.all([detect(), release()]);
    const urls = {};
    if (latest && Array.isArray(latest.assets)) {
      for (const [key, pattern] of Object.entries(ASSETS)) {
        const asset = latest.assets.find((a) => pattern.test(a.name));
        if (asset) urls[key] = { href: asset.browser_download_url, size: asset.size };
      }
      const version = (latest.tag_name || '').replace(/^v/, '');
      if (version) {
        document.getElementById('version').textContent = `Version ${version}`;
        document.getElementById('version-line').textContent = `Version ${version}.`;
      }
    }

    for (const link of document.querySelectorAll('[data-asset]')) {
      const found = urls[link.dataset.asset];
      if (!found) continue;
      link.href = found.href;
      link.title = megabytes(found.size);
    }

    if (mine) {
      const primary = document.getElementById('primary-download');
      document.getElementById('primary-label').textContent = LABELS[mine][0];
      document.getElementById('primary-detail').textContent = LABELS[mine][1];
      if (urls[mine]) primary.href = urls[mine].href;
      const card = document.getElementById(mine.startsWith('win') ? 'platform-windows' : 'platform-mac');
      card.classList.add('is-yours');
      if (mine === 'mac-x64-dmg') {  // the Intel download becomes the main button on an Intel Mac
        const [arm, intel] = card.querySelectorAll('.button');
        arm.classList.replace('button--primary', 'button--ghost');
        intel.classList.replace('button--ghost', 'button--primary');
        intel.after(arm);
      }
    } else {
      document.getElementById('primary-download').href = '#download';
    }
  }

  // ---- videos ---------------------------------------------------------------------------------------------------
  function videos() {
    const list = document.querySelectorAll('video[data-play-in-view]');
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      for (const video of list) {
        video.pause();
        video.removeAttribute('autoplay');
        video.controls = true;
      }
      return;
    }
    if (!('IntersectionObserver' in window)) return;
    const observer = new IntersectionObserver((entries) => {
      for (const { target, isIntersecting } of entries) {
        if (isIntersecting) target.play().catch(() => { target.controls = true; });
        else target.pause();
      }
    }, { threshold: 0.35 });
    for (const video of list) observer.observe(video);
  }

  // ---- copy buttons ---------------------------------------------------------------------------------------------
  function copyButtons() {
    for (const button of document.querySelectorAll('button[data-copy]')) {
      button.addEventListener('click', async () => {
        try {
          await navigator.clipboard.writeText(button.dataset.copy);
          button.textContent = 'Copied';
          button.classList.add('is-done');
        } catch {
          // no clipboard access: select the command so the usual shortcut copies it
          const code = button.parentElement.querySelector('code');
          const range = document.createRange();
          range.selectNodeContents(code);
          window.getSelection().removeAllRanges();
          window.getSelection().addRange(range);
          button.textContent = /Mac/.test(navigator.platform) ? 'Press ⌘C' : 'Press Ctrl+C';
        }
        setTimeout(() => {
          button.textContent = 'Copy';
          button.classList.remove('is-done');
        }, 2000);
      });
    }
  }

  copyButtons();
  videos();
  main();
})();
