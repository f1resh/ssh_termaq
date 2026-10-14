/**
 * Application settings. Changes apply live (useSettings.update -> host -> settings:onChanged).
 */
import { useEffect, useMemo, useState, type ReactNode } from 'react';
import {
  Brush,
  Database,
  FileUp,
  FolderOpen,
  Info,
  Keyboard,
  MonitorCog,
  MousePointerClick,
  Palette,
  Plus,
  RotateCcw,
  Save,
  ShieldCheck,
  SquareTerminal,
  Trash2,
  Copy as CopyIcon
} from 'lucide-react';
import { api } from '@/bridge/api';
import { COMMANDS } from '@/commands/commands';
import { chordOf, formatChord, normalizeChord } from '@/commands/chords';
import { Button, Checkbox, Modal, NumberInput, Select, TextArea, TextInput, cx, toast } from '@/components/ui';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { APP_THEMES } from '@/theme/appThemes';
import { BUILTIN_SCHEMES } from '@/theme/schemes';
import { useSystemDark } from '@/theme/ThemeController';
import { resolveAppTheme, resolveTerminalScheme } from '@/theme/resolve';
import { DEFAULT_KEYBINDINGS, newId } from '@shared/defaults';
import type { AppSettings, ColorScheme, CommandId, DeepPartial, KnownHost } from '@shared/types';
import { AboutContent } from '../about/AboutContent';
import { ColorSwatches, FormField, NavList, SegmentedControl, Section, SettingRow, Slider, ToggleRow, type NavItem } from '../common/controls';
import { FontPicker } from '../common/FontPicker';
import { errorMessage } from '../common/hooks';
import { SchemeCard } from '../common/SchemeCard';
import { ANSI_KEYS, TerminalSample } from '../common/SchemePreview';
import './settings.css';

type SectionId = 'appearance' | 'terminal' | 'behavior' | 'files' | 'keys' | 'schemes' | 'hosts' | 'defaults' | 'data' | 'about';

const NAV: NavItem[] = [
  { id: 'appearance', label: 'Appearance', icon: <Brush size={15} /> },
  { id: 'terminal', label: 'Terminal', icon: <SquareTerminal size={15} /> },
  { id: 'behavior', label: 'Behavior', icon: <MousePointerClick size={15} /> },
  { id: 'files', label: 'File manager', icon: <FolderOpen size={15} /> },
  { id: 'keys', label: 'Keybindings', icon: <Keyboard size={15} /> },
  { id: 'schemes', label: 'Color schemes', icon: <Palette size={15} /> },
  { id: 'hosts', label: 'Known hosts', icon: <ShieldCheck size={15} /> },
  { id: 'defaults', label: 'Default session', icon: <MonitorCog size={15} /> },
  { id: 'data', label: 'Data & backup', icon: <Database size={15} /> },
  { id: 'about', label: 'About', icon: <Info size={15} /> }
];

function useSettingsState(): [AppSettings, (patch: DeepPartial<AppSettings>) => void] {
  const settings = useSettings((s) => s.settings);
  const update = (patch: DeepPartial<AppSettings>) => void useSettings.getState().update(patch).catch((e) => toast.error('Could not save settings', errorMessage(e)));
  return [settings, update];
}

export function SettingsDialog({ initialSection, onClose }: { initialSection?: string; onClose: () => void }) {
  const [section, setSection] = useState<SectionId>(() => (NAV.some((n) => n.id === initialSection) ? (initialSection as SectionId) : 'appearance'));
  const content: Record<SectionId, ReactNode> = {
    appearance: <AppearancePage />,
    terminal: <TerminalPage />,
    behavior: <BehaviorPage />,
    files: <FilesPage />,
    keys: <KeybindingsPage />,
    schemes: <SchemesPage />,
    hosts: <KnownHostsPage />,
    defaults: <DefaultsPage onClose={onClose} />,
    data: <DataPage />,
    about: <AboutContent />
  };
  return (
    <Modal open title="Settings" onClose={onClose} width={980} height={700} bodyClassName="dlg-body--flush">
      <div className="dlg-split">
        <div className="dlg-split__nav">
          <NavList items={NAV} active={section} onSelect={(id) => setSection(id as SectionId)} ariaLabel="Settings sections" />
        </div>
        <div className="dlg-split__content settings-content">{content[section]}</div>
      </div>
    </Modal>
  );
}

/* ---------------------------------------------------------------- Appearance */

function ThemeCard({ id, label, active, onSelect }: { id: string; label: string; active: boolean; onSelect: () => void }) {
  const systemDark = useSystemDark();
  const t = id === 'system' ? APP_THEMES.find((x) => x.id === (systemDark ? 'dark' : 'light')) ?? APP_THEMES[0] : APP_THEMES.find((x) => x.id === id)!;
  const c = t.colors;
  return (
    <button className={cx('theme-card', active && 'theme-card--active')} onClick={onSelect} title={label}>
      <span className="theme-card__preview" style={{ background: c.bg, borderColor: c.border }}>
        <span className="theme-card__bar" style={{ background: c.titlebar }}>
          <span style={{ background: c.tabActive, borderColor: c.accent }} />
        </span>
        <span className="theme-card__side" style={{ background: c.sidebar }}>
          <i style={{ background: c.textFaint }} />
          <i style={{ background: c.accent }} />
          <i style={{ background: c.textFaint }} />
        </span>
        <span className="theme-card__main">
          <i style={{ background: c.text, width: '70%' }} />
          <i style={{ background: c.textMuted, width: '45%' }} />
          <i style={{ background: c.success, width: '30%' }} />
        </span>
      </span>
      <span className="theme-card__label">{label}</span>
    </button>
  );
}

function AppearancePage() {
  const [s, update] = useSettingsState();
  const a = s.appearance;
  return (
    <>
      <h2 className="dlg-page-title">Appearance</h2>
      <Section title="Theme">
        <div className="theme-grid">
          <ThemeCard id="system" label="System" active={a.theme === 'system'} onSelect={() => update({ appearance: { theme: 'system' } })} />
          {APP_THEMES.map((t) => (
            <ThemeCard key={t.id} id={t.id} label={t.name} active={a.theme === t.id} onSelect={() => update({ appearance: { theme: t.id } })} />
          ))}
        </div>
      </Section>
      <Section title="Accent color" description="Highlights, selection and primary buttons.">
        <ColorSwatches value={a.accentColor} onChange={(accentColor) => update({ appearance: { accentColor } })} noneLabel="Theme default" />
      </Section>
      <Section title="Window">
        <SettingRow label="Background material" description="Mica and Acrylic use the Windows 11 translucent window backdrop.">
          <SegmentedControl
            value={a.backgroundMaterial}
            onChange={(backgroundMaterial) => update({ appearance: { backgroundMaterial } })}
            options={[
              { value: 'none', label: 'Solid' },
              { value: 'mica', label: 'Mica' },
              { value: 'acrylic', label: 'Acrylic' }
            ]}
            size="sm"
          />
        </SettingRow>
        <SettingRow label="Interface scale">
          <Slider value={a.uiScale} min={0.8} max={1.5} step={0.05} onChange={(uiScale) => update({ appearance: { uiScale } })} format={(v) => `${Math.round(v * 100)}%`} />
        </SettingRow>
        <ToggleRow label="Show the session sidebar" checked={a.showSidebar} onChange={(showSidebar) => update({ appearance: { showSidebar } })} />
      </Section>
    </>
  );
}

/* ------------------------------------------------------------------ Terminal */

function TerminalPage() {
  const [s, update] = useSettingsState();
  const t = s.terminal;
  const systemDark = useSystemDark();
  const schemes = [...BUILTIN_SCHEMES, ...s.customSchemes];
  const autoScheme = resolveAppTheme(s, systemDark).defaultScheme;
  const preview = resolveTerminalScheme(s, undefined, systemDark);
  return (
    <>
      <h2 className="dlg-page-title">Terminal</h2>
      <Section title="Font">
        <TerminalSample
          scheme={preview}
          height={150}
          font={{
            fontFamily: t.fontFamily,
            fontSize: t.fontSize,
            lineHeight: t.lineHeight,
            letterSpacing: t.letterSpacing,
            fontWeight: t.fontWeight,
            fontWeightBold: t.fontWeightBold,
            cursorStyle: t.cursorStyle,
            cursorBlink: t.cursorBlink,
            boldIsBright: t.drawBoldTextInBrightColors,
            padding: t.padding
          }}
        />
        <div className="dlg-grid dlg-grid--2 settings-gap">
          <FormField label="Font family">
            <FontPicker value={t.fontFamily} onChange={(fontFamily) => update({ terminal: { fontFamily } })} />
          </FormField>
          <FormField label="Size">
            <NumberInput value={t.fontSize} min={6} max={48} onChange={(fontSize) => update({ terminal: { fontSize } })} />
          </FormField>
          <FormField label="Line height">
            <Slider value={t.lineHeight} min={0.9} max={2} step={0.05} onChange={(lineHeight) => update({ terminal: { lineHeight } })} format={(v) => v.toFixed(2)} />
          </FormField>
          <FormField label="Letter spacing">
            <Slider value={t.letterSpacing} min={-2} max={4} step={0.5} onChange={(letterSpacing) => update({ terminal: { letterSpacing } })} format={(v) => `${v}px`} />
          </FormField>
          <FormField label="Weight">
            <Select value={t.fontWeight} onChange={(fontWeight) => update({ terminal: { fontWeight } })} options={(['300', 'normal', '500', '600', 'bold'] as const).map((v) => ({ value: v, label: v }))} />
          </FormField>
          <FormField label="Bold weight">
            <Select value={t.fontWeightBold} onChange={(fontWeightBold) => update({ terminal: { fontWeightBold } })} options={(['500', '600', 'bold', '800'] as const).map((v) => ({ value: v, label: v }))} />
          </FormField>
        </div>
      </Section>
      <Section title="Color scheme" description="Sessions can override this in their own Appearance settings.">
        <div className="scheme-grid">
          <SchemeCard scheme={BUILTIN_SCHEMES.find((x) => x.id === autoScheme)} label="Match app theme" active={t.colorScheme === 'auto'} onSelect={() => update({ terminal: { colorScheme: 'auto' } })} />
          {schemes.map((sc) => (
            <SchemeCard key={sc.id} scheme={sc} label={sc.name} active={t.colorScheme === sc.id} onSelect={() => update({ terminal: { colorScheme: sc.id } })} />
          ))}
        </div>
      </Section>
      <Section title="Cursor & rendering">
        <SettingRow label="Cursor style">
          <SegmentedControl value={t.cursorStyle} onChange={(cursorStyle) => update({ terminal: { cursorStyle } })} size="sm" options={[{ value: 'block', label: 'Block' }, { value: 'underline', label: 'Underline' }, { value: 'bar', label: 'Bar' }]} />
        </SettingRow>
        <ToggleRow label="Blinking cursor" checked={t.cursorBlink} onChange={(cursorBlink) => update({ terminal: { cursorBlink } })} />
        <ToggleRow label="Show bold text in bright colors" checked={t.drawBoldTextInBrightColors} onChange={(drawBoldTextInBrightColors) => update({ terminal: { drawBoldTextInBrightColors } })} />
        <SettingRow label="Scrollback lines" description="Per-session values override this.">
          <NumberInput value={t.scrollback} min={0} max={1000000} step={1000} onChange={(scrollback) => update({ terminal: { scrollback } })} />
        </SettingRow>
        <SettingRow label="Minimum contrast" description="Adjusts colors that would be hard to read (1 = off).">
          <Slider value={t.minimumContrastRatio} min={1} max={7} step={0.5} onChange={(minimumContrastRatio) => update({ terminal: { minimumContrastRatio } })} format={(v) => (v === 1 ? 'Off' : v.toFixed(1))} />
        </SettingRow>
        <SettingRow label="Padding">
          <Slider value={t.padding} min={0} max={24} onChange={(padding) => update({ terminal: { padding } })} format={(v) => `${v}px`} />
        </SettingRow>
        <SettingRow label="Renderer" description="GPU rendering is faster; DOM rendering is the compatible fallback.">
          <SegmentedControl value={t.renderer} onChange={(renderer) => update({ terminal: { renderer } })} size="sm" options={[{ value: 'webgl', label: 'GPU (WebGL)' }, { value: 'dom', label: 'DOM' }]} />
        </SettingRow>
      </Section>
    </>
  );
}

/* ------------------------------------------------------------------ Behavior */

function BehaviorPage() {
  const [s, update] = useSettingsState();
  const b = s.behavior;
  return (
    <>
      <h2 className="dlg-page-title">Behavior</h2>
      <Section title="Mouse & clipboard">
        <ToggleRow label="Copy on select" description="Selected text is copied to the clipboard immediately, like PuTTY." checked={b.copyOnSelect} onChange={(copyOnSelect) => update({ behavior: { copyOnSelect } })} />
        <SettingRow label="Right click" description="Shift+right click always opens the menu.">
          <SegmentedControl value={b.rightClick} onChange={(rightClick) => update({ behavior: { rightClick } })} size="sm" options={[{ value: 'paste', label: 'Paste' }, { value: 'menu', label: 'Context menu' }]} />
        </SettingRow>
        <ToggleRow label="Middle click pastes" checked={b.middleClickPaste} onChange={(middleClickPaste) => update({ behavior: { middleClickPaste } })} />
        <ToggleRow label="Warn before pasting multiple lines" checked={b.warnOnMultilinePaste} onChange={(warnOnMultilinePaste) => update({ behavior: { warnOnMultilinePaste } })} />
        <FormField label="Word separators (double-click selection)">
          <TextInput value={b.wordSeparators} onChange={(wordSeparators) => update({ behavior: { wordSeparators } })} className="mono-input" />
        </FormField>
      </Section>
      <Section title="Sessions">
        <SettingRow label="Reconnect when a connection drops">
          <SegmentedControl value={b.reconnect} onChange={(reconnect) => update({ behavior: { reconnect } })} size="sm" options={[{ value: 'off', label: 'Off' }, { value: 'prompt', label: 'Ask' }, { value: 'auto', label: 'Automatically' }]} />
        </SettingRow>
        <SettingRow label="Double-click on a saved session">
          <SegmentedControl value={b.sessionDoubleClick} onChange={(sessionDoubleClick) => update({ behavior: { sessionDoubleClick } })} size="sm" options={[{ value: 'tab', label: 'Opens a tab' }, { value: 'split', label: 'Splits the pane' }]} />
        </SettingRow>
        <ToggleRow label="Confirm before closing connected sessions" checked={b.confirmCloseConnected} onChange={(confirmCloseConnected) => update({ behavior: { confirmCloseConnected } })} />
        <ToggleRow label="Restore tabs on startup" checked={b.restoreTabs} onChange={(restoreTabs) => update({ behavior: { restoreTabs } })} />
      </Section>
      <Section title="Scrolling">
        <ToggleRow label="Scroll to the bottom on output" checked={b.scrollOnOutput} onChange={(scrollOnOutput) => update({ behavior: { scrollOnOutput } })} />
        <ToggleRow label="Scroll to the bottom on keystroke" checked={b.scrollOnKeystroke} onChange={(scrollOnKeystroke) => update({ behavior: { scrollOnKeystroke } })} />
      </Section>
    </>
  );
}

/* -------------------------------------------------------------- File manager */

function FilesPage() {
  const [s, update] = useSettingsState();
  const f = s.files;
  const pickEditor = async () => {
    const path = await api.dialog.openFile({ title: 'Choose an editor', filters: [{ name: 'Programs', extensions: ['exe', 'cmd', 'bat'] }] });
    if (path) update({ files: { editor: path } });
  };
  return (
    <>
      <h2 className="dlg-page-title">File manager</h2>
      <Section title="Transfers">
        <SettingRow label="When a file already exists" description="Also changeable from the file manager toolbar.">
          <Select
            value={f.defaultConflict}
            onChange={(defaultConflict) => update({ files: { defaultConflict } })}
            options={[
              { value: 'ask', label: 'Ask' },
              { value: 'overwrite', label: 'Overwrite' },
              { value: 'newer', label: 'Overwrite if newer' },
              { value: 'skip', label: 'Skip' },
              { value: 'resume', label: 'Resume (SFTP)' }
            ]}
          />
        </SettingRow>
        <SettingRow label="Parallel transfers per connection" description="Applies to file manager tabs opened from now on.">
          <NumberInput value={f.parallelTransfers} min={1} max={6} onChange={(parallelTransfers) => update({ files: { parallelTransfers } })} />
        </SettingRow>
        <ToggleRow label="Keep modification times" description="Copies get the original file's date, which Synchronize relies on." checked={f.preserveTimestamps} onChange={(preserveTimestamps) => update({ files: { preserveTimestamps } })} />
      </Section>
      <Section title="Browsing">
        <ToggleRow label="Show hidden files" description="Names starting with a dot and files with the Hidden attribute." checked={f.showHidden} onChange={(showHidden) => update({ files: { showHidden } })} />
        <ToggleRow label="Confirm before deleting" checked={f.confirmDelete} onChange={(confirmDelete) => update({ files: { confirmDelete } })} />
        <ToggleRow label="Delete local files to the Recycle Bin" checked={f.deleteToRecycleBin} onChange={(deleteToRecycleBin) => update({ files: { deleteToRecycleBin } })} />
      </Section>
      <Section title="Editing" description="Edit (F4) opens a server file in an editor and uploads it again every time you save.">
        <FormField label="Editor" hint="Leave empty to use the program Windows uses to edit that file type (Notepad otherwise).">
          <div className="settings-inline">
            <TextInput value={f.editor} onChange={(editor) => update({ files: { editor } })} placeholder="C:\Program Files\Notepad++\notepad++.exe" className="mono-input" />
            <Button onClick={() => void pickEditor()}>Browse…</Button>
            {f.editor && <Button variant="ghost" onClick={() => update({ files: { editor: '' } })}>Reset</Button>}
          </div>
        </FormField>
      </Section>
    </>
  );
}

/* --------------------------------------------------------------- Keybindings */

function KeybindingsPage() {
  const [s, update] = useSettingsState();
  const [filter, setFilter] = useState('');
  const [recording, setRecording] = useState<CommandId | null>(null);
  const bindings = { ...DEFAULT_KEYBINDINGS, ...s.keybindings } as Record<CommandId, string>;
  const conflicts = useMemo(() => {
    const byChord = new Map<string, CommandId[]>();
    for (const [id, chord] of Object.entries(bindings)) {
      const n = normalizeChord(chord);
      if (!n) continue;
      byChord.set(n, [...(byChord.get(n) ?? []), id as CommandId]);
    }
    return new Set([...byChord.values()].filter((v) => v.length > 1).flat());
  }, [bindings]);

  useEffect(() => {
    if (!recording) return;
    const onKey = (e: KeyboardEvent) => {
      e.preventDefault();
      e.stopPropagation();
      if (['Control', 'Shift', 'Alt', 'Meta'].includes(e.key)) return;
      if (e.key === 'Escape') {
        setRecording(null);
        return;
      }
      const chord = e.key === 'Backspace' && !e.ctrlKey && !e.altKey && !e.shiftKey ? '' : chordOf(e);
      update({ keybindings: { [recording]: chord } });
      setRecording(null);
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [recording]); // eslint-disable-line react-hooks/exhaustive-deps

  const list = COMMANDS.filter((c) => `${c.category} ${c.label}`.toLowerCase().includes(filter.toLowerCase()));
  return (
    <>
      <h2 className="dlg-page-title">Keybindings</h2>
      <div className="kb-toolbar">
        <TextInput value={filter} onChange={setFilter} placeholder="Search commands" />
        <Button icon={<RotateCcw size={14} />} onClick={() => update({ keybindings: { ...DEFAULT_KEYBINDINGS } })}>
          Reset all
        </Button>
      </div>
      <p className="muted settings-hint">Click a shortcut and press the new key combination. Esc cancels, Backspace clears.</p>
      <div className="kb-list">
        {list.map((c) => {
          const id = c.id as CommandId;
          const chord = bindings[id] ?? '';
          const isDefault = normalizeChord(chord) === normalizeChord(DEFAULT_KEYBINDINGS[id]);
          return (
            <div key={id} className={cx('kb-row', conflicts.has(id) && 'kb-row--conflict')}>
              <span className="kb-row__cat">{c.category}</span>
              <span className="kb-row__label">{c.label}</span>
              <button className={cx('kb-chord', recording === id && 'kb-chord--recording')} onClick={() => setRecording(recording === id ? null : id)}>
                {recording === id ? 'Press keys…' : chord ? formatChord(normalizeChord(chord)) : <span className="muted">Unassigned</span>}
              </button>
              <Button size="sm" variant="ghost" disabled={isDefault} onClick={() => update({ keybindings: { [id]: DEFAULT_KEYBINDINGS[id] } })} title="Reset to default">
                <RotateCcw size={13} />
              </Button>
            </div>
          );
        })}
      </div>
    </>
  );
}

/* ------------------------------------------------------------- Color schemes */

const SCHEME_FIELDS: (keyof ColorScheme)[] = ['foreground', 'background', 'cursor', 'cursorAccent', 'selectionBackground', ...ANSI_KEYS];

/** Windows Terminal scheme JSON -> ColorScheme. */
export function fromWindowsTerminal(json: unknown): ColorScheme {
  const o = json as Record<string, string>;
  if (!o || typeof o !== 'object' || !o.background || !o.foreground) throw new Error('Not a Windows Terminal color scheme');
  const pick = (k: string, d: string) => (typeof o[k] === 'string' ? o[k] : d);
  const bg = o.background;
  const lum = parseInt(bg.slice(1, 3), 16) * 0.299 + parseInt(bg.slice(3, 5), 16) * 0.587 + parseInt(bg.slice(5, 7), 16) * 0.114;
  return {
    id: newId(),
    name: o.name || 'Imported scheme',
    variant: lum < 128 ? 'dark' : 'light',
    foreground: o.foreground,
    background: bg,
    cursor: pick('cursorColor', o.foreground),
    cursorAccent: bg,
    selectionBackground: pick('selectionBackground', '#44475a'),
    black: pick('black', '#000000'),
    red: pick('red', '#cc0000'),
    green: pick('green', '#4e9a06'),
    yellow: pick('yellow', '#c4a000'),
    blue: pick('blue', '#3465a4'),
    magenta: pick('purple', pick('magenta', '#75507b')),
    cyan: pick('cyan', '#06989a'),
    white: pick('white', '#d3d7cf'),
    brightBlack: pick('brightBlack', '#555753'),
    brightRed: pick('brightRed', '#ef2929'),
    brightGreen: pick('brightGreen', '#8ae234'),
    brightYellow: pick('brightYellow', '#fce94f'),
    brightBlue: pick('brightBlue', '#729fcf'),
    brightMagenta: pick('brightPurple', pick('brightMagenta', '#ad7fa8')),
    brightCyan: pick('brightCyan', '#34e2e2'),
    brightWhite: pick('brightWhite', '#eeeeec')
  };
}

function SchemesPage() {
  const [s, update] = useSettingsState();
  const custom = s.customSchemes;
  const [editId, setEditId] = useState<string | null>(custom[0]?.id ?? null);
  const [importText, setImportText] = useState('');
  const editing = custom.find((c) => c.id === editId);
  const save = (list: ColorScheme[]) => update({ customSchemes: list });
  const duplicate = (src: ColorScheme) => {
    const copy: ColorScheme = { ...src, id: newId(), name: `${src.name} (custom)`, builtIn: false };
    save([...custom, copy]);
    setEditId(copy.id);
  };
  const patch = (p: Partial<ColorScheme>) => editing && save(custom.map((c) => (c.id === editing.id ? { ...c, ...p } : c)));

  return (
    <>
      <h2 className="dlg-page-title">Color schemes</h2>
      <Section title="Create" description="Start from any scheme, or paste a Windows Terminal scheme.">
        <div className="scheme-grid scheme-grid--compact">
          {[...BUILTIN_SCHEMES, ...custom].map((sc) => (
            <SchemeCard key={sc.id} scheme={sc} label={sc.name} active={sc.id === editId} onSelect={() => (sc.builtIn ? duplicate(sc) : setEditId(sc.id))} title={sc.builtIn ? 'Duplicate to customize' : 'Edit'} />
          ))}
        </div>
        <div className="scheme-import">
          <TextArea value={importText} onChange={setImportText} placeholder='{ "name": "My scheme", "background": "#1e1e2e", "foreground": "#cdd6f4", ... }' rows={3} />
          <Button
            icon={<FileUp size={14} />}
            disabled={!importText.trim()}
            onClick={() => {
              try {
                const sc = fromWindowsTerminal(JSON.parse(importText));
                save([...custom, sc]);
                setEditId(sc.id);
                setImportText('');
                toast.success('Scheme imported', sc.name);
              } catch (e) {
                toast.error('Could not import the scheme', errorMessage(e));
              }
            }}
          >
            Import
          </Button>
        </div>
      </Section>
      {editing && (
        <Section
          title="Edit"
          actions={
            <>
              <Button size="sm" icon={<CopyIcon size={13} />} onClick={() => duplicate(editing)}>
                Duplicate
              </Button>
              <Button
                size="sm"
                icon={<Save size={13} />}
                onClick={() => {
                  const { id: _id, builtIn: _b, ...rest } = editing;
                  void api.dialog.saveTextFile(JSON.stringify(rest, null, 2), { title: 'Export color scheme', defaultPath: `${editing.name}.json`, filters: [{ name: 'JSON', extensions: ['json'] }] });
                }}
              >
                Export
              </Button>
              <Button
                size="sm"
                variant="danger"
                icon={<Trash2 size={13} />}
                onClick={() => {
                  save(custom.filter((c) => c.id !== editing.id));
                  setEditId(null);
                  if (s.terminal.colorScheme === editing.id) update({ terminal: { colorScheme: 'auto' } });
                }}
              >
                Delete
              </Button>
            </>
          }
        >
          <FormField label="Name">
            <TextInput value={editing.name} onChange={(name) => patch({ name })} />
          </FormField>
          <div className="scheme-editor">
            <div className="scheme-colors">
              {SCHEME_FIELDS.map((k) => (
                <label key={k} className="scheme-color">
                  <input type="color" value={(editing[k] as string) || '#000000'} onChange={(e) => patch({ [k]: e.target.value } as Partial<ColorScheme>)} />
                  <span>{k}</span>
                </label>
              ))}
            </div>
            <TerminalSample
              scheme={editing}
              height={220}
              font={{ ...s.terminal, boldIsBright: s.terminal.drawBoldTextInBrightColors }}
            />
          </div>
        </Section>
      )}
    </>
  );
}

/* --------------------------------------------------------------- Known hosts */

function KnownHostsPage() {
  const [hosts, setHosts] = useState<KnownHost[]>([]);
  const [filter, setFilter] = useState('');
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const reload = () => void api.knownHosts.list().then(setHosts).catch(() => setHosts([]));
  useEffect(() => {
    reload();
    return api.knownHosts.onChanged(reload);
  }, []);
  const q = filter.toLowerCase();
  const list = hosts.filter((h) => `${h.host} ${h.port} ${h.keyType} ${h.fingerprint}`.toLowerCase().includes(q)).sort((a, b) => a.host.localeCompare(b.host));
  return (
    <>
      <h2 className="dlg-page-title">Known hosts</h2>
      <p className="muted settings-hint">Servers whose host keys you trust. A changed key triggers a security warning when connecting.</p>
      <div className="kb-toolbar">
        <TextInput value={filter} onChange={setFilter} placeholder={`Search ${hosts.length} hosts`} />
        <Button
          icon={<FileUp size={14} />}
          onClick={() =>
            void api.knownHosts
              .importOpenSsh()
              .then((n) => n && toast.success(`Imported ${n} host key${n === 1 ? '' : 's'}`))
              .catch((e) => toast.error('Import failed', errorMessage(e)))
          }
        >
          Import known_hosts…
        </Button>
        <Button
          variant="danger"
          icon={<Trash2 size={14} />}
          disabled={!selected.size}
          onClick={() => void api.knownHosts.remove([...selected]).then(() => setSelected(new Set()))}
        >
          Remove {selected.size || ''}
        </Button>
      </div>
      <div className="dlg-table hosts-table">
        <div className="dlg-table__head hosts-row">
          <span />
          <span>Host</span>
          <span>Key</span>
          <span>Fingerprint</span>
          <span>Source</span>
        </div>
        <div className="dlg-table__body hosts-body">
          {list.map((h) => (
            <label key={h.id} className={cx('dlg-table__row hosts-row', selected.has(h.id) && 'dlg-table__row--selected')}>
              <Checkbox
                checked={selected.has(h.id)}
                onChange={(on) =>
                  setSelected((cur) => {
                    const n = new Set(cur);
                    if (on) n.add(h.id);
                    else n.delete(h.id);
                    return n;
                  })
                }
              />
              <span className="hosts-host" title={`${h.host}:${h.port}`}>
                {h.host}
                {h.port !== 22 && <span className="muted">:{h.port}</span>}
              </span>
              <span className="muted">{h.keyType.replace('ecdsa-sha2-', 'ecdsa-')}</span>
              <span className="mono-text hosts-fp" title={h.fingerprint}>
                {h.fingerprint.replace('SHA256:', '')}
              </span>
              <span className="muted">{h.source === 'putty-import' ? 'PuTTY' : h.source === 'openssh-import' ? 'OpenSSH' : 'You'}</span>
            </label>
          ))}
          {list.length === 0 && <div className="dlg-table__empty">{hosts.length ? 'No hosts match the search.' : 'No trusted host keys yet.'}</div>}
        </div>
      </div>
    </>
  );
}

/* ------------------------------------------------------- Default session / data */

function DefaultsPage({ onClose }: { onClose: () => void }) {
  return (
    <>
      <h2 className="dlg-page-title">Default session</h2>
      <p className="muted settings-hint">
        New sessions and quick connections start from these settings (PuTTY’s “Default Settings”): terminal type, keepalives, keyboard,
        logging, appearance and so on.
      </p>
      <Button
        variant="primary"
        icon={<MonitorCog size={15} />}
        onClick={() => {
          onClose();
          setTimeout(() => useUi.getState().openDialog({ type: 'sessionEditor', sessionId: 'default' }), 0);
        }}
      >
        Edit default session settings…
      </Button>
    </>
  );
}

function DataPage() {
  const [info, setInfo] = useState<{ dir: string; logs: string; portable: boolean } | null>(null);
  useEffect(() => {
    void Promise.all([api.app.info(), api.app.logDirectory()]).then(([i, logs]) => setInfo({ dir: i.userDataDir, logs, portable: i.portable }));
  }, []);
  const [s, update] = useSettingsState();
  return (
    <>
      <h2 className="dlg-page-title">Data & backup</h2>
      <Section title="Storage">
        <SettingRow label="Data folder" description={info?.portable ? 'Portable mode: stored next to the program.' : 'Sessions, settings and trusted host keys.'}>
          <Button size="sm" disabled={!info} onClick={() => info && void api.app.openPath(info.dir)}>
            Open folder
          </Button>
        </SettingRow>
        <div className="dlg-path mono-text">{info?.dir}</div>
        <FormField label="Session log folder" hint="Relative log file paths are stored here. Leave empty for the default.">
          <TextInput value={s.logging.directory} onChange={(directory) => update({ logging: { directory } })} placeholder={info?.logs} />
        </FormField>
      </Section>
      <Section title="Backup">
        <SettingRow label="Export all sessions" description="Passwords are never exported.">
          <Button
            size="sm"
            icon={<Save size={13} />}
            onClick={async () => {
              const ids = (await api.sessions.list()).map((x) => x.id);
              if (!ids.length) return toast.info('There are no saved sessions to export');
              const path = await api.importer.exportJsonFile(ids);
              if (path) toast.success('Sessions exported', path);
            }}
          >
            Export JSON…
          </Button>
        </SettingRow>
        <SettingRow label="Import sessions" description="From PuTTY, OpenSSH config or a JSON backup.">
          <Button size="sm" icon={<Plus size={13} />} onClick={() => useUi.getState().openDialog({ type: 'import', source: 'json' })}>
            Import…
          </Button>
        </SettingRow>
      </Section>
    </>
  );
}
