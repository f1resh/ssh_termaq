# ui-dialogs — notes

Owner of `ui/src/components/dialogs/**`, `ui/src/bridge/mock/{importer,keys}.ts`, additions to
`ui/src/shared/types.ts` + `ui/src/shared/defaults.ts`.

## Status (resume checklist)

- [x] common/ building blocks (controls, hooks, NestedConfirm, SchemePreview, FontPicker, fonts) — from Step 1
- [x] session/sessionModel.ts (validation, tunnels, protocol switching, log path) + algoModel.ts — from Step 1
- [ ] dialogs.css (all dialog styling, CSS variables only)
- [ ] DialogHost (all dialog types + global hostkey/confirm prompts + `#preview=dialogs` harness)
- [x] HostKeyDialog, PromptConfirmDialog, ConfirmDialog, InputDialog, AboutDialog — from Step 1
- [ ] SessionEditor shell (nav, draft state, validation, save / save & connect, default settings, unsaved confirm)
  - [x] Session / Connection / Proxy sections
  - [ ] SSH / Auth / Tunnels / X11 / Algorithms sections
  - [ ] Terminal / Keyboard / Appearance / Translation / Logging sections
  - [ ] Telnet / Rlogin / Serial sections
- [ ] Settings dialog (Appearance, Terminal, Behavior, Keybindings, Color Schemes, Known Hosts, Default Session, Data & Backup, About)
- [ ] Import wizard
- [ ] Keygen dialog
- [ ] Mocks: importer.ts, keys.ts
- [ ] vitest tests for pure helpers
- [ ] Screenshots (artifacts/ui-dialogs-*.png) dark + light

## Requests

(see bottom)
