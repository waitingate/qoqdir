# Developing Qoqdir

Notes for working on the code. `QOQDIR-SPEC.md` decides what is right: section 4
(behavior) and section 7 (tests). Section 6 lists what not to build.

## Build and test

Windows only: MinGW-w64 GCC 13 in `C:\mingw64`, with CMake and Ninja on the PATH.

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

- `build\unit_tests.exe`: paths, command line, sorting, filtering, listing. It is
  one `main()` of `CHECK` macros that prints `FAIL line N` and keeps going.
- `build\qoqdir.exe --selftest`: about 150 steps that drive the real app. Each
  step has a timeout and a failure doesn't stop the run. The log
  (`%TEMP%\qoqdir-selftest.log`) gets an `ok` or `FAIL` line as each step ends.
  There is no way to run a single step: run them all and read the log.
- `tests\cli-checks.ps1` and `tests\watch-windows.ps1`, see the README.

The sources also cross-build on Linux with MinGW-w64, and Wine can run the tests,
but only as a quick check.

Tests never touch the desktop: no global keys or mouse, no taking the focus, no
dialogs, no beeps. The self-test calls `App::onKey` directly, works in its own
folder in `%TEMP%` (never the real `qoqdir.ini` or Bookmarks), and uses
`\\192.0.2.1\share` as a network path that never answers. For a bug, first add a
test that fails, then fix it.

## Layout

- `common.h`: owners for Windows resources (`Com<T>`, `Pidl`, `Menu`, `Font`)
  and `kAppName`. Nothing is freed by hand.
- `folder.cpp` (library `core`): paths, command line, `listFolder`, sorting,
  filtering. No UI and no COM, so it is all unit-tested. `""` means This PC. Only
  typed text gets its `%VARS%` expanded, since a folder may really be named
  `%TEMP%`.
- `shell.cpp`: everything that talks to the Windows shell. Copy, paste, delete,
  rename, context menus, icons and type names come from Windows so they behave
  like Explorer. Don't write your own. `uiObject` looks items up through their
  parent folder once, because looking up each full path costs about 9 ms a file.
- `filelist.cpp`, `tree.cpp`: the two controls.
- `app.cpp`: main window, tabs, navigation, keys, settings. `selftest.cpp` is part
  of `App` so it can reach private state.
- `main.cpp`: command line, help box, message loop. It turns off Windows' "No Disk"
  box and, after reading the command line, moves the working folder to System32,
  so the start folder can still be renamed or ejected.
- `crashlog.cpp`: writes `%TEMP%\qoqdir-crash.log` on a crash, a failed library
  check or `abort()`.
- `version.h`: the version, shown in the exe's Properties, the help box and each
  crash-log line. The Rust version keeps its own in `rust/Cargo.toml`.

## How it works

- One UI thread owns all state. Folder reads, tree children and file icons run on
  short-lived worker threads that share nothing and post their result (`Listing`,
  `TreeBatch`, `IconBatch`) to the main window as `WM_APP_*` messages.
  `FolderTree::fill()` asks Windows nothing itself: even Desktop and Documents
  come from a worker, since one of them can live on a dead server.
- The newest request wins. `open()` bumps `generation_` and older results are
  dropped; a replaced folder read stops within 256 entries. A tab's path, history
  and title change only in `onLoaded`. `refresh()` does nothing while a load is
  pending (so holding F5 starts one load); what it was asked to select is kept in
  `queuedView_` for the refresh that follows.
- A tab remembers its selection, focus and scroll (`rememberView()`) when it
  leaves the screen, but only if the list shows that tab's own folder.
- A failed Refresh or tab switch leaves the tab "not available": empty list,
  `unavailable` set, a status message, no folder watch. Background tabs start
  with that flag until they have loaded once. Paste, new folder, the empty-space
  menu and drops on the tab check it, so they never ask Windows about a server
  that is off (that can freeze for 20 to 40 s). A failed Back or Forward drops the
  dead history entry; a typed path that is a file gets opened.
- The right-click menu is modal, so Qoqdir's own commands in it only act if the
  folder and the selection are the same as when it opened.
- Live updates: `FolderWatch` posts `WM_APP_CHANGED`, and a burst of changes
  becomes one refresh after 400 ms, which waits while a rename or a load runs. A
  drive coming or going re-reads only This PC in the tree.
- The file list is virtual (`LVS_OWNERDATA`): row i is `items_[view_[i]]`, and only
  `FileList::update` changes `view_`, rebuilding it before any ListView call.
  Hidden items are always read; Ctrl+H only shows or hides them.
- Tree nodes keep their path in `FolderTree::paths_`, removed in
  `TVN_DELETEITEM`. A bookmark shortcut is read from the .lnk file alone; its own
  icon comes later from the worker, like file icons in the list.
- Every message goes through `App::handleKey` before dispatch, but only for
  Qoqdir's own windows. `App::onKey` is the one shortcut table.
- `qoqdir.ini` is saved on close and at shutdown. A saved position on a monitor
  that is gone falls back to the default.
- `ShellProcessRef` keeps the process alive after the window closes until
  background copies and Properties windows are done. `OleFlushClipboard` keeps
  copied files pasteable after Qoqdir exits.

## Conventions

- Keep it small: one mechanism rather than special cases, and nothing outside the
  spec without asking first.
- No message boxes of our own in normal use. Errors go to the status bar
  (`setMessage`) with a beep (`warn()`), never during the self-test.
- Anything that can block (reading a folder, `SHGetFileInfoW` on a real path, any
  network path) runs on a worker thread.
- No exceptions. `_GLIBCXX_ASSERTIONS` and the signed overflow trap stay on.
- Link only DLLs that Windows protects (its KnownDLLs list, or comctl32 through
  the manifest). Windows looks for any other imported DLL next to the exe first:
  load it at run time from System32, as `useExplorerTheme` does with uxtheme.dll.
- "Qoqdir" in titles and messages, lowercase `qoqdir` in file names.
- Git stores every file with LF line endings (Git for Windows shows the C++ files
  with CRLF in the working folder).
- 4-space indent, lines up to about 120 columns, members end in `_`. Comments
  say why, not what.
- `rust/` is the same program in Rust, level with the C++ version since 5 October
  2026 (same features, same self-test steps). The C++ version is the main one.
