/**
 * Confirmation layered on top of an open dialog (useUi holds a single dialog, so the global confirm dialog
 * cannot be used from inside another dialog). Render `element` inside the dialog's CloseGuardContext so that
 * Escape / close requests dismiss the confirmation first.
 */
import { useCallback, useRef, useState, type ReactNode } from 'react';
import { AlertTriangle, HelpCircle } from 'lucide-react';
import { Button, Modal } from '@/components/ui';
import { useCloseGuard, useInitialFocus, useStableCallback } from './hooks';

export interface NestedConfirmOptions {
  title: string;
  message: ReactNode;
  confirmLabel?: string;
  cancelLabel?: string;
  /** Optional third button; resolves 'alt'. */
  altLabel?: string;
  danger?: boolean;
}

export type NestedConfirmResult = 'confirm' | 'cancel' | 'alt';

export function useNestedConfirm(): {
  element: ReactNode;
  ask: (o: NestedConfirmOptions) => Promise<NestedConfirmResult>;
  confirm: (o: NestedConfirmOptions) => Promise<boolean>;
  isOpen: boolean;
} {
  const [opts, setOpts] = useState<NestedConfirmOptions | null>(null);
  const resolver = useRef<((r: NestedConfirmResult) => void) | null>(null);

  const ask = useCallback((o: NestedConfirmOptions) => {
    resolver.current?.('cancel');
    return new Promise<NestedConfirmResult>((resolve) => {
      resolver.current = resolve;
      setOpts(o);
    });
  }, []);
  const confirm = useCallback(async (o: NestedConfirmOptions) => (await ask(o)) === 'confirm', [ask]);

  const finish = useStableCallback((r: NestedConfirmResult) => {
    const res = resolver.current;
    resolver.current = null;
    setOpts(null);
    res?.(r);
  });

  const element = opts ? <NestedConfirmView opts={opts} onFinish={finish} /> : null;
  return { element, ask, confirm, isOpen: !!opts };
}

function NestedConfirmView({ opts, onFinish }: { opts: NestedConfirmOptions; onFinish: (r: NestedConfirmResult) => void }) {
  const cancel = useStableCallback(() => onFinish('cancel'));
  useCloseGuard(true, cancel);
  const primaryRef = useInitialFocus<HTMLButtonElement>();
  return (
    <Modal
      open
      title={opts.title}
      onClose={cancel}
      width={440}
      icon={
        opts.danger ? (
          <AlertTriangle size={18} className="dlg-title-icon dlg-title-icon--danger" />
        ) : (
          <HelpCircle size={18} className="dlg-title-icon" />
        )
      }
      footer={
        <>
          {opts.altLabel && (
            <Button onClick={() => onFinish('alt')} className="dlg-footer__left">
              {opts.altLabel}
            </Button>
          )}
          <Button onClick={cancel}>{opts.cancelLabel ?? 'Cancel'}</Button>
          <Button ref={primaryRef} variant={opts.danger ? 'danger' : 'primary'} onClick={() => onFinish('confirm')}>
            {opts.confirmLabel ?? 'OK'}
          </Button>
        </>
      }
    >
      <div className="dlg-confirm__message">{opts.message}</div>
    </Modal>
  );
}
