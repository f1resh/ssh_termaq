import { describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import { deserializeWorkspace, serializeWorkspace } from './workspacePersist';
import { connIdOf, type PaneState, type TabState } from './workspace';

function pane(id: string, tabId: string, extra: Partial<PaneState> = {}): PaneState {
  return {
    id,
    tabId,
    kind: 'terminal',
    sessionId: null,
    session: createDefaultSession({ host: `${id}.example.test` }),
    generation: 1,
    connId: connIdOf(id, 1),
    status: 'connected',
    hasActivity: false,
    bell: false,
    ...extra
  };
}

describe('workspace persistence with file manager tabs', () => {
  it('keeps the kind and folders, drops the terminal link', () => {
    const panes: Record<string, PaneState> = {
      t1: pane('t1', 'A'),
      f1: pane('f1', 'B', { kind: 'files', linkedPaneId: 't1', files: { localPath: 'C:\\work', remotePath: '/srv/www' } })
    };
    const tabs: TabState[] = [
      { id: 'A', layout: { type: 'pane', paneId: 't1' }, activePaneId: 't1', broadcast: false },
      { id: 'B', layout: { type: 'pane', paneId: 'f1' }, activePaneId: 'f1', broadcast: false }
    ];
    const snap = serializeWorkspace({ tabs, panes, activeTabId: 'B' }, () => false);
    expect(snap.activeTab).toBe(1);
    const filesNode = snap.tabs[1].layout;
    expect(filesNode.type === 'pane' && filesNode.pane.kind).toBe('files');

    const restored = deserializeWorkspace(JSON.parse(JSON.stringify(snap)), () => undefined);
    expect(restored.tabs).toHaveLength(2);
    const restoredFiles = Object.values(restored.panes).find((p) => p.kind === 'files')!;
    expect(restoredFiles.files).toEqual({ localPath: 'C:\\work', remotePath: '/srv/www' });
    expect(restoredFiles.linkedPaneId).toBeUndefined();
    expect(restoredFiles.status).toBe('idle');
    expect(Object.values(restored.panes).filter((p) => p.kind === 'terminal')).toHaveLength(1);
  });

  it('treats old snapshots as terminals', () => {
    const restored = deserializeWorkspace(
      { version: 1, activeTab: 0, tabs: [{ layout: { type: 'pane', pane: { sessionId: null, session: createDefaultSession({ host: 'h' }) } }, activePane: 0 }] },
      () => undefined
    );
    expect(Object.values(restored.panes)[0].kind).toBe('terminal');
  });
});
