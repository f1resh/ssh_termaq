/**
 * Session editor: Terminal, Keyboard, Appearance, Translation and Logging sections.
 */
import { useEffect, useMemo, useState } from 'react';
import { Play, RotateCcw, X } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, IconButton, NumberInput, Select, TextInput, toast } from '@/components/ui';
import { CHARSETS } from '@shared/algorithms';
import type {
  BellMode,
  CloseOnExit,
  CursorStyle,
  FunctionKeyMode,
  KeyboardOptions,
  LogMode,
  LoggingOptions,
  TriState
} from '@shared/types';
import { allSchemes, resolveTerminalScheme } from '@/theme/resolve';
import { useSystemDark } from '@/theme/ThemeController';
import {
  Badge,
  Callout,
  ComboInput,
  FieldGrid,
  FormField,
  PathInput,
  Section,
  SegmentedControl,
  SettingRow,
  ToggleRow,
  type ComboOption,
  type SegmentOption
} from '../../common/controls';
import { FontPicker } from '../../common/FontPicker';
import { primaryFamily } from '../../common/fonts';
import { errorMessage } from '../../common/hooks';
import { SchemeCard } from '../../common/SchemeCard';
import { useEditor } from '../editorContext';
import { expandLogPath, fallbackSessionName } from '../sessionModel';

const TRISTATE: readonly SegmentOption<TriState>[] = [
  { value: 'auto', label: 'Auto' },
  { value: 'on', label: 'On' },
  { value: 'off', label: 'Off' }
];

/* -------------------------------------------------------------------------- */
/* Terminal                                                                   */
/* -------------------------------------------------------------------------- */

const BELL_OPTIONS: readonly SegmentOption<BellMode>[] = [
  { value: 'none', label: 'None' },
  { value: 'system', label: 'System sound' },
  { value: 'visual', label: 'Visual' },
  { value: 'sound', label: 'Sound file' }
];

const CLOSE_OPTIONS: readonly SegmentOption<CloseOnExit>[] = [
  { value: 'always', label: 'Always' },
  { value: 'clean', label: 'On clean exit' },
  { value: 'never', label: 'Never' }
];

const SOUND_FILTERS = [
  { name: 'Sound files', extensions: ['wav', 'mp3', 'ogg', 'flac', 'm4a'] },
  { name: 'All files', extensions: ['*'] }
];

async function testBell(mode: BellMode, path: string, flash: () => void): Promise<void> {
  if (mode === 'system') api.app.beep();
  else if (mode === 'visual') flash();
  else if (mode === 'sound') {
    const url = path ? await api.app.readFileDataUrl(path) : null;
    if (!url) {
      toast.warning('Cannot play the sound file', path ? 'The file could not be read.' : 'Choose a sound file first.');
      return;
    }
    try {
      await new Audio(url).play();
    } catch (e) {
      toast.warning('Cannot play the sound file', errorMessage(e));
    }
  }
}

export function TerminalSection() {
  const { draft, update, fieldError, warning, settings } = useEditor();
  const t = draft.terminal;
  const [flashing, setFlashing] = useState(false);
  useEffect(() => {
    if (!flashing) return;
    const id = setTimeout(() => setFlashing(false), 260);
    return () => clearTimeout(id);
  }, [flashing]);

  return (
    <>
      <Section title="Line discipline">
        <div className="dlg-rows">
          <SettingRow label="Local echo" description="Auto: on for Raw/Telnet until the server takes over echoing.">
            <SegmentedControl size="sm" value={t.localEcho} options={TRISTATE} onChange={(v) => update((d) => void (d.terminal.localEcho = v))} />
          </SettingRow>
          <SettingRow label="Local line editing" description="Edit the line locally and send it on Enter.">
            <SegmentedControl
              size="sm"
              value={t.localLineEditing}
              options={TRISTATE}
              onChange={(v) => update((d) => void (d.terminal.localLineEditing = v))}
            />
          </SettingRow>
          <ToggleRow label="Auto wrap mode" description="Wrap text at the right margin." checked={t.autoWrap} onChange={(v) => update((d) => void (d.terminal.autoWrap = v))} />
          <ToggleRow
            label="Implicit CR in every LF"
            description="For servers that send bare line feeds."
            checked={t.implicitCrInLf}
            onChange={(v) => update((d) => void (d.terminal.implicitCrInLf = v))}
          />
          <ToggleRow
            label="Implicit LF in every CR"
            description="For servers that send bare carriage returns."
            checked={t.implicitLfInCr}
            onChange={(v) => update((d) => void (d.terminal.implicitLfInCr = v))}
          />
        </div>
        <FormField label="Answerback to ^E" hint="Sent when the server sends ENQ (Ctrl+E)">
          <TextInput className="mono-input" value={t.answerback} onChange={(v) => update((d) => void (d.terminal.answerback = v))} placeholder="(nothing)" />
        </FormField>
      </Section>

      <Section title="Bell">
        <div className={flashing ? 'bell-row bell-row--flash' : 'bell-row'}>
          <SegmentedControl value={t.bell} options={BELL_OPTIONS} onChange={(v) => update((d) => void (d.terminal.bell = v))} ariaLabel="Bell" />
          <Button
            size="sm"
            icon={<Play size={13} />}
            disabled={t.bell === 'none'}
            onClick={() => void testBell(t.bell, t.bellSoundPath, () => setFlashing(true))}
          >
            Test
          </Button>
        </div>
        {t.bell === 'sound' && (
          <FormField label="Sound file" warning={warning('terminal.bellSoundPath')}>
            <PathInput
              value={t.bellSoundPath}
              placeholder="C:\\Windows\\Media\\ding.wav"
              onChange={(v) => update((d) => void (d.terminal.bellSoundPath = v))}
              onBrowse={() => api.dialog.openFile({ title: 'Choose a bell sound', defaultPath: t.bellSoundPath || undefined, filters: SOUND_FILTERS })}
            />
          </FormField>
        )}
      </Section>

      <Section title="Window">
        <FieldGrid cols={2}>
          <FormField
            label="Scrollback lines"
            hint={`0 = use the global setting (${settings.terminal.scrollback.toLocaleString('en-US')})`}
            error={fieldError('terminal.scrollbackLines')}
          >
            <NumberInput value={t.scrollbackLines} min={0} max={1000000} step={1000} onChange={(n) => update((d) => void (d.terminal.scrollbackLines = n))} />
          </FormField>
          <FormField label="Fixed tab title" hint="Empty = session name or the title set by the server">
            <TextInput value={t.windowTitle} placeholder={draft.name || fallbackSessionName(draft)} onChange={(v) => update((d) => void (d.terminal.windowTitle = v))} />
          </FormField>
        </FieldGrid>
        <div className="dlg-rows">
          <ToggleRow
            label="Allow the server to change the title"
            description="Honour OSC title sequences (ignored when a fixed title is set)."
            checked={t.allowRemoteTitle}
            onChange={(v) => update((d) => void (d.terminal.allowRemoteTitle = v))}
          />
          <SettingRow label="Close the pane on exit" description="What happens when the connection ends.">
            <SegmentedControl size="sm" value={t.closeOnExit} options={CLOSE_OPTIONS} onChange={(v) => update((d) => void (d.terminal.closeOnExit = v))} />
          </SettingRow>
        </div>
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* Keyboard                                                                   */
/* -------------------------------------------------------------------------- */

const BACKSPACE_OPTIONS: readonly SegmentOption<KeyboardOptions['backspace']>[] = [
  { value: 'del', label: 'Control-? (127)' },
  { value: 'ctrl-h', label: 'Control-H (8)' }
];

const HOME_END_OPTIONS: readonly SegmentOption<KeyboardOptions['homeEnd']>[] = [
  { value: 'xterm', label: 'xterm', title: 'ESC[H / ESC[F' },
  { value: 'standard', label: 'Standard', title: 'ESC[1~ / ESC[4~ (PuTTY default)' },
  { value: 'rxvt', label: 'rxvt', title: 'ESC[H / ESC[Ow' }
];

const FUNCTION_KEY_OPTIONS: { value: FunctionKeyMode; label: string }[] = [
  { value: 'xterm', label: 'Xterm 216+ (modern xterm)' },
  { value: 'esc-n', label: 'ESC[n~ (PuTTY default)' },
  { value: 'linux', label: 'Linux console' },
  { value: 'xterm-r6', label: 'Xterm R6' },
  { value: 'vt400', label: 'VT400' },
  { value: 'vt100plus', label: 'VT100+' },
  { value: 'sco', label: 'SCO' }
];

export function KeyboardSection() {
  const { draft, update } = useEditor();
  const k = draft.keyboard;
  return (
    <>
      <Section title="Keys">
        <div className="dlg-rows">
          <SettingRow label="Backspace key sends">
            <SegmentedControl size="sm" value={k.backspace} options={BACKSPACE_OPTIONS} onChange={(v) => update((d) => void (d.keyboard.backspace = v))} />
          </SettingRow>
          <SettingRow label="Home and End keys" description="Hover an option to see the sequences sent.">
            <SegmentedControl size="sm" value={k.homeEnd} options={HOME_END_OPTIONS} onChange={(v) => update((d) => void (d.keyboard.homeEnd = v))} />
          </SettingRow>
          <SettingRow label="Function keys and keypad">
            <Select<FunctionKeyMode> value={k.functionKeys} options={FUNCTION_KEY_OPTIONS} onChange={(v) => update((d) => void (d.keyboard.functionKeys = v))} />
          </SettingRow>
        </div>
      </Section>
      <Section title="Application modes">
        <div className="dlg-rows">
          <ToggleRow
            label="Disable application cursor keys mode"
            description="Cursor keys always send normal sequences, even when the server requests application mode."
            checked={k.disableAppCursorKeys}
            onChange={(v) => update((d) => void (d.keyboard.disableAppCursorKeys = v))}
          />
          <ToggleRow
            label="Disable application keypad mode"
            description="The numeric keypad always sends digits."
            checked={k.disableAppKeypad}
            onChange={(v) => update((d) => void (d.keyboard.disableAppKeypad = v))}
          />
          <ToggleRow
            label="Control + Alt is AltGr"
            description="Needed to type characters such as @ { } on many non-US layouts."
            checked={k.ctrlAltIsAltGr}
            onChange={(v) => update((d) => void (d.keyboard.ctrlAltIsAltGr = v))}
          />
        </div>
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* Appearance                                                                 */
/* -------------------------------------------------------------------------- */

type Inherit<T extends string> = T | 'global';

function boolChoice(v: boolean | undefined): Inherit<'on' | 'off'> {
  return v === undefined || v === null ? 'global' : v ? 'on' : 'off';
}
function fromBoolChoice(v: Inherit<'on' | 'off'>): boolean | undefined {
  return v === 'global' ? undefined : v === 'on';
}

const FONT_SIZES = [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24, 26, 28, 32, 36, 40];

export function AppearanceSection() {
  const { draft, update, settings, stashedCustomScheme, setStashedCustomScheme, isDefault } = useEditor();
  const systemDark = useSystemDark();
  const a = draft.appearance;
  const g = settings.terminal;
  const schemes = useMemo(() => allSchemes(settings), [settings]);
  const globalScheme = useMemo(() => resolveTerminalScheme(settings, undefined, systemDark), [settings, systemDark]);
  const usingCustom = !!a.customScheme;
  const selectedId = usingCustom ? '__custom' : a.colorScheme || '';

  const sizeOptions = useMemo(() => {
    const sizes = new Set(FONT_SIZES);
    if (a.fontSize) sizes.add(a.fontSize);
    return [
      { value: 0, label: `Global (${g.fontSize}px)` },
      ...[...sizes].sort((x, y) => x - y).map((n) => ({ value: n, label: `${n}px` }))
    ];
  }, [a.fontSize, g.fontSize]);

  const cursorOptions: readonly SegmentOption<Inherit<CursorStyle>>[] = [
    { value: 'global', label: 'Global', title: `Global setting: ${g.cursorStyle}` },
    { value: 'block', label: 'Block' },
    { value: 'underline', label: 'Underline' },
    { value: 'bar', label: 'Bar' }
  ];
  const boolOptions = (globalValue: boolean): readonly SegmentOption<Inherit<'on' | 'off'>>[] => [
    { value: 'global', label: `Global (${globalValue ? 'on' : 'off'})` },
    { value: 'on', label: 'On' },
    { value: 'off', label: 'Off' }
  ];

  const pickScheme = (id: string) =>
    update((d) => {
      d.appearance.customScheme = undefined;
      d.appearance.colorScheme = id || undefined;
    });

  return (
    <>
      <Section title="Font" description="Leave a setting on Global to follow Settings › Terminal.">
        <div className="dlg-rows">
          <SettingRow label="Font family" description={a.fontFamily ? undefined : `Global: ${primaryFamily(g.fontFamily)}`}>
            <div className="se-font-picker">
              <FontPicker
                value={a.fontFamily ?? ''}
                placeholder={primaryFamily(g.fontFamily)}
                onChange={(css) => update((d) => void (d.appearance.fontFamily = css || undefined))}
              />
            </div>
            {a.fontFamily && (
              <IconButton label="Use the global font" size="sm" onClick={() => update((d) => void (d.appearance.fontFamily = undefined))}>
                <X size={13} />
              </IconButton>
            )}
          </SettingRow>
          <SettingRow label="Font size">
            <Select<number> value={a.fontSize ?? 0} options={sizeOptions} onChange={(n) => update((d) => void (d.appearance.fontSize = n || undefined))} />
          </SettingRow>
          <SettingRow label="Cursor shape">
            <SegmentedControl
              size="sm"
              value={a.cursorStyle ?? 'global'}
              options={cursorOptions}
              onChange={(v) => update((d) => void (d.appearance.cursorStyle = v === 'global' ? undefined : v))}
            />
          </SettingRow>
          <SettingRow label="Cursor blinks">
            <SegmentedControl
              size="sm"
              value={boolChoice(a.cursorBlink)}
              options={boolOptions(g.cursorBlink)}
              onChange={(v) => update((d) => void (d.appearance.cursorBlink = fromBoolChoice(v)))}
            />
          </SettingRow>
          <SettingRow label="Bold text in bright colours">
            <SegmentedControl
              size="sm"
              value={boolChoice(a.boldIsBright)}
              options={boolOptions(g.drawBoldTextInBrightColors)}
              onChange={(v) => update((d) => void (d.appearance.boldIsBright = fromBoolChoice(v)))}
            />
          </SettingRow>
        </div>
      </Section>

      <Section
        title="Colour scheme"
        actions={
          (stashedCustomScheme || a.colorScheme) && (
            <Button
              size="sm"
              variant="ghost"
              icon={<RotateCcw size={13} />}
              disabled={!usingCustom && !a.colorScheme}
              onClick={() => pickScheme('')}
            >
              Reset to global
            </Button>
          )
        }
      >
        {usingCustom && (
          <Callout tone="info" title="Using the session's own colours">
            {isDefault ? 'These colours apply to every new session.' : 'Typically imported from PuTTY’s Colours panel. Pick a scheme to replace them.'}
          </Callout>
        )}
        <div className="scheme-grid" role="radiogroup" aria-label="Colour scheme">
          <SchemeCard
            scheme={globalScheme}
            label="Use global"
            badge={<Badge>{globalScheme.name}</Badge>}
            active={selectedId === ''}
            onSelect={() => pickScheme('')}
          />
          {stashedCustomScheme && (
            <SchemeCard
              scheme={stashedCustomScheme}
              label={stashedCustomScheme.name || 'Session colours'}
              badge={<Badge tone="accent">Imported</Badge>}
              active={selectedId === '__custom'}
              onSelect={() => update((d) => void (d.appearance.customScheme = structuredClone(stashedCustomScheme)))}
            />
          )}
          {schemes.map((s) => (
            <SchemeCard key={s.id} scheme={s} label={s.name} active={selectedId === s.id} onSelect={() => pickScheme(s.id)} />
          ))}
        </div>
        {stashedCustomScheme && !usingCustom && (
          <div className="se-stash-note">
            The imported colours stay available until you save.{' '}
            <button type="button" className="link-btn" onClick={() => setStashedCustomScheme(undefined)}>
              Forget them
            </button>
          </div>
        )}
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* Translation                                                                */
/* -------------------------------------------------------------------------- */

const CHARSET_OPTIONS: ComboOption[] = CHARSETS.map((c) => ({
  value: c,
  hint:
    c === 'UTF-8'
      ? 'Recommended'
      : c === 'ISO-8859-1'
        ? 'Latin-1'
        : c === 'windows-1251' || c === 'KOI8-R' || c === 'CP866'
          ? 'Cyrillic'
          : c === 'Shift_JIS' || c === 'EUC-JP'
            ? 'Japanese'
            : c === 'GBK' || c === 'GB18030'
              ? 'Simplified Chinese'
              : c === 'Big5'
                ? 'Traditional Chinese'
                : c === 'EUC-KR'
                  ? 'Korean'
                  : c === 'CP437'
                    ? 'IBM PC / DOS'
                    : undefined
}));

export function TranslationSection() {
  const { draft, update } = useEditor();
  const t = draft.translation;
  const known = (CHARSETS as readonly string[]).some((c) => c.toLowerCase() === t.charset.toLowerCase());
  return (
    <>
      <Section title="Character set" description="Text received from the host is decoded from this character set, and keystrokes are encoded to it.">
        <FormField label="Remote character set" warning={known ? undefined : 'Not in the list of known character sets'}>
          <ComboInput value={t.charset} options={CHARSET_OPTIONS} onChange={(v) => update((d) => void (d.translation.charset = v))} mono />
        </FormField>
        <div className="dlg-rows">
          <ToggleRow
            label="Treat CJK ambiguous characters as wide"
            description="Use double-width cells for East Asian ambiguous-width characters (matches many CJK systems)."
            checked={t.cjkAmbiguousWide}
            onChange={(v) => update((d) => void (d.translation.cjkAmbiguousWide = v))}
          />
        </div>
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* Logging                                                                    */
/* -------------------------------------------------------------------------- */

const IF_EXISTS_OPTIONS: readonly SegmentOption<LoggingOptions['ifExists']>[] = [
  { value: 'append', label: 'Append' },
  { value: 'overwrite', label: 'Overwrite' },
  { value: 'ask', label: 'Ask' }
];

const LOG_FILTERS = [
  { name: 'Log files', extensions: ['log', 'txt'] },
  { name: 'All files', extensions: ['*'] }
];

function useLogDirectory(configured: string): string {
  const [dir, setDir] = useState(configured);
  useEffect(() => {
    if (configured) {
      setDir(configured);
      return;
    }
    let alive = true;
    api.app
      .logDirectory()
      .then((d) => alive && setDir(d || ''))
      .catch(() => undefined);
    return () => {
      alive = false;
    };
  }, [configured]);
  return dir;
}

export function LoggingSection() {
  const { draft, update, fieldError, settings, isDefault } = useEditor();
  const l = draft.logging;
  const logDir = useLogDirectory(settings.logging.directory);
  const modeOptions: readonly SegmentOption<LogMode>[] = [
    { value: 'off', label: 'Off' },
    { value: 'printable', label: 'Printable output', title: 'Text only, control sequences removed' },
    { value: 'all', label: 'All output', title: 'Raw bytes received from the host' },
    ...(draft.protocol === 'ssh' || isDefault ? [{ value: 'ssh-debug' as const, label: 'SSH packets', title: 'SSH protocol debug log (no secrets)' }] : [])
  ];
  const example = useMemo(
    () =>
      expandLogPath(l.path || '', {
        host: draft.protocol === 'serial' ? draft.serial.line : draft.host || 'host',
        port: draft.port,
        session: draft.name || fallbackSessionName(draft),
        date: new Date()
      }),
    [l.path, draft]
  );
  const relative = !!l.path && !/^([a-zA-Z]:[\\/]|\\\\|\/)/.test(l.path);
  const off = l.mode === 'off';

  return (
    <>
      <Section title="Session logging">
        <SegmentedControl
          value={l.mode === 'ssh-debug' && draft.protocol !== 'ssh' && !isDefault ? 'all' : l.mode}
          options={modeOptions}
          onChange={(v) => update((d) => void (d.logging.mode = v))}
          ariaLabel="Logging mode"
        />
      </Section>
      <Section title="Log file">
        <div className="placeholder-layout">
          <FormField
            label="File name"
            error={fieldError('logging.path')}
            hint={relative && logDir ? `Relative to ${logDir}` : undefined}
          >
            <PathInput
              value={l.path}
              placeholder="&S-&Y&M&D-&T.log"
              invalid={!!fieldError('logging.path')}
              onChange={(v) => update((d) => void (d.logging.path = v))}
              onBrowse={() =>
                api.dialog.saveFile({ title: 'Log file', defaultPath: l.path && !relative ? l.path : undefined, filters: LOG_FILTERS })
              }
            />
          </FormField>
          <div className="placeholder-help">
            <div className="placeholder-help__title">Placeholders</div>
            <dl>
              <dt>&amp;Y &amp;M &amp;D</dt>
              <dd>year, month, day</dd>
              <dt>&amp;T</dt>
              <dd>time (HHMMSS)</dd>
              <dt>&amp;H &amp;P</dt>
              <dd>host, port</dd>
              <dt>&amp;S</dt>
              <dd>session name</dd>
              <dt>&amp;&amp;</dt>
              <dd>a literal &amp;</dd>
            </dl>
            {example && (
              <div className="placeholder-help__example">
                e.g. <code>{example}</code>
              </div>
            )}
          </div>
        </div>
      </Section>
      <Section title="Options">
        <div className="dlg-rows">
          <SettingRow label="If the file already exists" disabled={off}>
            <SegmentedControl size="sm" value={l.ifExists} options={IF_EXISTS_OPTIONS} onChange={(v) => update((d) => void (d.logging.ifExists = v))} />
          </SettingRow>
          <ToggleRow
            label="Flush to disk frequently"
            description="Write every chunk immediately so the log is complete even if the app crashes."
            checked={l.flush}
            disabled={off}
            onChange={(v) => update((d) => void (d.logging.flush = v))}
          />
          <ToggleRow
            label="Write a header line"
            description="Records the date, time and host when the log is opened."
            checked={l.header}
            disabled={off}
            onChange={(v) => update((d) => void (d.logging.header = v))}
          />
        </div>
        {l.mode === 'ssh-debug' && <Callout tone="info">Packet logs omit passwords and other authentication secrets, but contain everything else sent over the connection.</Callout>}
      </Section>
    </>
  );
}
