# host module notes

Owner of `native/src/app/**`, `native/src/store/**`, `native/res/**`, `native/tests/{store,app}/**`,
additions to `native/src/core/Types.*` and `CMakeLists.txt`.

## Status (resume checklist)

- [x] store/JsonFile: atomic writes (temp + MoveFileExW REPLACE_EXISTING|WRITE_THROUGH, retries), corrupt backup
- [x] store/Credentials: DPAPI (UI_FORBIDDEN + entropy), callbacks outside locks
- [x] store/KnownHosts: per header semantics
- [x] store/Store: sessions JSON docs merged over defaults, folders (rename/delete re-parenting), proxy.password
      `enc:`, settings deep merge, defaultSessionTemplate, logDirectory, state
- [ ] tests/store (sc_test_store)
- [ ] app/Paths, applog init
- [ ] app/Cli (+ tests/app sc_test_app)
- [ ] app/SingleInstance (mutex + WM_COPYDATA)
- [ ] app/MainWindow (custom frame, DPI, placement, WebView2, dispatcher, theme/backdrop, fullscreen, close flow)
- [ ] app/Dialogs
- [ ] app/CoreHandlers (sessions/credentials/settings/knownHosts/serial/dialog/app/window)
- [ ] app/App.cpp runApp + main.cpp + crash handlers
- [ ] res/: app.manifest, app.rc, make_icon.py, app.ico, icon-256.png
- [ ] Build SSHCrawler + CDP verification screenshot (artifacts/host-*.png)

## Design decisions

(filled in as work progresses)

## Limitations

## Requests
