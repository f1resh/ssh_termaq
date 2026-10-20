/**
 * "File exists" question for a transfer: compares both copies and answers with overwrite / overwrite if newer /
 * resume / rename / skip / cancel, optionally for all remaining conflicts of the same request.
 */
import { useEffect, useState } from 'react';
import { AlertTriangle } from 'lucide-react';
import { Button, Checkbox, Modal } from '@/components/ui';
import { osTerms } from '@shared/platform';
import type { ConflictResolution, TransferConflict } from '@shared/types';
import { fmtDate, fmtSize } from './paths';

export function suggestName(name: string): string {
  const dot = name.lastIndexOf('.');
  return dot > 0 ? `${name.slice(0, dot)} (1)${name.slice(dot)}` : `${name} (1)`;
}

export function ConflictDialog({ conflict, onAnswer }: { conflict: TransferConflict; onAnswer: (r: ConflictResolution) => void }) {
  const [all, setAll] = useState(false);
  const [renaming, setRenaming] = useState(false);
  const [newName, setNewName] = useState(suggestName(conflict.name));

  useEffect(() => {
    setAll(false);
    setRenaming(false);
    setNewName(suggestName(conflict.name));
  }, [conflict.conflictId, conflict.name]);

  const answer = (action: ConflictResolution['action'], name?: string) => onAnswer({ action, newName: name, applyToAll: all });
  const src = conflict.source;
  const dst = conflict.target;
  const srcNewer = src.mtime > dst.mtime + 2000;
  const dstNewer = dst.mtime > src.mtime + 2000;
  const upload = conflict.direction === 'upload';
  const changed = conflict.reason === 'changed';

  return (
    <Modal
      open
      width={560}
      icon={<AlertTriangle size={18} className="fm-warn-icon" />}
      title={changed ? `"${conflict.name}" changed on the server` : `"${conflict.name}" already exists`}
      onClose={() => answer('cancel')}
      dismissable={false}
      footer={
        renaming ? (
          <>
            <Button onClick={() => setRenaming(false)}>Back</Button>
            <Button variant="primary" disabled={!newName.trim() || newName.includes('/')} onClick={() => answer('rename', newName.trim())}>
              Save as this name
            </Button>
          </>
        ) : (
          <>
            {conflict.multiple && <Checkbox className="fm-conflict__all" checked={all} onChange={setAll} label="Do this for all conflicts" />}
            <span className="fm-spacer" />
            <Button variant="ghost" onClick={() => answer('cancel')}>
              Cancel transfer
            </Button>
            <Button onClick={() => answer('skip')}>Skip</Button>
            <Button onClick={() => (all ? answer('rename') : setRenaming(true))}>{all ? 'Keep both' : 'Rename…'}</Button>
            {conflict.canResume && <Button onClick={() => answer('resume')}>Resume</Button>}
            {!changed && <Button onClick={() => answer('newer')}>If newer</Button>}
            <Button variant="primary" autoFocus onClick={() => answer('overwrite')}>
              Overwrite
            </Button>
          </>
        )
      }
    >
      {changed ? (
        <p className="fm-conflict__text">
          Someone changed the server copy after you opened it for editing. Uploading your version replaces their changes.
        </p>
      ) : (
        <p className="fm-conflict__text">
          {upload ? 'The server already has' : `${osTerms.ThisComputer} already has`} a file with this name in the destination folder.
        </p>
      )}
      {renaming ? (
        <label className="field">
          <span className="field__label">New name in the destination folder</span>
          <input
            className="input"
            value={newName}
            autoFocus
            spellCheck={false}
            onChange={(e) => setNewName(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' && newName.trim()) answer('rename', newName.trim());
            }}
          />
        </label>
      ) : (
        <div className="fm-conflict__cards">
          <div className="fm-conflict__card">
            <div className="fm-conflict__label">{upload ? `Your file (${osTerms.thisComputer})` : 'Server file'} — new</div>
            <div className="fm-conflict__path mono" title={src.path}>
              {src.path}
            </div>
            <div className="fm-conflict__facts">
              <span>{fmtSize(src.size)}</span>
              <span className={srcNewer ? 'fm-good' : undefined}>
                {fmtDate(src.mtime) || 'unknown date'}
                {srcNewer ? ' · newer' : ''}
              </span>
            </div>
          </div>
          <div className="fm-conflict__card">
            <div className="fm-conflict__label">{upload ? 'Server file' : `Existing file (${osTerms.thisComputer})`} — replaced</div>
            <div className="fm-conflict__path mono" title={dst.path}>
              {dst.path}
            </div>
            <div className="fm-conflict__facts">
              <span>
                {fmtSize(dst.size)}
                {conflict.canResume ? ' · incomplete' : ''}
              </span>
              <span className={dstNewer ? 'fm-warn' : undefined}>
                {fmtDate(dst.mtime) || 'unknown date'}
                {dstNewer ? ' · newer' : ''}
              </span>
            </div>
          </div>
        </div>
      )}
    </Modal>
  );
}
