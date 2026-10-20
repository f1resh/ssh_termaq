/**
 * macOS: tells the host where the title bar is and which parts of it move the window. WKWebView does not read CSS
 * app-region (WebView2 does, so Windows needs none of this); the host hit-tests these areas on mouse-down and centres
 * the traffic lights in the bar's height.
 */
import { useEffect, type RefObject } from 'react';
import { api } from '@/bridge/api';
import type { PageRect } from '@shared/ipc';
import { isMac } from '@shared/platform';

/** What does not move the window: anything interactive in the bar. */
const NO_DRAG = '.no-drag, button, input, select, textarea, a, [role="tab"], [role="button"]';

function rect(r: DOMRect): PageRect {
  return [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height)];
}

export function useTitleBarLayout(ref: RefObject<HTMLElement | null>): void {
  useEffect(() => {
    const bar = ref.current;
    if (!isMac || !bar) return;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let sent = '';
    const send = () => {
      timer = undefined;
      const box = bar.getBoundingClientRect();
      const noDrag = [...bar.querySelectorAll<HTMLElement>(NO_DRAG)]
        .map((el) => el.getBoundingClientRect())
        .filter((r) => r.width > 0 && r.height > 0)
        .map(rect);
      const layout = { height: Math.round(box.height), drag: [rect(box)], noDrag };
      const json = JSON.stringify(layout);
      if (json === sent) return;
      sent = json;
      api.window.setTitleBarLayout(layout);
    };
    // Tabs come and go, scroll and get dragged: measure again after any of it, at most once per frame. A timer, not
    // requestAnimationFrame: that does not run while the window is hidden, and the host needs the layout anyway.
    const schedule = () => {
      if (timer === undefined) timer = setTimeout(send, 16);
    };
    const resize = new ResizeObserver(schedule);
    resize.observe(bar);
    const mutation = new MutationObserver(schedule);
    mutation.observe(bar, { subtree: true, childList: true, attributes: true });
    bar.addEventListener('scroll', schedule, true);
    window.addEventListener('resize', schedule);
    schedule();
    return () => {
      resize.disconnect();
      mutation.disconnect();
      bar.removeEventListener('scroll', schedule, true);
      window.removeEventListener('resize', schedule);
      if (timer !== undefined) clearTimeout(timer);
    };
  }, [ref]);
}
