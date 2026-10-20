# Host probe (macOS)

A throwaway AppKit + WKWebView app from step B3 of the macOS port. It measures what the macOS host window had to
decide before it was written: how host → page messages travel, how the page copes with a flood of terminal output,
whether output keeps flowing while a sheet or a modal panel is open, and what the page sees (secure context, WebGL,
page zoom, window chrome). It is not part of the app and not built by CMake. The decisions it led to are recorded in
[docs/ARCHITECTURE.md](../../../docs/ARCHITECTURE.md#bridge).

## Running it

```bash
native/tools/hostprobe/build.sh
```
```bash
build/hostprobe/HostProbe.app/Contents/MacOS/HostProbe --page 'probe.html?plan=full' --out probe.json
```

It opens a window, runs the measurements of `probe.html` (about a minute; a file sheet and an alert appear for two
seconds each), prints a JSON report and quits. `?plan=quick` runs only the checks and one short flood;
`?runs=[{"mode":"pull","mb":20}]` runs the floods given. xterm.js is loaded from `ui/node_modules` (`npm ci` in `ui`
first). To look at the real UI (its mock backend) in the same web view, with a snapshot:

```bash
build/hostprobe/HostProbe.app/Contents/MacOS/HostProbe --root "$PWD/ui/dist" --page index.html --snapshot ui.png
```

## How it measures

- **Host → page.** A background thread queues `conn:onData` messages the way the session manager emits them
  (64 KiB of coloured `ls -l` output with some UTF-8, as bridge JSON). The main thread delivers them in one of four
  ways: `evaluateJavaScript` of `__stDeliver([...])`, `callAsyncJavaScript` with the batch as a string argument, a
  pull loop in the page answered through `WKScriptMessageHandlerWithReply`, or a streamed `fetch` of
  `sshtermaq://app/__events` fed by the scheme handler. Batches hold up to 1 MB.
- **During a flood** the page records the longest gap between 10 ms timer ticks, the animation frame rate, and the
  round trip of a ping every 100 ms whose answer is queued behind the output (as a real reply would be).
- **Rendering**: the same floods written into xterm.js 5.5 (DOM and WebGL renderers).
- **Panels**: 10 MB/s of output while an `NSOpenPanel` sheet or an `NSAlert` modal is shown for two seconds, with
  the main thread woken through a run-loop source in the common modes or through the GCD main queue.

## Results

macOS 26.6 on an Apple M1 (8 GB), arm64 build, 18 October 2026. Not yet measured: macOS 12 (needs a VM) and an
Intel Mac.

| Delivery | 100 MB took | Throughput | Longest timer gap | Frames/s | Reply wait p50 / p95 |
|---|---|---|---|---|---|
| `evaluateJavaScript`, 4 in flight | 0.43 s | 234 MB/s | 16 ms | 54 | 228 / 328 ms |
| `callAsyncJavaScript`, 4 in flight | 0.44 s | 225 MB/s | 15 ms | 54 | 243 / 343 ms |
| `callAsyncJavaScript`, 1 in flight | 0.58 s | 172 MB/s | 14 ms | 60 | 279 / 480 ms |
| pull (`WKScriptMessageHandlerWithReply`) | 0.60 s | 167 MB/s | 16 ms | 60 | 299 / 499 ms |
| streamed `fetch` | 8.3 s | 12 MB/s | 41 ms | 60 | 4166 / 7905 ms |

Replies wait behind the whole flood here; at a realistic 10 MB/s they took 1 ms (p50).

| Into xterm.js (32 MB) | Throughput | Longest timer gap |
|---|---|---|
| `callAsyncJavaScript`, DOM renderer | 55 MB/s | 55 ms |
| `callAsyncJavaScript`, WebGL renderer | 51 MB/s | 446 ms (once, when the WebGL context starts) |
| pull, WebGL renderer | 62 MB/s | 31 ms |

| 10 MB/s of output while… | Main thread woken by | Arrived while it was open |
|---|---|---|
| an `NSAlert` runs modally (2 s) | run-loop source, common modes | 20.3 MB (all of it) |
| an `NSAlert` runs modally (2 s) | GCD main queue | **nothing** until it closed |
| an `NSOpenPanel` sheet is open (3.3 s) | run-loop source, common modes | 32.8 MB (all), after a 1.2 s pause while the sheet opened |
| an `NSOpenPanel` sheet, pull delivery (2.6 s) | run-loop source, common modes | 25.6 MB (all), after a 0.5 s pause |

What the page sees, at `sshtermaq://app/`:

- `isSecureContext` is true (so `crypto.randomUUID` and `crypto.subtle` work); WebGL2 on the Apple GPU; CSS
  `color-mix()`, nesting and `:has()` are supported; `queryLocalFonts` does not exist (fonts must come from the
  host); `localStorage` works but, with the non-persistent data store, only until the app quits (only the UI's mock
  backend uses it).
- `pageZoom` behaves like WebView2's zoom: at 1.5 the device pixel ratio goes from 2 to 3 and the CSS viewport from
  1100 to 733 px. `magnification` is pinch zoom (visual viewport scale) and stays off.
- Traffic lights in a full-size-content window with a transparent title bar: x 9–69 and y 9–23 CSS px; the title bar
  is 32 px.
- The built UI loads unchanged, without errors; what it needs on macOS (no caption buttons, room for the traffic
  lights, ⌘ shortcuts) is step B4's.

## Decisions

- Host → page: `callAsyncJavaScript`, batched (up to ~1 MB per call, 4 calls in flight). As fast as
  `evaluateJavaScript` without turning messages into script code, and its completions give backpressure.
- The main thread is woken through a run-loop source in the common modes, never the GCD main queue.
- File dialogs are sheets.
