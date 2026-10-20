/**
 * Renders the dialog selected in useUi().dialog plus the global prompt dialogs raised by the host
 * (usePrompts: 'hostkey' -> HostKeyDialog, 'confirm' -> PromptConfirmDialog). Authentication prompts
 * (username / password / kbdint / passphrase) are rendered inside their panes by the terminal module.
 */
import { useCallback } from 'react';
import { EventLogDialog } from '@/terminal/EventLogDialog';
import { QuickConnectDialog } from '@/components/quickconnect/QuickConnectDialog';
import { usePrompts } from '@/store/data';
import { useUi, type DialogState } from '@/store/ui';
import { AboutDialog } from './AboutDialog';
import { ConfirmDialog } from './ConfirmDialog';
import { HostKeyDialog, type HostKeyChoice, type HostKeyPrompt } from './HostKeyDialog';
import { InputDialog } from './InputDialog';
import { PromptConfirmDialog, type ConfirmPrompt } from './PromptConfirmDialog';
import { ImportDialog } from './import/ImportDialog';
import { KeygenDialog } from './keygen/KeygenDialog';
import { SessionEditor } from './session/SessionEditor';
import { SettingsDialog } from './settings/SettingsDialog';
import './dialogs.css';

/** Stable per-object keys so a newly opened dialog of the same type always starts with fresh state. */
const dialogKeys = new WeakMap<object, number>();
let nextDialogKey = 1;
function keyOf(d: object): number {
  let k = dialogKeys.get(d);
  if (k === undefined) {
    k = nextDialogKey++;
    dialogKeys.set(d, k);
  }
  return k;
}

/** Closes `d` unless a callback already replaced it with another dialog. */
function closeIfCurrent(d: DialogState): void {
  const ui = useUi.getState();
  if (ui.dialog === d) ui.closeDialog();
}

export function DialogHost() {
  const dialog = useUi((s) => s.dialog);
  const prompt = usePrompts((s) => s.pending.find((p) => p.kind === 'hostkey' || p.kind === 'confirm'));

  return (
    <>
      {dialog && <DialogSwitch key={keyOf(dialog)} dialog={dialog} />}
      {prompt?.kind === 'hostkey' && <HostKeyPromptHost key={prompt.id} prompt={prompt} />}
      {prompt?.kind === 'confirm' && <ConfirmPromptHost key={prompt.id} prompt={prompt} />}
    </>
  );
}

function DialogSwitch({ dialog }: { dialog: DialogState }) {
  const close = useCallback(() => closeIfCurrent(dialog), [dialog]);

  switch (dialog.type) {
    case 'sessionEditor':
      return <SessionEditor dialog={dialog} onClose={close} />;
    case 'settings':
      return <SettingsDialog initialSection={dialog.section} onClose={close} />;
    case 'import':
      return <ImportDialog source={dialog.source} onClose={close} />;
    case 'keygen':
      return <KeygenDialog onClose={close} />;
    case 'about':
      return <AboutDialog onClose={close} />;
    case 'eventLog':
      return <EventLogDialog paneId={dialog.paneId} onClose={close} />;
    case 'quickConnect':
      return <QuickConnectDialog initial={dialog.initial} onClose={close} />;
    case 'confirm':
      return (
        <ConfirmDialog
          dialog={dialog}
          onResolve={(ok) => {
            // Run the callback first: it may open a follow-up dialog, which must not be closed here.
            if (ok) dialog.onConfirm();
            else dialog.onCancel?.();
            closeIfCurrent(dialog);
          }}
        />
      );
    case 'input':
      return (
        <InputDialog
          dialog={dialog}
          onSubmit={(v) => {
            // inputDialog() resolves null when the dialog closes before onSubmit ran, so submit first.
            dialog.onSubmit(v);
            closeIfCurrent(dialog);
          }}
          onCancel={close}
        />
      );
    default:
      return null;
  }
}

function HostKeyPromptHost({ prompt }: { prompt: HostKeyPrompt }) {
  const onChoice = (choice: HostKeyChoice) =>
    usePrompts.getState().respond(prompt.id, { cancelled: choice === 'reject', choice });
  return <HostKeyDialog prompt={prompt} onChoice={onChoice} />;
}

function ConfirmPromptHost({ prompt }: { prompt: ConfirmPrompt }) {
  const onChoice = (label: string | null) =>
    usePrompts.getState().respond(prompt.id, label === null ? { cancelled: true } : { cancelled: false, choice: label });
  return <PromptConfirmDialog prompt={prompt} onChoice={onChoice} />;
}
