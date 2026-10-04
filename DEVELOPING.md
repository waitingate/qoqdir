# Developing Qoqdir

Notes for working on the code.

## What this is

Qoqdir is a small Win32 file explorer (tabs, folder tree, virtual detail list) in C++20, built as one
static exe and started from Windows Terminal. `QOQDIR-SPEC.md` is the contract: section 4 (behavior
rules) and section 7 (acceptance tests) decide whether a change is right, section 6 lists what must
not be built, and section 2 holds the working rules. Read the relevant sections before changing
behavior. `README.md` has the user-facing command line, keyboard table and file layout.

## Build and test (Windows only)

The code is Win32-only: there is no mock layer, and even the unit tests call the real Windows API
(they list `C:\Windows` and the drives). Building and testing happen on a Windows machine with
MinGW-w64 GCC 13 at `C:\mingw64` plus CMake and Ninja on the PATH. No Visual Studio, nothing
installed system-wide. (The same sources also cross-build on Linux with the MinGW-w64 cross-compiler,
and Wine can run both tests; that is a smoke check only, never the spec's results.)

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH
cmake -S . -B build -G Ninja     # Release by default: static link, stripped, _GLIBCXX_ASSERTIONS on
cmake --build build              # -> build\qoqdir.exe and build\unit_tests.exe

ctest --test-dir build --output-on-failure    # runs both tests below
build\unit_tests.exe                          # tests/tests.cpp: paths, command line, sort, filter, listing
build\qoqdir.exe --selftest                   # drives the real app; log: %TEMP%\qoqdir-selftest.log; exit 1 on any FAIL
powershell -File tests\cli-checks.ps1 -Exe build\qoqdir.exe      # -t, duplicate folders, -h/-x exit codes (on a private copy of the exe)
powershell -File tests\watch-windows.ps1 -Exe build\qoqdir.exe   # runs the self-test and lists every new window from any process
```

There is no single-test selector. `unit_tests` is one `main()` of `CHECK`/`CHECK_EQ` macros that
print `FAIL line N` and keep going. The self-test runs about 156 steps in order (one `add(...)` each
in `App::startSelfTest`, covering the 40 scenarios of spec 7.2); every step has its own timeout, a
failure does not stop the run, and the log gets one `ok`/`FAIL` line per step with timings, written
the moment the step ends (so a hang still leaves the steps so far). To work
on one scenario, run the whole self-test and read its log.

Tests never touch the user's desktop session: no global keystrokes or mouse input, no focus
stealing, no dialogs, no beeps. The self-test feeds keys straight into `App::onKey`, uses a
Bookmarks folder inside its own `%TEMP%\qoqdir-selftest` folder (never the real `qoqdir.ini` or
Bookmarks), and uses `\\192.0.2.1\share` as the unreachable network path. When fixing a bug, first
add a unit test or self-test step that fails, then fix it. Report real results, including failures.

## Architecture

Layers, bottom up (each header's top comment says more):

- `src/common.h`: RAII owners for every Windows resource (`Com<T>`, `Pidl`, `Menu`, `Font`) and
  `kAppName`. Nothing is freed by hand.
- `src/folder.cpp` (static library `core`): paths, command line, `listFolder`, sorting, filtering.
  No UI and no COM, so all of it is unit-tested. The empty path `""` means This PC. Only text the user
  typed has its %VARS% expanded (`expandVars`): a folder may really be named `%TEMP%`.
- `src/shell.cpp`: all contact with the Windows shell. Copy, cut, paste, delete and rename go
  through `IFileOperation` and shell verbs; context menus, icons and type names come from the shell,
  so everything behaves exactly like Explorer (progress, conflicts, undo, Recycle Bin). Never
  re-implement any of that. `uiObject` resolves items through their parent folder once because
  resolving each full path costs ~9 ms per file.
- `src/filelist.cpp` and `src/tree.cpp`: the two controls.
- `src/app.cpp` with `app.h`: the main window, tabs, navigation, keyboard and settings.
  `src/selftest.cpp` is part of `App` (the nested `App::SelfTest`) so it can drive private state.
- `src/main.cpp`: command line, help box, message loop. It turns off Windows' "No Disk" box
  (`SetErrorMode`) and, once the command line is read, moves the working folder to System32, so the
  folder Qoqdir was started from can still be renamed, deleted or ejected.
- `src/crashlog.cpp`: spec 4.10. Three hooks (unhandled-exception filter, `std::__glibcxx_assert_fail`,
  SIGABRT) write the reason to `%TEMP%\qoqdir-crash.log`; its header comment explains them.

Rules the design depends on:

- **One UI thread owns all state.** Worker threads are short-lived detached `std::thread`s that own
  nothing shared. They read a folder (`App::open`), a tree node's children or the tree's user-folder
  roots (`FolderTree::startWorker`) or files' own icons, then `PostMessageW` a heap object (`Listing`,
  `TreeBatch`, `IconBatch`) to the main window, which takes it into a `unique_ptr`. The custom
  messages are the `WM_APP_*` enum at the top of app.cpp. `FolderTree::fill()` itself asks Windows
  about nothing: Desktop, Documents and the rest arrive from the worker (one of them can sit on a
  dead server).
- **Newest request wins.** `App::open` bumps `generation_` and records `pending_`; `onLoaded` drops
  any result whose generation is stale. The read itself stops too: `listFolder` asks every 256 entries
  whether `g_newestLoad` has moved on, so quick tab switches between huge folders never pile up full
  reads (a read stuck in one Windows call, on a dead server, holds no list while it waits). `FolderTree` numbers every read and forgets a node's read in
  `TVN_DELETEITEM`, so a batch for a node that was collapsed or rebuilt meanwhile is dropped (a tree
  handle can be reused). A tab's path, history and title change only inside `onLoaded`, never before
  the folder has loaded; a Refresh that lands while the rename box is open is dropped and queued
  again (the box remembers a row, not a file). `refresh()` starts nothing while a load is pending,
  which is why a held-down F5 starts one load, not many; what it was asked to select (a new folder,
  the item after a delete) is kept in `queuedView_` for the queued refresh, as it is when a Refresh is
  held back by a rename box, and is scrolled into view when it lands. A plain refresh and a tab switch
  keep the scroll position instead; if the focused item has gone, the item now in its row takes the
  focus (`ViewState::focusRow`). A delete the user cancelled in Windows' dialog starts no refresh at all,
  so the selection stays; after a real one nothing is selected until the reload lands. The
  right-click menu is modal, so Qoqdir's own commands in it (paste, rename, delete, open here) act only
  if the folder and selection are unchanged when the user picks one.
- **Tabs remember their view.** `rememberView()` stores a tab's selection, focus and scroll when it
  leaves the screen, but only if the list shows that tab's own folder and no switch to it is still
  loading; otherwise the tab keeps what it had (a quick Ctrl+Tab past a slow folder loses nothing).
- **Failed loads.** `onLoadFailed` decides what happens. A failed Refresh or Switch puts the tab in
  the "not available" state: empty list, `tab().unavailable = true`, status message, no folder watch.
  The flag also starts true: a background tab (from `-t` or a middle-click) whose folder has never
  loaded here is treated the same until it has been shown once. Paste, new folder, the empty-space
  menu and tab drops check that flag so they never ask Windows about an unreachable folder (a 20-40 s
  freeze). While a tab switch loads, the list is already empty for the same reason. A typed path that
  fails gives the focus back to the address bar only while the focus is still on the list, where Enter
  left it: a rename box or the filter box the user opened meanwhile is left alone. A failed Back or Forward drops the dead history
  entry; a typed path that turns out to be a file is opened instead. The filter box is cleared when
  a navigation starts (`App::open`), so letters typed while it loads stay; a navigation that fails or
  opens a file puts the old filter back (`Pending::filter`).
- **Live updates.** `FolderWatch` (SHChangeNotifyRegister) posts `WM_APP_CHANGED`; `queueRefresh()`
  coalesces a burst into one 400 ms `kRefreshTimer`, which waits while a rename or a load is in
  progress. A drive arriving or going (`WM_DEVICECHANGE`) re-reads only This PC in the tree
  (`FolderTree::refreshDrives`, so what the user expanded stays) and queues the same refresh. The
  timer skips a tab that is "not available" (F5 is its retry, as for a deleted folder).
- **Tree nodes.** Each node's path is in `FolderTree::paths_`, keyed by the node's handle, and leaves
  it in `TVN_DELETEITEM`. Bookmark shortcuts are read from the .lnk file alone (`shortcutTarget`: a
  path, `""` for This PC, or nothing for a web link or library), so they show at once with a folder or
  file-type icon; their own icon, which asks the target (maybe a server that is off), follows from the
  same worker via `WM_APP_ICONS` and `FolderTree::setIcons`, as for files in the list.
- **Virtual list.** `FileList` is `LVS_OWNERDATA`: `items_` holds the folder, `view_` the filtered
  indices, and row i is `items_[view_[i]]`. `FileList::update` is the only place that changes
  `view_`, and it rebuilds it (and clears the selection) before any ListView call. Hidden items are
  always read; Ctrl+H (`FileList::setShowHidden`) only shows or hides them, as the filter does. Type names and
  icons per extension come from a cache in shell.cpp that never touches the disk; the own icons of
  .exe, .lnk and the like arrive later from the loader thread via `WM_APP_ICONS`.
- **Keyboard.** The message loop in main.cpp hands every message to `App::handleKey` before
  dispatch, only for Qoqdir's own windows (never a Properties dialog's). `App::onKey` is the single
  shortcut table. Typing a character in the list forwards it to the filter box.
- **Settings.** `qoqdir.ini` next to the exe via `GetPrivateProfileString`: window geometry, tree and
  column widths, sort order and the hidden-files flag. Never tabs or paths. A read-only location is
  silently fine. Saved on `WM_CLOSE` and on `WM_ENDSESSION` (shutdown sends no close); a saved position
  on a monitor that is gone falls back to the default placement. The `Bookmarks` folder is also next to the exe. Nothing goes in the registry or
  AppData.
- `ShellProcessRef` (SHSetInstanceExplorer) keeps the process alive after the window closes until
  background copies and Properties windows have finished. Then `OleFlushClipboard` keeps files
  copied in Qoqdir pasteable after it has closed.

## Conventions

- Keep it small: one mechanism rather than special cases. Do not add features outside the spec
  (section 6 lists what is out of scope) without asking. When in doubt, leave it out.
- Qoqdir shows no message boxes of its own during normal work. Errors go to the status bar
  (`setMessage`) with a beep (`warn()`), and never beep during the self-test. The only dialogs are
  Windows' own (file operations, Properties, Open With) and the command-line help.
- No C++ exceptions; failures are return values and `Listing::error`. `_GLIBCXX_ASSERTIONS` is on
  so a bad index stops the program loudly instead of corrupting memory. Do not turn it off.
- Anything that can block (reading a folder, `SHGetFileInfoW` on a real path, any network path)
  runs on a worker thread. The UI-thread-only functions are marked as such in shell.h.
- "Qoqdir" in window titles and messages (`kAppName`); lowercase `qoqdir` in every file name
  (`qoqdir.exe`, `qoqdir.ini`, `qoqdir-*.log`).
- The C++ sources and `CMakeLists.txt` use CRLF line endings; keep them, or git shows whole-file
  diffs. The `.md` files and the `.ps1` scripts use LF.
- Style: 4-space indent, lines up to about 120 columns, members end in an underscore (`list_`),
  and comments explain why in plain English, as the existing header comments do.
- `rust/`, where present, is the earlier Rust version kept for reference only. Spec section 3.1
  concludes that one version is built and maintained: this C++ one.
