/** Remote permissions editor (owner / group / others read-write-execute, special bits, octal). */
import { useState } from 'react';
import { Button, Checkbox, Modal } from '@/components/ui';
import type { FileItem } from './paths';

export function ChmodDialog({ item, onClose, onApply }: { item: FileItem; onClose: () => void; onApply: (mode: number) => void }) {
  const [mode, setMode] = useState((item.mode ?? 0o644) & 0o7777);
  const bit = (b: number) => (mode & b) !== 0;
  const toggle = (b: number) => setMode((m) => m ^ b);
  const rows: [string, number][] = [
    ['Owner', 6],
    ['Group', 3],
    ['Others', 0]
  ];
  return (
    <Modal
      open
      title={`Permissions — ${item.name}`}
      onClose={onClose}
      width={400}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" onClick={() => onApply(mode)}>
            Apply
          </Button>
        </>
      }
    >
      <div className="chmod">
        <div className="chmod__head">
          <span />
          <span>Read</span>
          <span>Write</span>
          <span>Execute</span>
        </div>
        {rows.map(([label, shift]) => (
          <div key={label} className="chmod__row">
            <span>{label}</span>
            {[4, 2, 1].map((b) => (
              <Checkbox key={b} checked={bit(b << shift)} onChange={() => toggle(b << shift)} />
            ))}
          </div>
        ))}
        <div className="chmod__special">
          <Checkbox checked={bit(0o4000)} onChange={() => toggle(0o4000)} label="Set user ID" />
          <Checkbox checked={bit(0o2000)} onChange={() => toggle(0o2000)} label="Set group ID" />
          <Checkbox checked={bit(0o1000)} onChange={() => toggle(0o1000)} label="Sticky" />
        </div>
        <label className="chmod__octal">
          Octal
          <input
            className="input mono"
            value={mode.toString(8).padStart(4, '0')}
            onChange={(e) => {
              const v = parseInt(e.target.value || '0', 8);
              if (!Number.isNaN(v) && v <= 0o7777) setMode(v);
            }}
          />
        </label>
      </div>
    </Modal>
  );
}
