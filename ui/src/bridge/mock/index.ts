/**
 * In-browser mock of the native host, used when the UI runs outside WebView2 (vite dev server,
 * Playwright previews, unit tests). Each namespace registers handlers in its own file so features can
 * extend the mock independently. Unknown invoke channels resolve to null.
 */
import type { Transport } from '../api';
import { registerCoreMocks } from './core';
import { registerConnMocks } from './conn';
import { registerImporterMocks } from './importer';
import { registerKeysMocks } from './keys';
import { registerSftpMocks } from './sftp';

export type MockInvoke = (...args: any[]) => unknown;
export type MockSend = (...args: any[]) => void;

export interface MockContext {
  handle(channel: string, fn: MockInvoke): void;
  on(channel: string, fn: MockSend): void;
  emit(channel: string, ...args: unknown[]): void;
}

export function createMockBackend(): Transport {
  const invokes = new Map<string, MockInvoke>();
  const sends = new Map<string, MockSend>();
  const listeners = new Map<string, Set<(...args: unknown[]) => void>>();

  const ctx: MockContext = {
    handle: (ch, fn) => void invokes.set(ch, fn),
    on: (ch, fn) => void sends.set(ch, fn),
    emit(ch, ...args) {
      const set = listeners.get(ch);
      if (!set) return;
      // async like the real bridge
      queueMicrotask(() => {
        for (const cb of [...set]) cb(...args);
      });
    }
  };

  registerCoreMocks(ctx);
  registerConnMocks(ctx);
  registerSftpMocks(ctx);
  registerImporterMocks(ctx);
  registerKeysMocks(ctx);

  return {
    async invoke(channel, args) {
      const fn = invokes.get(channel);
      if (!fn) {
        console.debug('[mock] unhandled invoke', channel, args);
        return null;
      }
      await new Promise((r) => setTimeout(r, 5));
      return structuredClone(await fn(...args));
    },
    send(channel, args) {
      const fn = sends.get(channel);
      if (fn) fn(...args);
      else console.debug('[mock] unhandled send', channel, args);
    },
    subscribe(channel, cb) {
      let set = listeners.get(channel);
      if (!set) listeners.set(channel, (set = new Set()));
      set.add(cb);
      return () => void set!.delete(cb);
    },
    async pathsForFiles(files) {
      return Array.from(files as ArrayLike<File>).map((f) => `C:\\mock\\${f.name}`);
    }
  };
}
