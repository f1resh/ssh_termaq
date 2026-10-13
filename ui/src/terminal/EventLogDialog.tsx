/**
 * PuTTY-style Event Log viewer for a pane's connection (live updating).
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { Copy, Download } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Modal, toast } from '@/components/ui';
import { useWorkspace, paneTitle } from '@/store/workspace';
import type { EventLogEntry } from '@shared/types';
import { followEventLog } from './connEvents';

export interface EventLogDialogProps {
  paneId: string;
  onClose: () => void;
}

const fmt = (t: number) => {
  const d = new Date(t);
  const p = (n: number, w = 2) => String(n).padStart(w, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
};

export function EventLogDialog({ paneId, onClose }: EventLogDialogProps) {
  const pane = useWorkspace((s) => s.panes[paneId]);
  const connId = pane?.connId ?? '';
  const [entries, setEntries] = useState<EventLogEntry[]>([]);
  const listRef = useRef<HTMLDivElement>(null);
  const stick = useRef(true);

  useEffect(() => (connId ? followEventLog(connId, setEntries) : undefined), [connId]);

  useEffect(() => {
    const el = listRef.current;
    if (el && stick.current) el.scrollTop = el.scrollHeight;
  }, [entries]);

  const text = useMemo(() => entries.map((e) => `${fmt(e.time)}\t${e.message}`).join('\n'), [entries]);

  return (
    <Modal
      open
      title={`Event Log — ${pane ? paneTitle(pane) : 'session'}`}
      onClose={onClose}
      width={820}
      height={560}
      bodyClassName="eventlog__body"
      footer={
        <>
          <span className="muted" style={{ marginRight: 'auto' }}>
            {entries.length} entries
          </span>
          <Button
            icon={<Copy size={14} />}
            onClick={() => {
              api.app.writeClipboard(text);
              toast.success('Event log copied');
            }}
          >
            Copy
          </Button>
          <Button
            icon={<Download size={14} />}
            onClick={() =>
              void api.dialog.saveTextFile(text + '\n', {
                title: 'Save event log',
                defaultPath: 'event-log.txt',
                filters: [{ name: 'Text files', extensions: ['txt', 'log'] }]
              })
            }
          >
            Save…
          </Button>
          <Button variant="primary" onClick={onClose}>
            Close
          </Button>
        </>
      }
    >
      <div
        ref={listRef}
        className="eventlog"
        onScroll={(e) => {
          const el = e.currentTarget;
          stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
        }}
      >
        {entries.length === 0 && <div className="muted eventlog__empty">No events yet.</div>}
        {entries.map((e, i) => (
          <div key={i} className={`eventlog__row eventlog__row--${e.level}`}>
            <span className="eventlog__time">{fmt(e.time)}</span>
            <span className="eventlog__msg">{e.message}</span>
          </div>
        ))}
      </div>
    </Modal>
  );
}
