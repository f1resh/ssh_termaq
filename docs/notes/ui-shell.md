# ui-shell — notes

Owner of `ui/src/App.tsx`, `ui/src/components/{titlebar,sidebar,layout,palette,welcome,statusbar,quickconnect}/**`,
`ui/src/commands/**`, `ui/src/theme/**`, `ui/src/store/**`, `ui/src/bridge/mock/{index,core}.ts`, `ui/src/main.tsx`,
`ui/index.html`, `ui/public/**`.

## Status (resume checklist)

- [x] Themes: 14 app themes (`theme/appThemes.ts`), 29 terminal schemes (`theme/schemes.ts`), resolve + ThemeController
- [x] Keybinding helpers (`commands/chords.ts`, pure) — in progress: keybindings.ts re-export + global listener
- [ ] commands.ts: COMMANDS metadata + runCommand for every CommandId
- [ ] Title bar: tabs (DnD, rename, colors, context menu, status/activity/bell), + dropdown, caption buttons
- [ ] Sidebar: search/highlight, tree, DnD move, context menus, footer, empty state, resize
- [ ] Pane layout: split tree with splitters, all tabs mounted, SFTP side panel
- [ ] Status bar
- [ ] Welcome screen
- [ ] Quick connect parser + dialog + tests
- [ ] Command palette
- [ ] App wiring: open requests, close requests, workspace restore, window title, focus/maximize
- [ ] Mock core additions
- [ ] Tests (chords, quick connect parser, fuzzy, tree)
- [ ] Screenshots reviewed

## Requests

(none yet)
