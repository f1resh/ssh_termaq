/**
 * Host-initiated confirmation (global prompt kind 'confirm', e.g. "accept weak cipher?").
 * The first button is the primary choice (rendered rightmost); response.choice = the clicked label.
 */
import { AlertTriangle, HelpCircle } from 'lucide-react';
import { Button, Modal } from '@/components/ui';
import type { PromptRequest } from '@shared/types';
import { useInitialFocus, useStableCallback } from './common/hooks';

export type ConfirmPrompt = Extract<PromptRequest, { kind: 'confirm' }>;

export interface PromptConfirmDialogProps {
  prompt: ConfirmPrompt;
  /** label = chosen button; null = dismissed. */
  onChoice: (label: string | null) => void;
}

export function PromptConfirmDialog({ prompt, onChoice }: PromptConfirmDialogProps) {
  const buttons = prompt.buttons.length ? prompt.buttons : ['OK', 'Cancel'];
  const dismiss = useStableCallback(() => onChoice(null));
  // Danger prompts focus the safe (second) choice by default.
  const safeIndex = prompt.danger && buttons.length > 1 ? 1 : 0;
  const focusRef = useInitialFocus<HTMLButtonElement>();
  return (
    <Modal
      open
      dismissable={false}
      title={prompt.title}
      onClose={dismiss}
      width={500}
      icon={
        prompt.danger ? (
          <AlertTriangle size={18} className="dlg-title-icon dlg-title-icon--danger" />
        ) : (
          <HelpCircle size={18} className="dlg-title-icon" />
        )
      }
      footer={
        <>
          {buttons
            .map((label, i) => (
              <Button
                key={`${i}:${label}`}
                ref={i === safeIndex ? focusRef : undefined}
                variant={i === 0 ? (prompt.danger ? 'danger' : 'primary') : 'default'}
                onClick={() => onChoice(label)}
              >
                {label}
              </Button>
            ))
            .reverse()}
        </>
      }
    >
      <div className="dlg-confirm__message">{prompt.message}</div>
    </Modal>
  );
}
