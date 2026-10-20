/**
 * Session editor (PuTTY's configuration dialog, modernised): a category tree on the left, grouped fields on
 * the right, protocol-aware (irrelevant categories are hidden). Edits a local draft; nothing is stored until
 * Save. Opening with sessionId 'default' edits the "Default Settings" template (settings.defaultSession).
 */
import { useEffect, useMemo, useRef, useState, type CSSProperties, type ReactNode } from 'react';
import {
  Cable,
  Fingerprint,
  Globe,
  Keyboard,
  Languages,
  Lock,
  MonitorCog,
  Network,
  Palette,
  Plug,
  ScrollText,
  Settings2,
  Shield,
  SquareTerminal,
  Usb,
  Waypoints
} from 'lucide-react';
import { api } from '@/bridge/api';
import { connectSession } from '@/commands/actions';
import { Button, Modal, toast } from '@/components/ui';
import { useSessions, useSettings } from '@/store/data';
import type { DialogState } from '@/store/ui';
import type { ColorScheme, SessionConfig } from '@shared/types';
import { NavList, type NavItem } from '../common/controls';
import { CloseGuardContext, errorMessage, useCloseGuards, useStableCallback } from '../common/hooks';
import { useNestedConfirm } from '../common/NestedConfirm';
import { EditorContext, getPath, type EditorContextValue, type PasswordChange } from './editorContext';
import {
  SESSION_SECTIONS,
  defaultSessionPatch,
  finalizeSession,
  isSectionId,
  sectionOfField,
  sectionsWithErrors,
  sessionFromTemplate,
  stableStringify,
  validateSession,
  visibleSections,
  type SectionId
} from './sessionModel';
import { ConnectionSection, ProxySection, SessionSection } from './sections/GeneralSections';
import { RloginSection, SerialSection, TelnetSection } from './sections/ProtocolSections';
import { AlgorithmsSection, AuthSection, SshSection, TunnelsSection, X11Section } from './sections/SshSections';
import { AppearanceSection, KeyboardSection, LoggingSection, TerminalSection, TranslationSection } from './sections/TerminalSections';

export type SessionEditorState = Extract<DialogState, { type: 'sessionEditor' }>;

const SECTION_ICONS: Record<SectionId, ReactNode> = {
  session: <Plug size={15} />,
  connection: <Network size={15} />,
  proxy: <Globe size={15} />,
  ssh: <Lock size={15} />,
  'ssh.auth': <Fingerprint size={15} />,
  'ssh.tunnels': <Waypoints size={15} />,
  'ssh.x11': <MonitorCog size={15} />,
  'ssh.algorithms': <Shield size={15} />,
  telnet: <Network size={15} />,
  rlogin: <Cable size={15} />,
  serial: <Usb size={15} />,
  terminal: <SquareTerminal size={15} />,
  keyboard: <Keyboard size={15} />,
  appearance: <Palette size={15} />,
  translation: <Languages size={15} />,
  logging: <ScrollText size={15} />
};

const SECTION_DESCRIPTIONS: Record<SectionId, string> = {
  session: 'Where to connect and how this session is listed',
  connection: 'Keepalives, TCP options, terminal identity and environment',
  proxy: 'Reach the host through a proxy or an SSH jump host',
  ssh: 'Remote command, shell and compression',
  'ssh.auth': 'Private keys, agents and authentication methods',
  'ssh.tunnels': 'Local, remote and dynamic (SOCKS) port forwarding',
  'ssh.x11': 'Forward X11 applications to a local X server',
  'ssh.algorithms': 'Preferred key exchange, cipher, host key and MAC algorithms',
  telnet: 'Telnet option negotiation and keyboard behaviour',
  rlogin: 'Rlogin login options',
  serial: 'Serial line parameters',
  terminal: 'Line discipline, bell, scrollback and window behaviour',
  keyboard: 'Codes sent by Backspace, Home/End, function and cursor keys',
  appearance: 'Font, cursor and colours for this session',
  translation: 'Character set of the remote host',
  logging: 'Record session output to a file'
};

function sectionTitle(id: SectionId): string {
  const def = SESSION_SECTIONS.find((s) => s.id === id);
  if (!def) return id;
  return def.depth === 1 ? `SSH › ${def.label}` : def.label;
}

function SectionBody({ id }: { id: SectionId }) {
  switch (id) {
    case 'session':
      return <SessionSection />;
    case 'connection':
      return <ConnectionSection />;
    case 'proxy':
      return <ProxySection />;
    case 'ssh':
      return <SshSection />;
    case 'ssh.auth':
      return <AuthSection />;
    case 'ssh.tunnels':
      return <TunnelsSection />;
    case 'ssh.x11':
      return <X11Section />;
    case 'ssh.algorithms':
      return <AlgorithmsSection />;
    case 'telnet':
      return <TelnetSection />;
    case 'rlogin':
      return <RloginSection />;
    case 'serial':
      return <SerialSection />;
    case 'terminal':
      return <TerminalSection />;
    case 'keyboard':
      return <KeyboardSection />;
    case 'appearance':
      return <AppearanceSection />;
    case 'translation':
      return <TranslationSection />;
    case 'logging':
      return <LoggingSection />;
  }
}

interface InitialState {
  initial: SessionConfig;
  isDefault: boolean;
  isNew: boolean;
}

function computeInitial(dialog: SessionEditorState): InitialState {
  const { settings } = useSettings.getState();
  const sessions = useSessions.getState();
  if (dialog.sessionId === 'default') {
    return { initial: structuredClone(dialog.draft ?? settings.defaultSession), isDefault: true, isNew: false };
  }
  const existing = sessions.byId(dialog.sessionId ?? dialog.draft?.id);
  if (dialog.draft) {
    const draft = structuredClone(dialog.draft);
    if (dialog.folder !== undefined && !existing) draft.folder = dialog.folder;
    return { initial: draft, isDefault: false, isNew: !existing };
  }
  if (existing) return { initial: structuredClone(existing), isDefault: false, isNew: false };
  return {
    initial: sessionFromTemplate(settings.defaultSession, { folder: dialog.folder ?? '' }),
    isDefault: false,
    isNew: true
  };
}

export function SessionEditor({ dialog, onClose }: { dialog: SessionEditorState; onClose: () => void }) {
  const [{ initial, isDefault, isNew }] = useState(() => computeInitial(dialog));
  const [draft, setDraft] = useState<SessionConfig>(() => structuredClone(initial));
  const [attempted, setAttempted] = useState(false);
  const [busy, setBusy] = useState<false | 'save' | 'connect'>(false);
  const [stashedCustomScheme, setStashedCustomScheme] = useState<ColorScheme | undefined>(initial.appearance.customScheme);
  const [passwordChange, setPasswordChange] = useState<PasswordChange | null>(null);
  const settings = useSettings((s) => s.settings);
  const sessions = useSessions((s) => s.sessions);
  const folders = useSessions((s) => s.folders);
  const nested = useNestedConfirm();
  const contentRef = useRef<HTMLDivElement>(null);

  const sections = useMemo(() => visibleSections(draft.protocol, isDefault), [draft.protocol, isDefault]);
  const [active, setActive] = useState<SectionId>(() => (isSectionId(dialog.section) ? dialog.section : 'session'));
  const activeVisible = sections.some((s) => s.id === active) ? active : 'session';
  useEffect(() => {
    if (activeVisible !== active) setActive(activeVisible);
  }, [activeVisible, active]);
  useEffect(() => {
    contentRef.current?.scrollTo({ top: 0 });
  }, [activeVisible]);

  const validation = useMemo(() => validateSession(draft, { sessions, isDefault }), [draft, sessions, isDefault]);
  const errorCount = Object.keys(validation.errors).length;
  const errorSections = useMemo(
    () => (attempted ? sectionsWithErrors(validation.errors, draft.protocol) : new Set<SectionId>()),
    [attempted, validation.errors, draft.protocol]
  );

  const dirty = useMemo(
    () => stableStringify(draft) !== stableStringify(initial) || passwordChange !== null,
    [draft, initial, passwordChange]
  );

  const update = useStableCallback((mutate: (d: SessionConfig) => void) => {
    setDraft((prev) => {
      const next = structuredClone(prev);
      mutate(next);
      return next;
    });
  });

  // Errors for untouched fields stay hidden until the first save attempt; list rows (tunnels, env) always show.
  const fieldError = useStableCallback((path: string): string | undefined => {
    const msg = validation.errors[path];
    if (!msg) return undefined;
    if (attempted || path.startsWith('tunnel:') || path.startsWith('env:')) return msg;
    return stableStringify(getPath(draft, path)) !== stableStringify(getPath(initial, path)) ? msg : undefined;
  });
  const warning = useStableCallback((path: string) => validation.warnings[path]);

  const ctx: EditorContextValue = {
    draft,
    initial,
    update,
    fieldError,
    warning,
    errors: validation.errors,
    isDefault,
    isNew,
    settings,
    sessions,
    folders,
    stashedCustomScheme,
    setStashedCustomScheme,
    passwordChange,
    setPasswordChange,
    confirm: nested.confirm
  };

  const requestClose = useStableCallback(async () => {
    if (busy) return;
    if (dirty) {
      const discard = await nested.confirm({
        title: 'Discard changes?',
        message: isDefault
          ? 'Your changes to the Default Settings have not been saved.'
          : `Your changes to “${draft.name.trim() || (isNew ? 'the new session' : initial.name)}” have not been saved.`,
        confirmLabel: 'Discard changes',
        cancelLabel: 'Keep editing',
        danger: true
      });
      if (!discard) return;
    }
    onClose();
  });
  const { registry, guardedClose } = useCloseGuards(() => void requestClose());

  const save = async (connect: boolean) => {
    if (busy) return;
    setAttempted(true);
    if (errorCount > 0) {
      const first = Object.keys(validation.errors)[0];
      const target = sectionOfField(first, draft.protocol);
      if (sections.some((s) => s.id === target)) setActive(target);
      return;
    }
    setBusy(connect ? 'connect' : 'save');
    try {
      const final = finalizeSession(draft, isDefault);
      if (isDefault) {
        final.id = 'default';
        await useSettings.getState().update({ defaultSession: defaultSessionPatch(initial, final) });
        toast.success('Default Settings saved');
        onClose();
        return;
      }
      const stored = await api.sessions.save(final);
      if (passwordChange) {
        try {
          if (passwordChange.action === 'set') await api.credentials.setPassword(stored.id, passwordChange.value);
          else await api.credentials.clearPassword(stored.id);
        } catch (e) {
          toast.error('Could not update the saved password', errorMessage(e));
        }
      }
      await useSessions.getState().reload();
      onClose();
      if (connect) connectSession(stored);
    } catch (e) {
      toast.error('Could not save the session', errorMessage(e));
      setBusy(false);
    }
  };

  const navItems: NavItem[] = sections.map((s) => ({
    id: s.id,
    label: s.label,
    group: s.group,
    depth: s.depth,
    icon: SECTION_ICONS[s.id],
    error: errorSections.has(s.id)
  }));

  const title = isDefault ? 'Default Settings' : isNew ? 'New session' : `Edit session`;
  const subtitle = isDefault ? 'Template for new sessions' : draft.name.trim() || initial.name || '';

  return (
    <CloseGuardContext.Provider value={registry}>
      <EditorContext.Provider value={ctx}>
        <Modal
          open
          dismissable={false}
          width={960}
          height="min(680px, calc(100vh - 48px))"
          onClose={guardedClose}
          bodyClassName="dlg-body--flush"
          icon={
            draft.color ? (
              <span className="se-title-dot" style={{ '--swatch': draft.color } as CSSProperties} aria-hidden />
            ) : isDefault ? (
              <Settings2 size={18} className="dlg-title-icon" />
            ) : (
              <SquareTerminal size={18} className="dlg-title-icon" />
            )
          }
          title={
            <span className="se-title">
              {title}
              {subtitle && !isNew && <span className="se-title__sub">{subtitle}</span>}
            </span>
          }
          footer={
            <>
              <div className={attempted && errorCount > 0 ? 'dlg-footer__status dlg-footer__status--error' : 'dlg-footer__status'}>
                {attempted && errorCount > 0 ? (
                  <span>
                    {errorCount === 1 ? '1 field needs attention' : `${errorCount} fields need attention`}
                  </span>
                ) : dirty ? (
                  <>
                    <span className="se-dirty-dot" aria-hidden />
                    <span>Unsaved changes</span>
                  </>
                ) : null}
              </div>
              <Button onClick={guardedClose} disabled={!!busy}>
                Cancel
              </Button>
              <Button variant={isDefault ? 'primary' : 'default'} onClick={() => void save(false)} disabled={!!busy}>
                {busy === 'save' ? 'Saving…' : isDefault ? 'Save defaults' : 'Save'}
              </Button>
              {!isDefault && (
                <Button variant="primary" onClick={() => void save(true)} disabled={!!busy}>
                  {busy === 'connect' ? 'Connecting…' : 'Save & Connect'}
                </Button>
              )}
            </>
          }
        >
          <div className="dlg-split">
            <div className="dlg-split__nav">
              <NavList items={navItems} active={activeVisible} onSelect={(id) => setActive(id as SectionId)} ariaLabel="Session categories" />
            </div>
            <div className="dlg-split__content" ref={contentRef}>
              <div className="dlg-page-title">
                <h2>{sectionTitle(activeVisible)}</h2>
                <p>{SECTION_DESCRIPTIONS[activeVisible]}</p>
              </div>
              <SectionBody id={activeVisible} />
            </div>
          </div>
        </Modal>
        {nested.element}
      </EditorContext.Provider>
    </CloseGuardContext.Provider>
  );
}
