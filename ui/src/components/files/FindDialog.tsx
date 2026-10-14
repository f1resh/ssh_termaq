/**
 * Recursive search on the server by name pattern. Results stream in while the search runs; a result can be shown
 * in its folder, downloaded or edited.
 */
import { useEffect, useRef, useState } from 'react';
import { File as FileIcon, Folder, Search, Square } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Checkbox, Field, Modal, TextInput, cx, showContextMenu, toast } from '@/components/ui';
import type { SftpEntry } from '@shared/types';
import { fmtDate, fmtSize, remote } from './paths';

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

export function FindDialog(props: {
  sftpId: string;
  root: string;
  onClose: () => void;
  /** Show a result in the server pane (folder + selection). */
  onReveal: (path: string) => void;
  onDownload: (paths: string[]) => void;
  onEdit: (path: string) => void;
}) {
  const { sftpId, onClose, onReveal, onDownload, onEdit } = props;
  const [root, setRoot] = useState(props.root);
  const [pattern, setPattern] = useState('');
  const [caseSensitive, setCaseSensitive] = useState(false);
  const [includeHidden, setIncludeHidden] = useState(false);
  const [findId, setFindId] = useState<string | null>(null);
  const [running, setRunning] = useState(false);
  const [results, setResults] = useState<SftpEntry[]>([]);
  const [scanned, setScanned] = useState(0);
  const [currentDir, setCurrentDir] = useState('');
  const [note, setNote] = useState('');
  const [selected, setSelected] = useState<string | null>(null);
  const findRef = useRef<string | null>(null);

  useEffect(
    () =>
      api.sftp.onFind((e) => {
        if (e.findId !== findRef.current) return;
        if (e.results.length) setResults((r) => [...r, ...e.results]);
        setScanned(e.scannedDirs);
        if (e.currentDir) setCurrentDir(e.currentDir);
        if (e.done) {
          setRunning(false);
          setCurrentDir('');
          setNote(e.error ? e.error : e.truncated ? 'Stopped after the maximum number of results.' : '');
        }
      }),
    []
  );

  useEffect(
    () => () => {
      if (findRef.current) api.sftp.cancelFind(findRef.current);
    },
    []
  );

  const start = async () => {
    if (!pattern.trim()) return;
    if (findRef.current) api.sftp.cancelFind(findRef.current);
    setResults([]);
    setScanned(0);
    setNote('');
    setSelected(null);
    setRunning(true);
    try {
      const id = await api.sftp.find(sftpId, { root: root.trim() || '/', pattern, caseSensitive, includeHidden });
      findRef.current = id;
      setFindId(id);
    } catch (e) {
      setRunning(false);
      toast.error('Search failed', errorText(e));
    }
  };

  const stop = () => {
    if (findId) api.sftp.cancelFind(findId);
  };

  const menu = (x: number, y: number, r: SftpEntry) => {
    const isDir = r.type === 'dir';
    showContextMenu(x, y, [
      { label: 'Show in folder', onClick: () => onReveal(r.path) },
      { label: 'Download', onClick: () => onDownload([r.path]) },
      { label: 'Edit', disabled: isDir, onClick: () => onEdit(r.path) },
      { type: 'separator' },
      { label: 'Copy path', onClick: () => api.app.writeClipboard(r.path) }
    ]);
  };

  return (
    <Modal
      open
      width={760}
      height={560}
      icon={<Search size={18} />}
      title="Find files on the server"
      onClose={onClose}
      bodyClassName="fm-find__body"
      footer={
        <>
          <span className="fm-find__status">
            {running
              ? `Searching… ${scanned} folder${scanned === 1 ? '' : 's'}${currentDir ? ` · ${currentDir}` : ''}`
              : findId
                ? `${results.length} found in ${scanned} folder${scanned === 1 ? '' : 's'}${note ? ` · ${note}` : ''}`
                : ''}
          </span>
          <span className="fm-spacer" />
          <Button disabled={!selected} onClick={() => selected && onReveal(selected)}>
            Show in folder
          </Button>
          <Button onClick={onClose}>Close</Button>
        </>
      }
    >
      <form
        className="fm-find__form"
        onSubmit={(e) => {
          e.preventDefault();
          if (running) stop();
          else void start();
        }}
      >
        <Field label="Name" hint='Wildcards * and ?, several patterns separated by ";"'>
          <TextInput value={pattern} onChange={setPattern} placeholder="*.log; config*" autoFocus />
        </Field>
        <Field label="In folder">
          <TextInput value={root} onChange={setRoot} className="mono" />
        </Field>
        <div className="fm-find__opts">
          <Checkbox checked={caseSensitive} onChange={setCaseSensitive} label="Match case" />
          <Checkbox checked={includeHidden} onChange={setIncludeHidden} label="Include hidden" />
          <span className="fm-spacer" />
          <Button type="submit" variant={running ? 'default' : 'primary'} icon={running ? <Square size={13} /> : <Search size={13} />} disabled={!running && !pattern.trim()}>
            {running ? 'Stop' : 'Search'}
          </Button>
        </div>
      </form>
      <div className="fm-find__results" role="grid">
        {results.length === 0 && !running && findId && <div className="fm-state fm-state--muted">No matches.</div>}
        {results.map((r) => (
          <div
            key={r.path}
            role="row"
            className={cx('fm-find__row', selected === r.path && 'fm-row--selected')}
            onMouseDown={() => setSelected(r.path)}
            onDoubleClick={() => onReveal(r.path)}
            onContextMenu={(e) => {
              e.preventDefault();
              setSelected(r.path);
              menu(e.clientX, e.clientY, r);
            }}
            title={r.path}
          >
            {r.type === 'dir' ? <Folder size={14} className="fm-icon fm-icon--dir" /> : <FileIcon size={14} className="fm-icon" />}
            <span className="fm-find__name">{r.name}</span>
            <span className="fm-find__dir mono">{remote.parent(r.path) ?? '/'}</span>
            <span className="fm-find__size">{r.type === 'dir' ? '' : fmtSize(r.size)}</span>
            <span className="fm-find__date">{fmtDate(r.mtime)}</span>
          </div>
        ))}
      </div>
    </Modal>
  );
}
