/**
 * One terminal pane bound to a workspace pane (connection). (Placeholder — replaced by the terminal implementation.)
 */
export interface TerminalPaneProps {
  paneId: string;
  /** Pane has keyboard focus within its tab (the tab's activePaneId). */
  active: boolean;
  /** Pane's tab is the visible tab. */
  visible: boolean;
}

export function TerminalPane({ paneId }: TerminalPaneProps) {
  return <div data-pane={paneId} style={{ width: '100%', height: '100%', background: '#000' }} />;
}
