# ui-terminal — notes

Owner of `ui/src/terminal/**` and `ui/src/bridge/mock/conn.ts`.

## Status (resume checklist)

- [ ] keymap.ts (PuTTY keyboard modes) + keymap.test.ts
- [ ] connEvents.ts (single conn:onData/onStatus/onEventLog router)
- [ ] termSession.ts (xterm instance per pane, addons, connection lifecycle, input/broadcast, bell, zoom)
- [ ] TerminalPane.tsx (mount, overlays: connecting card, disconnected banner, prompt overlay, find bar)
- [ ] context menu
- [ ] EventLogDialog.tsx
- [ ] mock conn.ts (prompt/kbd/fail/exit/drop/demo output)
- [ ] preview harness + screenshots
- [ ] typecheck + tests

## Requests

(none yet)
