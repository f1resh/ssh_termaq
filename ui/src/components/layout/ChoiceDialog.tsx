/**
 * Small multi-choice modal used by the shell (e.g. "delete folder: keep or delete its sessions").
 * `choiceDialog(...)` resolves with the chosen value, or null when dismissed. Render <ChoiceDialogHost/> once.
 */
import type { ReactNode } from 'react';
import { create } from 'zustand';
import { Button, Modal } from '@/components/ui';

export interface Choice<T extends string> {
  value: T;
  label: string;
  variant?: 'default' | 'primary' | 'danger';
}

interface ChoiceRequest {
  title: string;
  message: ReactNode;
  icon?: ReactNode;
  choices: Choice<string>[];
  resolve: (v: string | null) => void;
}

const useChoice = create<{ req: ChoiceRequest | null }>(() => ({ req: null }));

export function choiceDialog<T extends string>(opts: {
  title: string;
  message: ReactNode;
  icon?: ReactNode;
  choices: Choice<T>[];
}): Promise<T | null> {
  return new Promise((resolve) => {
    // A newer request supersedes (and cancels) an older one.
    useChoice.getState().req?.resolve(null);
    useChoice.setState({
      req: { ...opts, resolve: (v) => resolve(v as T | null) }
    });
  });
}

export function ChoiceDialogHost() {
  const req = useChoice((s) => s.req);
  if (!req) return null;
  const finish = (v: string | null) => {
    useChoice.setState({ req: null });
    req.resolve(v);
  };
  return (
    <Modal
      open
      title={req.title}
      icon={req.icon}
      onClose={() => finish(null)}
      width={460}
      footer={
        <>
          <Button onClick={() => finish(null)}>Cancel</Button>
          {req.choices.map((c) => (
            <Button key={c.value} variant={c.variant ?? 'default'} onClick={() => finish(c.value)}>
              {c.label}
            </Button>
          ))}
        </>
      }
    >
      <div className="choice-dialog__message">{req.message}</div>
    </Modal>
  );
}
