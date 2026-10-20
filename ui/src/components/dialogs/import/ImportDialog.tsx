/**
 * Import wizard: 1) choose a source  2) preview + select + options  3) result.
 * Sources: PuTTY sessions on this computer (Windows: the registry; macOS: ~/.putty), PuTTY .reg file, OpenSSH config,
 * SSHTermaq JSON backup.
 */
import { useEffect, useMemo, useState, type ReactNode } from 'react';
import { AlertTriangle, ArrowLeft, CheckCircle2, Download, FileCode2, FileJson, FolderOpen, KeyRound, Monitor } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Checkbox, Modal, Select, TextInput, cx } from '@/components/ui';
import { useSessions } from '@/store/data';
import { useUi } from '@/store/ui';
import { isMac, osTerms } from '@shared/platform';
import type { ImportPreview, ImportResult, ImportSource } from '@shared/types';
import { Badge, Callout, EmptyState, FormField, Spinner } from '../common/controls';
import { errorMessage } from '../common/hooks';
import {
  buildImportRequest,
  candidateAddress,
  candidateExists,
  defaultImportOptions,
  filterCandidates,
  initialSelection,
  isPuttySource,
  SOURCE_LABELS,
  type ImportOptions
} from './importModel';
import './import.css';

type Step = { kind: 'source' } | { kind: 'loading'; source: ImportSource } | { kind: 'preview'; preview: ImportPreview } | { kind: 'importing' } | { kind: 'done'; result: ImportResult };

interface SourceCard {
  source: ImportSource;
  title: string;
  description: string;
  icon: ReactNode;
}

const SOURCES: SourceCard[] = [
  {
    source: 'putty-registry',
    title: `PuTTY on ${osTerms.thisComputer}`,
    description: isMac
      ? 'Saved sessions, default settings and trusted host keys of PuTTY for macOS (~/.putty).'
      : 'Saved sessions, default settings and trusted host keys from the registry.',
    icon: <Monitor size={22} />
  },
  { source: 'putty-regfile', title: 'PuTTY .reg file', description: 'A registry export of PuTTY sessions (e.g. from another computer).', icon: <FileCode2 size={22} /> },
  { source: 'ssh-config', title: 'OpenSSH config', description: 'Hosts from ~/.ssh/config, including ProxyJump and port forwards.', icon: <KeyRound size={22} /> },
  { source: 'json', title: 'SSHTermaq backup', description: 'A JSON file exported from SSHTermaq.', icon: <FileJson size={22} /> }
];

async function loadPreview(source: ImportSource): Promise<ImportPreview | null> {
  switch (source) {
    case 'putty-registry':
      return api.importer.scanPuttyRegistry();
    case 'putty-regfile':
      return api.importer.parsePuttyRegFile();
    case 'ssh-config':
      return api.importer.scanSshConfig();
    case 'json':
      return api.importer.parseJsonFile();
  }
}

export function ImportDialog({ source, onClose }: { source: ImportSource | 'json'; onClose: () => void }) {
  const [step, setStep] = useState<Step>({ kind: 'source' });
  const [error, setError] = useState('');
  const [puttyCount, setPuttyCount] = useState<number | null>(null);

  useEffect(() => {
    api.importer
      .scanPuttyRegistry()
      .then((p) => setPuttyCount(p.candidates.length))
      .catch(() => setPuttyCount(0));
  }, []);

  const start = async (src: ImportSource) => {
    setError('');
    setStep({ kind: 'loading', source: src });
    try {
      const preview = await loadPreview(src);
      if (!preview) {
        setStep({ kind: 'source' });
        return;
      }
      setStep({ kind: 'preview', preview });
    } catch (e) {
      setError(errorMessage(e));
      setStep({ kind: 'source' });
    }
  };

  // Opened from "Import from PuTTY": go straight to the registry preview.
  useEffect(() => {
    if (source === 'putty-registry') void start('putty-registry');
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const title =
    step.kind === 'preview'
      ? `Import from ${SOURCE_LABELS[step.preview.source]}`
      : step.kind === 'done'
        ? 'Import complete'
        : 'Import sessions';

  return (
    <Modal open title={title} onClose={onClose} width={step.kind === 'preview' ? 920 : 640} icon={<Download size={18} className="dlg-title-icon" />} bodyClassName="imp-body" dismissable={step.kind !== 'importing'}
      footer={step.kind === 'preview' ? undefined : step.kind === 'done' ? (
        <>
          <Button onClick={() => setStep({ kind: 'source' })}>Import more</Button>
          <Button variant="primary" onClick={onClose}>Done</Button>
        </>
      ) : (
        <Button onClick={onClose}>Cancel</Button>
      )}
    >
      {error && <Callout tone="danger" title="Import failed">{error}</Callout>}
      {step.kind === 'source' && (
        <div className="imp-sources">
          {SOURCES.map((s) => (
            <button key={s.source} className="imp-source" onClick={() => void start(s.source)}>
              <span className="imp-source__icon">{s.icon}</span>
              <span className="imp-source__text">
                <span className="imp-source__title">
                  {s.title}
                  {s.source === 'putty-registry' && puttyCount !== null && (
                    <Badge tone={puttyCount ? 'accent' : 'neutral'}>{puttyCount ? `${puttyCount} sessions` : 'none found'}</Badge>
                  )}
                </span>
                <span className="imp-source__desc">{s.description}</span>
              </span>
            </button>
          ))}
        </div>
      )}
      {(step.kind === 'loading' || step.kind === 'importing') && (
        <div className="imp-center">
          <Spinner size={22} label={step.kind === 'loading' ? 'Reading sessions…' : 'Importing…'} />
        </div>
      )}
      {step.kind === 'preview' && (
        <PreviewStep
          preview={step.preview}
          onBack={() => setStep({ kind: 'source' })}
          onImport={async (req) => {
            setStep({ kind: 'importing' });
            try {
              setStep({ kind: 'done', result: await api.importer.apply(req) });
            } catch (e) {
              setError(errorMessage(e));
              setStep({ kind: 'preview', preview: step.preview });
            }
          }}
        />
      )}
      {step.kind === 'done' && <ResultStep result={step.result} />}
    </Modal>
  );
}

function PreviewStep({
  preview,
  onBack,
  onImport
}: {
  preview: ImportPreview;
  onBack: () => void;
  onImport: (req: ReturnType<typeof buildImportRequest>) => void;
}) {
  const sessions = useSessions((s) => s.sessions);
  const folders = useSessions((s) => s.folders);
  const [selected, setSelected] = useState(() => initialSelection(preview));
  const [opts, setOpts] = useState<ImportOptions>(() => defaultImportOptions(preview));
  const [filter, setFilter] = useState('');
  const [expanded, setExpanded] = useState<string | null>(null);
  const visible = useMemo(() => filterCandidates(preview.candidates, filter), [preview, filter]);
  const conflicts = useMemo(
    () => preview.candidates.filter((c) => selected.has(c.key) && candidateExists(c, sessions, opts.targetFolder)).length,
    [preview, selected, sessions, opts.targetFolder]
  );
  const allVisibleSelected = visible.length > 0 && visible.every((c) => selected.has(c.key));
  const toggleAll = () =>
    setSelected((cur) => {
      const next = new Set(cur);
      for (const c of visible) allVisibleSelected ? next.delete(c.key) : next.add(c.key);
      return next;
    });
  const puttySrc = isPuttySource(preview.source);
  const nothing = preview.candidates.length === 0 && preview.hostKeys.length === 0;

  return (
    <div className="imp-preview">
      <div className="imp-preview__origin muted" title={preview.origin}>
        {preview.origin}
      </div>
      {preview.errors.length > 0 && (
        <Callout tone="warning" title={`${preview.errors.length} note${preview.errors.length === 1 ? '' : 's'}`}>
          <ul className="imp-errors">
            {preview.errors.map((e, i) => (
              <li key={i}>{e}</li>
            ))}
          </ul>
        </Callout>
      )}
      {nothing ? (
        <EmptyState icon={<FolderOpen size={28} />} title="Nothing to import">
          No sessions were found in this source.
        </EmptyState>
      ) : (
        <>
          <div className="imp-toolbar">
            <TextInput value={filter} onChange={setFilter} placeholder="Filter sessions" className="imp-filter" />
            <span className="muted">
              {selected.size} of {preview.candidates.length} selected
            </span>
          </div>
          <div className="dlg-table imp-table">
            <div className="dlg-table__head imp-row">
              <span>
                <Checkbox checked={allVisibleSelected} onChange={toggleAll} />
              </span>
              <span>Name</span>
              <span>Address</span>
              <span>Protocol</span>
              <span />
            </div>
            <div className="dlg-table__body imp-table__body">
              {visible.map((c) => {
                const exists = candidateExists(c, sessions, opts.targetFolder);
                const open = expanded === c.key;
                return (
                  <div key={c.key} className={cx('imp-item', !selected.has(c.key) && 'imp-item--off')}>
                    <div className="dlg-table__row imp-row">
                      <span>
                        <Checkbox
                          checked={selected.has(c.key)}
                          onChange={(on) =>
                            setSelected((cur) => {
                              const next = new Set(cur);
                              if (on) next.add(c.key);
                              else next.delete(c.key);
                              return next;
                            })
                          }
                        />
                      </span>
                      <span className="imp-name" title={c.name}>
                        {c.name}
                      </span>
                      <span className="mono-text imp-addr" title={candidateAddress(c.session)}>
                        {candidateAddress(c.session)}
                      </span>
                      <span className="imp-proto">{c.session.protocol}</span>
                      <span className="imp-badges">
                        {exists && <Badge tone="warning" title="A saved session with this name already exists in the target folder">exists</Badge>}
                        {c.warnings.length > 0 && (
                          <button className="link-btn" onClick={() => setExpanded(open ? null : c.key)} title="Show import notes">
                            <AlertTriangle size={13} /> {c.warnings.length}
                          </button>
                        )}
                      </span>
                    </div>
                    {open && (
                      <ul className="imp-warnings">
                        {c.warnings.map((w, i) => (
                          <li key={i}>{w}</li>
                        ))}
                      </ul>
                    )}
                  </div>
                );
              })}
              {visible.length === 0 && <div className="dlg-table__empty">No sessions match the filter.</div>}
            </div>
          </div>

          <div className="imp-options">
            <FormField label="Import into folder" hint="Leave empty for the top level">
              <TextInput
                value={opts.targetFolder}
                onChange={(targetFolder) => setOpts({ ...opts, targetFolder })}
                placeholder="(top level)"
                list="imp-folders"
              />
              <datalist id="imp-folders">
                {folders.map((f) => (
                  <option key={f} value={f} />
                ))}
              </datalist>
            </FormField>
            <FormField label="When a session already exists" hint={conflicts ? `${conflicts} of the selected sessions already exist` : 'No conflicts'}>
              <Select
                value={opts.conflict}
                onChange={(conflict) => setOpts({ ...opts, conflict })}
                options={[
                  { value: 'rename', label: 'Keep both (rename the imported one)' },
                  { value: 'overwrite', label: 'Replace the saved session' },
                  { value: 'skip', label: 'Skip it' }
                ]}
              />
            </FormField>
            <div className="imp-checks">
              {preview.hostKeys.length > 0 && (
                <Checkbox
                  checked={opts.importHostKeys}
                  onChange={(importHostKeys) => setOpts({ ...opts, importHostKeys })}
                  label={`Import ${preview.hostKeys.length} trusted host key${preview.hostKeys.length === 1 ? '' : 's'}`}
                />
              )}
              {puttySrc && preview.defaultSettings && (
                <Checkbox
                  checked={opts.useDefaultSettings}
                  onChange={(useDefaultSettings) => setOpts({ ...opts, useDefaultSettings })}
                  label={'Use PuTTY’s "Default Settings" as the template for new sessions'}
                />
              )}
            </div>
          </div>
        </>
      )}
      <div className="modal__footer imp-footer">
        <Button icon={<ArrowLeft size={14} />} onClick={onBack}>
          Back
        </Button>
        <span className="imp-footer__spacer" />
        <Button
          variant="primary"
          disabled={selected.size === 0 && !(opts.importHostKeys && preview.hostKeys.length) && !opts.useDefaultSettings}
          onClick={() => onImport(buildImportRequest(preview, selected, opts))}
        >
          Import {selected.size ? `${selected.size} session${selected.size === 1 ? '' : 's'}` : ''}
        </Button>
      </div>
    </div>
  );
}

function ResultStep({ result }: { result: ImportResult }) {
  const openImported = () => useUi.getState().closeDialog();
  const rows: [string, number][] = [
    ['Imported', result.imported],
    ['Replaced', result.overwritten],
    ['Skipped', result.skipped],
    ['Host keys', result.hostKeysImported]
  ];
  return (
    <div className="imp-result">
      <CheckCircle2 size={40} className="imp-result__icon" />
      <div className="imp-result__stats">
        {rows.map(([label, n]) => (
          <div key={label} className="imp-stat">
            <span className="imp-stat__n">{n}</span>
            <span className="imp-stat__label">{label}</span>
          </div>
        ))}
      </div>
      {result.errors.length > 0 && (
        <Callout tone="warning" title="Some items need attention">
          <ul className="imp-errors">
            {result.errors.map((e, i) => (
              <li key={i}>{e}</li>
            ))}
          </ul>
        </Callout>
      )}
      <p className="muted">Imported sessions are now in the sidebar.</p>
      <Button size="sm" variant="ghost" onClick={openImported}>
        Show sessions
      </Button>
    </div>
  );
}
