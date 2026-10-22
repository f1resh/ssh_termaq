/**
 * Session editor: Telnet, Rlogin and Serial sections.
 */
import { Select, TextInput } from '@/components/ui';
import { osTerms } from '@shared/platform';
import type { FlowControl, Parity, SerialOptions } from '@shared/types';
import { FieldGrid, FormField, Section, SegmentedControl, SettingRow, ToggleRow, type SegmentOption } from '../../common/controls';
import { useEditor } from '../editorContext';
import { BaudRateInput, SerialLineInput } from './serialInputs';

/* -------------------------------------------------------------------------- */
/* Telnet                                                                     */
/* -------------------------------------------------------------------------- */

const NEGOTIATION: readonly SegmentOption<'active' | 'passive'>[] = [
  { value: 'active', label: 'Active', title: 'Start option negotiation immediately' },
  { value: 'passive', label: 'Passive', title: 'Wait for the server to negotiate first' }
];

export function TelnetSection() {
  const { draft, update } = useEditor();
  const t = draft.telnet;
  return (
    <Section title="Telnet protocol">
      <div className="dlg-rows">
        <SettingRow label="Option negotiation mode" description="Some servers misbehave unless the client waits for them (passive).">
          <SegmentedControl
            size="sm"
            value={t.passive ? 'passive' : 'active'}
            options={NEGOTIATION}
            onChange={(v) => update((d) => void (d.telnet.passive = v === 'passive'))}
          />
        </SettingRow>
        <ToggleRow
          label="Keyboard sends Telnet special commands"
          description="Backspace sends Erase Character, Ctrl+C sends Interrupt Process, Ctrl+Z sends Suspend."
          checked={t.keyboardSendsSpecials}
          onChange={(v) => update((d) => void (d.telnet.keyboardSendsSpecials = v))}
        />
        <ToggleRow
          label="Return key sends Telnet New Line instead of ^M"
          description="Sends CR LF; turn off for servers that expect CR NUL."
          checked={t.returnSendsNewline}
          onChange={(v) => update((d) => void (d.telnet.returnSendsNewline = v))}
        />
      </div>
    </Section>
  );
}

/* -------------------------------------------------------------------------- */
/* Rlogin                                                                     */
/* -------------------------------------------------------------------------- */

export function RloginSection() {
  const { draft, update } = useEditor();
  return (
    <Section title="Rlogin" description="Rlogin sends both the local and the remote user name when connecting.">
      <FieldGrid cols={2}>
        <FormField label="Local username" hint="Empty = your Windows user name">
          <TextInput value={draft.rlogin.localUsername} placeholder="(Windows user name)" onChange={(v) => update((d) => void (d.rlogin.localUsername = v))} />
        </FormField>
        <FormField label="Remote username" hint="Set on the Session page">
          <TextInput value={draft.username} placeholder="(prompt)" onChange={(v) => update((d) => void (d.username = v))} />
        </FormField>
      </FieldGrid>
    </Section>
  );
}

/* -------------------------------------------------------------------------- */
/* Serial                                                                     */
/* -------------------------------------------------------------------------- */

const DATA_BITS: readonly SegmentOption<SerialOptions['dataBits']>[] = [5, 6, 7, 8].map((n) => ({
  value: n as SerialOptions['dataBits'],
  label: String(n)
}));

const STOP_BITS: readonly SegmentOption<SerialOptions['stopBits']>[] = [
  { value: 1, label: '1' },
  { value: 1.5, label: '1.5' },
  { value: 2, label: '2' }
];

const PARITY: { value: Parity; label: string }[] = [
  { value: 'none', label: 'None' },
  { value: 'odd', label: 'Odd' },
  { value: 'even', label: 'Even' },
  { value: 'mark', label: 'Mark' },
  { value: 'space', label: 'Space' }
];

const FLOW: { value: FlowControl; label: string }[] = [
  { value: 'none', label: 'None' },
  { value: 'xonxoff', label: 'XON/XOFF' },
  { value: 'rtscts', label: 'RTS/CTS' },
  { value: 'dsrdtr', label: 'DSR/DTR' }
];

export function SerialSection() {
  const { draft, update, fieldError } = useEditor();
  const s = draft.serial;
  return (
    <>
      <Section title="Serial line">
        <FieldGrid cols={2}>
          <FormField label="Serial line" error={fieldError('serial.line')} hint={`Pick a detected port or type its name, e.g. ${osTerms.serialLine}`}>
            <SerialLineInput />
          </FormField>
          <FormField label="Speed (baud)" error={fieldError('serial.baudRate')}>
            <BaudRateInput />
          </FormField>
        </FieldGrid>
      </Section>
      <Section title="Framing">
        <div className="dlg-rows">
          <SettingRow label="Data bits">
            <SegmentedControl size="sm" value={s.dataBits} options={DATA_BITS} onChange={(v) => update((d) => void (d.serial.dataBits = v))} />
          </SettingRow>
          <SettingRow label="Stop bits">
            <SegmentedControl size="sm" value={s.stopBits} options={STOP_BITS} onChange={(v) => update((d) => void (d.serial.stopBits = v))} />
          </SettingRow>
          <SettingRow label="Parity">
            <Select<Parity> value={s.parity} options={PARITY} onChange={(v) => update((d) => void (d.serial.parity = v))} />
          </SettingRow>
          <SettingRow label="Flow control">
            <Select<FlowControl> value={s.flowControl} options={FLOW} onChange={(v) => update((d) => void (d.serial.flowControl = v))} />
          </SettingRow>
        </div>
      </Section>
    </>
  );
}
