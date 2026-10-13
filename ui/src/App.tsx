/**
 * Application root. (Placeholder — replaced by the shell implementation.)
 */
import { useEffect, useState } from 'react';
import { initDataStores, useSessions } from '@/store/data';
import { ContextMenuHost, ToastHost } from '@/components/ui';
import { DialogHost } from '@/components/dialogs/DialogHost';

export function App() {
  const [ready, setReady] = useState(false);
  const sessions = useSessions((s) => s.sessions);
  useEffect(() => {
    void initDataStores().then(() => setReady(true));
  }, []);
  return (
    <div style={{ padding: 24 }}>
      <h1>SSH Crawler</h1>
      <p>{ready ? `${sessions.length} saved sessions` : 'Loading…'}</p>
      <DialogHost />
      <ContextMenuHost />
      <ToastHost />
    </div>
  );
}
