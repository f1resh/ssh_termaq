/**
 * `api` — the UI's single entry point to the native host.
 *
 * Inside the SSHTermaq host messages go over window.chrome.webview (Windows, WebView2) or
 * window.webkit.messageHandlers.st (macOS, WKWebView) using the wire format documented in src/shared/ipc.ts. In a plain browser (vite dev server / tests) a mock backend is used so
 * the UI can be developed and previewed without the native host.
 */
import { API_SCHEMA, channelOf, type Api, type Unsubscribe } from '@shared/ipc';
import { createMockBackend } from './mock';

export interface Transport {
  invoke(channel: string, args: unknown[]): Promise<unknown>;
  send(channel: string, args: unknown[]): void;
  subscribe(channel: string, cb: (...args: unknown[]) => void): Unsubscribe;
  pathsForFiles(files: File[] | FileList): Promise<string[]>;
}

interface WebViewLike {
  postMessage(message: unknown): void;
  postMessageWithAdditionalObjects?(message: unknown, additionalObjects: ArrayLike<unknown>): void;
  addEventListener(type: 'message', listener: (ev: { data: unknown }) => void): void;
}

type WireMessage =
  | { t: 'res'; id: number; ok: true; result: unknown }
  | { t: 'res'; id: number; ok: false; error: string }
  | { t: 'evt'; ch: string; args: unknown[] };

/**
 * The macOS host (platform/mac/ui/HostWindowMac.mm) as a WebViewLike: messages to it are JSON text for the "st"
 * message handler; its messages arrive in batches, as a JSON array string, through window.__stReceive.
 */
function webKitWebView(): WebViewLike | undefined {
  const g = globalThis as unknown as {
    webkit?: { messageHandlers?: { st?: { postMessage(message: string): void } } };
    __stReceive?: (batch: string) => void;
  };
  const st = g.webkit?.messageHandlers?.st;
  if (!st) return undefined;
  const listeners: ((ev: { data: unknown }) => void)[] = [];
  g.__stReceive = (batch) => {
    for (const data of JSON.parse(batch) as unknown[]) for (const listener of listeners) listener({ data });
  };
  return {
    postMessage: (message) => st.postMessage(JSON.stringify(message)),
    // A File has no path in WebKit: the host finds the dropped files by name (src/shared/ipc.ts).
    postMessageWithAdditionalObjects: (message, files) =>
      st.postMessage(JSON.stringify({ ...(message as object), args: [Array.from(files as ArrayLike<File>, (f) => f.name)] })),
    addEventListener: (_type, listener) => void listeners.push(listener)
  };
}

let hostWebView: WebViewLike | null | undefined;  // undefined: not looked up yet; null: no host (browser)

/** The host's message channel, looked up once (the macOS adapter owns window.__stReceive). */
function getWebView(): WebViewLike | undefined {
  if (hostWebView === undefined)
    hostWebView = (globalThis as unknown as { chrome?: { webview?: WebViewLike } }).chrome?.webview ?? webKitWebView() ?? null;
  return hostWebView ?? undefined;
}

export function isNativeHost(): boolean {
  return getWebView() !== undefined;
}

function createWebViewTransport(webview: WebViewLike): Transport {
  let nextId = 1;
  const pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();
  const listeners = new Map<string, Set<(...args: unknown[]) => void>>();

  webview.addEventListener('message', (ev) => {
    let msg = ev.data as WireMessage | string;
    if (typeof msg === 'string') {
      try {
        msg = JSON.parse(msg) as WireMessage;
      } catch {
        return;
      }
    }
    if (!msg || typeof msg !== 'object') return;
    if (msg.t === 'res') {
      const p = pending.get(msg.id);
      if (!p) return;
      pending.delete(msg.id);
      if (msg.ok) p.resolve(msg.result ?? undefined);
      else p.reject(new Error(msg.error || 'Unknown error'));
    } else if (msg.t === 'evt') {
      const set = listeners.get(msg.ch);
      if (!set) return;
      for (const cb of [...set]) {
        try {
          cb(...(msg.args ?? []));
        } catch (err) {
          console.error(`listener for ${msg.ch} failed`, err);
        }
      }
    }
  });

  const invoke = (channel: string, args: unknown[]) =>
    new Promise<unknown>((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      webview.postMessage({ t: 'req', id, ch: channel, args });
    });

  return {
    invoke,
    send(channel, args) {
      webview.postMessage({ t: 'send', ch: channel, args });
    },
    subscribe(channel, cb) {
      let set = listeners.get(channel);
      if (!set) listeners.set(channel, (set = new Set()));
      set.add(cb);
      return () => {
        set!.delete(cb);
      };
    },
    pathsForFiles(files) {
      const list = Array.from(files as ArrayLike<File>);
      if (!list.length) return Promise.resolve([]);
      if (!webview.postMessageWithAdditionalObjects) return Promise.resolve([]);
      return new Promise<string[]>((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve: (v) => resolve((v as string[]) ?? []), reject });
        webview.postMessageWithAdditionalObjects!({ t: 'req', id, ch: 'files:pathsForFiles', args: [] }, list);
      });
    }
  };
}

/** Strip trailing undefined args so optional parameters serialize cleanly. */
function cleanArgs(args: unknown[]): unknown[] {
  let end = args.length;
  while (end > 0 && args[end - 1] === undefined) end--;
  return args.slice(0, end).map((a) => (a === undefined ? null : a));
}

export function buildApi(transport: Transport): Api {
  const api: Record<string, Record<string, unknown>> = {};
  for (const [ns, methods] of Object.entries(API_SCHEMA)) {
    const target: Record<string, unknown> = {};
    for (const [method, kind] of Object.entries(methods as Record<string, string>)) {
      const ch = channelOf(ns, method);
      switch (kind) {
        case 'invoke':
          target[method] = (...args: unknown[]) => transport.invoke(ch, cleanArgs(args));
          break;
        case 'send':
          target[method] = (...args: unknown[]) => transport.send(ch, cleanArgs(args));
          break;
        case 'event':
          target[method] = (cb: (...args: unknown[]) => void) => transport.subscribe(ch, cb);
          break;
        case 'local':
          if (ns === 'files' && method === 'pathsForFiles') {
            target[method] = (files: File[] | FileList) => transport.pathsForFiles(files);
          }
          break;
      }
    }
    api[ns] = target;
  }
  return api as unknown as Api;
}

const webview = getWebView();
export const transport: Transport = webview ? createWebViewTransport(webview) : createMockBackend();
export const api: Api = buildApi(transport);
