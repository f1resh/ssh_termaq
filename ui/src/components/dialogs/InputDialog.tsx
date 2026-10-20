/**
 * Single-line text input dialog (useUi dialog type 'input', see inputDialog() in store/ui.ts).
 */
import { useId, useState } from 'react';
import { Button, Modal, TextInput } from '@/components/ui';
import type { DialogState } from '@/store/ui';
import { FormField } from './common/controls';
import { useStableCallback } from './common/hooks';

export type InputDialogState = Extract<DialogState, { type: 'input' }>;

export interface InputDialogProps {
  dialog: InputDialogState;
  onSubmit: (value: string) => void;
  onCancel: () => void;
}

export function InputDialog({ dialog, onSubmit, onCancel }: InputDialogProps) {
  const [value, setValue] = useState(dialog.initial ?? '');
  const [touched, setTouched] = useState(false);
  const id = useId();
  const error = dialog.validate ? dialog.validate(value) : null;
  const cancel = useStableCallback(onCancel);

  const submit = () => {
    setTouched(true);
    if (error) return;
    onSubmit(value);
  };

  return (
    <Modal
      open
      title={dialog.title}
      onClose={cancel}
      width={440}
      footer={
        <>
          <Button onClick={cancel}>Cancel</Button>
          <Button variant="primary" onClick={submit} disabled={touched && !!error}>
            {dialog.confirmLabel ?? 'OK'}
          </Button>
        </>
      }
    >
      <form
        className="dlg-input-form"
        onSubmit={(e) => {
          e.preventDefault();
          submit();
        }}
      >
        <FormField label={dialog.label} error={touched ? error : null} htmlFor={id}>
          <TextInput
            id={id}
            value={value}
            onChange={(v) => {
              setValue(v);
              setTouched(true);
            }}
            placeholder={dialog.placeholder}
            invalid={touched && !!error}
            onFocus={(e) => e.currentTarget.select()}
          />
        </FormField>
      </form>
    </Modal>
  );
}
