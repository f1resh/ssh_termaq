/**
 * Serial line + baud rate inputs (shared by the Session and Serial sections).
 */
import { useEffect, useMemo, useState } from 'react';
import { RefreshCw } from 'lucide-react';
import { api } from '@/bridge/api';
import { IconButton, NumberInput, Select } from '@/components/ui';
import { BAUD_RATES } from '@shared/algorithms';
import type { SerialPortInfo } from '@shared/types';
import { ComboInput, type ComboOption } from '../../common/controls';
import { useEditor } from '../editorContext';

let cachedPorts: SerialPortInfo[] | null = null;

function useSerialPorts() {
  const [ports, setPorts] = useState<SerialPortInfo[]>(cachedPorts ?? []);
  const [loading, setLoading] = useState(false);
  const refresh = async () => {
    setLoading(true);
    try {
      const list = (await api.serial.listPorts()) ?? [];
      cachedPorts = list;
      setPorts(list);
    } catch {
      setPorts([]);
    } finally {
      setLoading(false);
    }
  };
  useEffect(() => {
    if (!cachedPorts) void refresh();
  }, []);
  return { ports, loading, refresh };
}

export function SerialLineInput({ id }: { id?: string }) {
  const { draft, update, fieldError } = useEditor();
  const { ports, loading, refresh } = useSerialPorts();
  const options = useMemo<ComboOption[]>(
    () => ports.map((p) => ({ value: p.path, hint: [p.friendlyName, p.manufacturer].filter(Boolean).join(' · ') || undefined })),
    [ports]
  );
  return (
    <div className="dlg-path">
      <ComboInput
        id={id}
        mono
        value={draft.serial.line}
        options={options}
        placeholder="COM1"
        invalid={!!fieldError('serial.line')}
        onChange={(v) => update((d) => void (d.serial.line = v))}
        emptyText={loading ? 'Looking for serial ports…' : 'No serial ports found'}
        filter={false}
      />
      <IconButton label="Refresh serial ports" onClick={() => void refresh()} disabled={loading}>
        <RefreshCw size={14} className={loading ? 'dlg-spin' : undefined} />
      </IconButton>
    </div>
  );
}

const CUSTOM = -1;

export function BaudRateInput() {
  const { draft, update } = useEditor();
  const rate = draft.serial.baudRate;
  const isStandard = (BAUD_RATES as readonly number[]).includes(rate);
  const [custom, setCustom] = useState(!isStandard);
  const options = [
    ...BAUD_RATES.map((b) => ({ value: b as number, label: b.toLocaleString('en-US') })),
    { value: CUSTOM, label: 'Custom…' }
  ];
  return (
    <div className="dlg-path">
      <Select<number>
        value={custom ? CUSTOM : rate}
        options={options}
        onChange={(v) => {
          if (v === CUSTOM) setCustom(true);
          else {
            setCustom(false);
            update((d) => void (d.serial.baudRate = v));
          }
        }}
        aria-label="Baud rate"
      />
      {custom && (
        <NumberInput value={rate} min={50} max={4000000} onChange={(n) => update((d) => void (d.serial.baudRate = n))} aria-label="Custom baud rate" />
      )}
    </div>
  );
}
