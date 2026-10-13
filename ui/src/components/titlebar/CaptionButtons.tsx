/**
 * Window caption buttons drawn in HTML (the host window has no native caption). Windows 11 metrics:
 * 46px wide buttons, 10px glyphs with 1px strokes; close turns red on hover.
 */
import { api } from '@/bridge/api';
import { useUi } from '@/store/ui';

function Glyph({ kind }: { kind: 'min' | 'max' | 'restore' | 'close' | 'unfullscreen' }) {
  const common = { width: 10, height: 10, viewBox: '0 0 10 10', fill: 'none', stroke: 'currentColor', strokeWidth: 1, 'aria-hidden': true } as const;
  switch (kind) {
    case 'min':
      return (
        <svg {...common}>
          <path d="M0 5.5h10" />
        </svg>
      );
    case 'max':
      return (
        <svg {...common}>
          <rect x="0.5" y="0.5" width="9" height="9" rx="1.2" />
        </svg>
      );
    case 'restore':
      return (
        <svg {...common}>
          <rect x="0.5" y="2.5" width="7" height="7" rx="1.1" />
          <path d="M2.5 2.5V1.6c0-.6.5-1.1 1.1-1.1h4.8c.6 0 1.1.5 1.1 1.1v4.8c0 .6-.5 1.1-1.1 1.1h-.9" />
        </svg>
      );
    case 'unfullscreen':
      return (
        <svg {...common}>
          <path d="M3.5 0.5v3h-3M6.5 0.5v3h3M3.5 9.5v-3h-3M6.5 9.5v-3h3" />
        </svg>
      );
    case 'close':
      return (
        <svg {...common}>
          <path d="M0.5 0.5l9 9M9.5 0.5l-9 9" />
        </svg>
      );
  }
}

export function CaptionButtons() {
  const maximized = useUi((s) => s.maximized);
  const fullscreen = useUi((s) => s.fullscreen);
  if (fullscreen) {
    return (
      <div className="caption no-drag">
        <button className="caption__btn" title="Exit full screen" aria-label="Exit full screen" onClick={() => api.app.toggleFullscreen()}>
          <Glyph kind="unfullscreen" />
        </button>
      </div>
    );
  }
  return (
    <div className="caption no-drag">
      <button className="caption__btn" title="Minimize" aria-label="Minimize" tabIndex={-1} onClick={() => api.window.minimize()}>
        <Glyph kind="min" />
      </button>
      <button
        className="caption__btn"
        title={maximized ? 'Restore down' : 'Maximize'}
        aria-label={maximized ? 'Restore down' : 'Maximize'}
        tabIndex={-1}
        onClick={() => api.window.toggleMaximize()}
      >
        <Glyph kind={maximized ? 'restore' : 'max'} />
      </button>
      <button className="caption__btn caption__btn--close" title="Close" aria-label="Close" tabIndex={-1} onClick={() => api.window.close()}>
        <Glyph kind="close" />
      </button>
    </div>
  );
}
