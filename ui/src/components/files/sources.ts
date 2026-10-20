/**
 * The two sides of the file manager behind one interface: the local disks (api.local) and the server (api.sftp,
 * which runs over SFTP or SCP). Panes only talk to a PaneSource.
 */
import { api } from '@/bridge/api';
import type { LocalEntry, SftpEntry } from '@shared/types';
import { attributeString, local, remote, setLocalHome, THIS_PC, type FileItem, type Side } from './paths';

export interface PaneSource {
  side: Side;
  root: string;
  home(): Promise<string>;
  list(path: string): Promise<FileItem[]>;
  mkdir(path: string): Promise<void>;
  rename(from: string, to: string): Promise<void>;
  /** Folders are deleted with their contents. */
  remove(items: FileItem[], toRecycleBin: boolean): Promise<void>;
  /** Opens a file with its associated application (remote: via a temporary copy). */
  open(item: FileItem): Promise<void>;
}

export function fromLocal(e: LocalEntry): FileItem {
  return {
    name: e.name,
    path: e.path,
    type: e.type,
    isDir: e.type !== 'file',
    size: e.type === 'drive' ? (e.totalBytes ?? 0) : e.size,
    mtime: e.mtime,
    hidden: e.hidden,
    permissions: e.type === 'drive' || e.type === 'place' ? '' : attributeString(e),
    link: e.link,
    label: e.label,
    freeBytes: e.freeBytes,
    totalBytes: e.totalBytes,
    icon: e.icon
  };
}

export function fromRemote(e: SftpEntry): FileItem {
  return {
    name: e.name,
    path: e.path,
    type: e.type,
    isDir: e.type === 'dir' || (e.type === 'link' && !!e.linkTargetIsDir),
    size: e.type === 'dir' ? 0 : e.size,
    mtime: e.mtime,
    hidden: e.name.startsWith('.'),
    permissions: e.permissions,
    mode: e.mode,
    link: e.type === 'link'
  };
}

export const localSource: PaneSource = {
  side: 'local',
  root: THIS_PC,
  async home() {
    const home = await api.local.home();
    setLocalHome(home);
    return home;
  },
  list: async (path) => (await api.local.list(path)).map(fromLocal),
  mkdir: (path) => api.local.mkdir(path),
  rename: (from, to) => api.local.rename(from, to),
  remove: (items, toRecycleBin) => api.local.remove(items.map((i) => i.path), toRecycleBin),
  open: (item) => api.local.open(item.path)
};

export function remoteSource(sftpId: string, home: string): PaneSource {
  return {
    side: 'remote',
    root: remote.root,
    home: async () => home,
    list: async (path) => (await api.sftp.list(sftpId, path)).map(fromRemote),
    mkdir: (path) => api.sftp.mkdir(sftpId, path),
    rename: (from, to) => api.sftp.rename(sftpId, from, to),
    async remove(items) {
      for (const i of items) await api.sftp.remove(sftpId, i.path, i.type === 'dir');
    },
    open: (item) => api.sftp.openRemote(sftpId, item.path)
  };
}

export { local, remote };
