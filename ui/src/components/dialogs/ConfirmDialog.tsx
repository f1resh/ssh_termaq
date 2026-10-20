/**
 * Generic confirmation dialog (useUi dialog type 'confirm', see confirmDialog() in store/ui.ts).
 */
import { AlertTriangle, HelpCircle } from 'lucide-react';
import { Button, Modal } from '@/components/ui';
import type { DialogState } from '@/store/ui';
import { useInitialFocus, useStableCallback } from './common/hooks';

export type ConfirmDialogState = Extract<DialogState, { type: 'confirm' }>;

export interface ConfirmDialogProps {
  dialog: ConfirmDialogState;
  /** true = confirmed, false = cancelled / dismissed. */
  onResolve: (confirmed: boolean) => void;
}

export function ConfirmDialog({ dialog, onResolve }: ConfirmDialogProps) {
  const cancel = useStableCallback(() => onResolve(false));
  // Danger confirmations focus the safe choice by default.
  const cancelRef = useInitialFocus<HTMLButtonElement>(!!dialog.danger);
  const okRef = useInitialFocus<HTMLButtonElement>(!dialog.danger);
  return (
    <Modal
      open
      title={dialog.title}
      onClose={cancel}
      width={460}
      icon={
        dialog.danger ? (
          <AlertTriangle size={18} className="dlg-title-icon dlg-title-icon--danger" />
        ) : (
          <HelpCircle size={18} className="dlg-title-icon" />
        )
      }
      footer={
        <>
          <Button ref={cancelRef} onClick={cancel}>
            Cancel
          </Button>
          <Button ref={okRef} variant={dialog.danger ? 'danger' : 'primary'} onClick={() => onResolve(true)}>
            {dialog.confirmLabel ?? 'OK'}
          </Button>
        </>
      }
    >
      <div className="dlg-confirm__message">{dialog.message}</div>
    </Modal>
  );
}
