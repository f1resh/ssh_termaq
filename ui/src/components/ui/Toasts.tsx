import { create } from 'zustand';
import { X } from 'lucide-react';
import type { ReactNode } from 'react';

export interface Toast {
  id: number;
  kind: 'info' | 'success' | 'warning' | 'error';
  title: string;
  message?: ReactNode;
  /** ms; 0 = sticky */
  timeout: number;
  action?: { label: string; onClick: () => void };
}

interface ToastState {
  toasts: Toast[];
  push(t: Omit<Toast, 'id' | 'timeout'> & { timeout?: number }): number;
  dismiss(id: number): void;
}

let nextId = 1;

const useToasts = create<ToastState>((set, get) => ({
  toasts: [],
  push(t) {
    const id = nextId++;
    const toast: Toast = { timeout: t.kind === 'error' ? 8000 : 4000, ...t, id };
    set({ toasts: [...get().toasts, toast].slice(-5) });
    if (toast.timeout > 0) setTimeout(() => get().dismiss(id), toast.timeout);
    return id;
  },
  dismiss(id) {
    set({ toasts: get().toasts.filter((x) => x.id !== id) });
  }
}));

export const toast = {
  info: (title: string, message?: ReactNode) => useToasts.getState().push({ kind: 'info', title, message }),
  success: (title: string, message?: ReactNode) => useToasts.getState().push({ kind: 'success', title, message }),
  warning: (title: string, message?: ReactNode) => useToasts.getState().push({ kind: 'warning', title, message }),
  error: (title: string, message?: ReactNode) => useToasts.getState().push({ kind: 'error', title, message }),
  show: (t: Omit<Toast, 'id' | 'timeout'> & { timeout?: number }) => useToasts.getState().push(t),
  dismiss: (id: number) => useToasts.getState().dismiss(id)
};

export function ToastHost() {
  const toasts = useToasts((s) => s.toasts);
  const dismiss = useToasts((s) => s.dismiss);
  return (
    <div className="toasts" role="status" aria-live="polite">
      {toasts.map((t) => (
        <div key={t.id} className={`toast toast--${t.kind}`}>
          <div className="toast__body">
            <div className="toast__title">{t.title}</div>
            {t.message && <div className="toast__message">{t.message}</div>}
            {t.action && (
              <button
                className="btn btn--sm"
                style={{ marginTop: 6 }}
                onClick={() => {
                  t.action!.onClick();
                  dismiss(t.id);
                }}
              >
                {t.action.label}
              </button>
            )}
          </div>
          <button className="icon-btn icon-btn--sm" aria-label="Dismiss" onClick={() => dismiss(t.id)}>
            <X size={14} />
          </button>
        </div>
      ))}
    </div>
  );
}
