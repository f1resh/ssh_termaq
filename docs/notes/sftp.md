# sftp module — notes

Owner files: `native/src/{sftp,localfs,dnd}/**`, `native/tests/{sftp,localfs,dnd}/**`, `ui/src/components/files/**`,
`ui/src/bridge/mock/{sftp,local}.ts`.

## Status

- [x] RemoteFs over SFTP (`SftpFs`) and SCP (`ScpFs`: `sh -c` scripts + `ls` parsing + scp protocol), auto fallback
- [x] TransferEngine: per-session queue, conflicts (ask / overwrite / newer / skip / resume / rename, apply to all),
      resume (.sc-partial, SFTP offset writes), move, synchronize deletes, progress with file counts
- [x] Find (glob, streamed results), Synchronize (pure compare + walkers, apply via the queue)
- [x] Edit with auto-upload (temp copy watcher, "changed on the server" question)
- [x] Local disks (`localfs`): This PC, listings, IFileOperation copy / move / delete (Recycle Bin), folder watchers
- [x] Drag & drop (`dnd`): virtual files to Explorer from an MTA data object, local files via the shell's data
      object, internal drops reported to the page
- [x] UI: file manager tab (`components/files`), mocks, settings page, session editor fields, sftp:// / scp://
- [x] Tests: native (sftp, localfs, dnd), UI (paths, quick connect, persistence), real-app E2E over SFTP and SCP

## Manual checks still worth doing with a real mouse

Dragging files from the server pane onto the desktop / an Explorer window and between the two panes (the data
object and the shell's drop handler are covered by tests; the mouse gesture itself is not).
