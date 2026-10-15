import { beforeEach, describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { useWorkspace, type PaneState } from '@/store/workspace';
import { focusTabPane, liveSftpPanels, toggleSftpPanel, toggleSidebar } from './actions';

function reset() {
  useWorkspace.setState({ tabs: [], panes: {}, activeTabId: null });
  useUi.setState({ sftpPanels: {}, focusRequest: null });
}

const open = (session: Partial<SessionConfig> = {}, view?: 'terminal' | 'files') =>
  useWorkspace.getState().openSession(createDefaultSession({ host: 'ssh.example.test', ...session }), { view });
const tabOf = (paneId: string) => useWorkspace.getState().panes[paneId].tabId;

describe('SFTP panel per tab', () => {
  beforeEach(reset);

  it('opens on an SSH terminal and closes again', () => {
    const p = open();
    toggleSftpPanel(p);
    expect(useUi.getState().sftpPanels).toEqual({ [tabOf(p)]: p });
    toggleSftpPanel(p);
    expect(useUi.getState().sftpPanels).toEqual({});
  });

  it('switches to another pane of the same tab', () => {
    const first = open();
    const second = useWorkspace.getState().splitPane(first, 'row');
    toggleSftpPanel(first);
    toggleSftpPanel(second);
    expect(useUi.getState().sftpPanels).toEqual({ [tabOf(first)]: second });
  });

  it('keeps one panel per tab', () => {
    const a = open({ host: 'a.example.test' });
    const b = open({ host: 'b.example.test' });
    toggleSftpPanel(a);
    toggleSftpPanel(b);
    expect(useUi.getState().sftpPanels).toEqual({ [tabOf(a)]: a, [tabOf(b)]: b });
  });

  it('is not offered for Telnet sessions or file manager tabs', () => {
    toggleSftpPanel(open({ protocol: 'telnet', port: 23 }));
    toggleSftpPanel(open({}, 'files'));
    toggleSftpPanel('no-such-pane');
    toggleSftpPanel(undefined);
    expect(useUi.getState().sftpPanels).toEqual({});
  });

  it('forgets panels whose tab or pane is gone', () => {
    const panes = { a: { tabId: 'T1' }, b: { tabId: 'T3' } } as unknown as Record<string, PaneState>;
    // T2: its pane closed; T3: points at a pane that now lives in another tab.
    expect(liveSftpPanels({ T1: 'a', T2: 'closed', T3: 'a' }, panes)).toEqual({ T1: 'a' });
  });
});

describe('focus after clicking a tab', () => {
  beforeEach(reset);

  it("asks the tab's active pane for focus, with a new request every time", () => {
    const first = open();
    const second = useWorkspace.getState().splitPane(first, 'column');
    useWorkspace.getState().activatePane(second);
    const tabId = tabOf(first);

    focusTabPane(tabId);
    expect(useUi.getState().focusRequest).toEqual({ paneId: second, seq: 1 });
    focusTabPane(tabId);
    expect(useUi.getState().focusRequest).toEqual({ paneId: second, seq: 2 });

    focusTabPane('no-such-tab');
    expect(useUi.getState().focusRequest?.seq).toBe(2);
  });
});

describe('sidebar button', () => {
  it('shows and hides the session sidebar', async () => {
    const before = useSettings.getState().settings.appearance.showSidebar;
    await toggleSidebar();
    expect(useSettings.getState().settings.appearance.showSidebar).toBe(!before);
    await toggleSidebar();
    expect(useSettings.getState().settings.appearance.showSidebar).toBe(before);
  });
});
