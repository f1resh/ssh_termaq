/**
 * The OS the UI runs on, known before anything renders: the macOS host (WKWebView, window.webkit.messageHandlers.st)
 * or the Windows host (WebView2, window.chrome.webview). In a plain browser (dev server, mock backend) a Mac browser
 * counts as macOS, anything else as Windows, so both layouts can be looked at without the host. app:info's `platform`
 * names the OS for display.
 */
export type HostPlatform = 'win32' | 'darwin';

export function detectPlatform(g: unknown = globalThis): HostPlatform {
  const w = g as {
    webkit?: { messageHandlers?: { st?: unknown } };
    chrome?: { webview?: unknown };
    navigator?: { platform?: string; userAgent?: string };
  };
  if (w.webkit?.messageHandlers?.st) return 'darwin';
  if (w.chrome?.webview) return 'win32';
  return /\bMac/.test(`${w.navigator?.platform ?? ''} ${w.navigator?.userAgent ?? ''}`) ? 'darwin' : 'win32';
}

export const hostPlatform: HostPlatform = detectPlatform();
export const isMac = hostPlatform === 'darwin';

/**
 * The OS's own words in labels: the local side, its file browser, where local deletes go, and a serial line as
 * examples name it (alone, and as quick-connect input with a baud rate).
 */
export const osTerms = isMac
  ? {
      computer: 'Computer',
      ThisComputer: 'This Mac',
      thisComputer: 'this Mac',
      fileBrowser: 'Finder',
      trash: 'Trash',
      serialLine: '/dev/cu.usbserial-1410',
      serialExample: '/dev/cu.usbserial-1410?baud=115200'
    }
  : {
      computer: 'This PC',
      ThisComputer: 'This PC',
      thisComputer: 'this PC',
      fileBrowser: 'Explorer',
      trash: 'Recycle Bin',
      serialLine: 'COM3',
      serialExample: 'serial://COM3?baud=115200'
    };
