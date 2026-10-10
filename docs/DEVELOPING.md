# Developing Qoqdir

Notes for working on the code. `SPEC.md` decides what is right: section 4
(behavior) and section 7 (tests). Section 6 lists what not to build.
`BUILDING.md` says how to build and test.

## Working rules

- Both versions stay level, as they have since 5 October 2026: a change goes into
  `src/` and `rust/` alike, with the same self-test steps under the same names and
  in the same order. `scripts\build.ps1 -Test` checks that the two self-test logs
  match. The C++ version is the main one.
- A change is done when `scripts\build.ps1 -Test` passes: no compiler warnings,
  nothing from clippy, every test and check passing.
- For a bug, first add a test that fails, then fix it.
- Tests never touch the desktop: no global keys or mouse, no taking the focus, no
  dialogs, no beeps. The self-test calls `App::onKey` directly, works in its own
  folder in `%TEMP%` (never the real `qoqdir.ini` or Bookmarks), and uses
  `\\192.0.2.1\share` as a network path that never answers.

## Layout

### C++ (`src/`)

- `util.h`: owners for Windows resources (`Com<T>`, `Pidl`, `Menu`, `Font`)
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
  of `App` so it can reach private state; only `qoqdir-test.exe` links it in,
  with the same compiled app code as `qoqdir.exe` (CMake's `app` object library),
  so the self-test checks the very code that ships.
- `main.cpp`: command line, help box, message loop. It turns off Windows' "No Disk"
  box and, after reading the command line, moves the working folder to System32,
  so the start folder can still be renamed or ejected.
- `crashlog.cpp`: writes `%TEMP%\qoqdir-crash.log` on a crash, a failed library
  check or `abort()`.
- `res/qoqdir.rc`, `res/qoqdir.manifest`, `res/qoqdir.ico` (next to `src/`): the
  manifest (modern controls, high DPI, long paths), the version block and the
  icon, which `scripts/make-icon.ps1` draws (`BUILDING.md`, section 4). The Rust
  build takes the icon from there too.
- `version.h`: the version, shown in the exe's Properties, the help box and each
  crash-log line. The Rust version keeps its own in `rust/Cargo.toml`.

### Rust (`rust/`)

The same parts, file for file, written with Microsoft's `windows` crate:

- `src/folder.rs`: paths, folder reading, sorting, filtering, with the unit tests
- `src/shell.rs`: the Windows shell: menus, clipboard, delete, rename, icons,
  drag and drop
- `src/filelist.rs`, `src/tree.rs`: the file list and the folder tree
- `src/app.rs`: main window, tabs, navigation, keys, settings
- `src/selftest.rs`: the self-test (only in a build with the `selftest` feature)
- `src/main.rs`, `src/crashlog.rs`: as in C++
- `src/util.rs`: UTF-16 strings, and owners that free Windows resources
- `build.rs`, `icon.rc`: embed the manifest, and `../res/qoqdir.ico` through
  `windres`

Shared state is never borrowed across a call into Windows, because Windows can
call straight back into the app. A mistake there stops the app (`panic = "abort"`)
instead of corrupting data, and the crash log says where.

## How it works

- One UI thread owns all state. Folder reads, tree children and file icons run on
  short-lived worker threads that share nothing and post their result (`Listing`,
  `TreeBatch`, `IconBatch`) to the main window as `WM_APP_*` messages.
  `FolderTree::fill()` asks Windows nothing that can block: even Desktop and
  Documents come from a worker, since one of them can live on a dead server.
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
  `TVN_DELETEITEM`. A bookmark shortcut is read from the .lnk file; a target on
  a drive letter that is gone is looked for by the relative path Windows stores
  in it (`Resolve` without search or UI); a drive that is there is never asked.
  Its own icon comes later from the worker, like file icons in the list.
- Every message goes through `App::handleKey` before dispatch, but only for
  Qoqdir's own windows. `App::onKey` is the one shortcut table. A key counts for
  the window it was typed in, not for where the focus is once it is read: an input
  method's word arrives as several characters at once.
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
  load it at run time from System32, as `setTheme` does with uxtheme.dll.
- "Qoqdir" in titles and messages, lowercase `qoqdir` in file names.
- Git stores every file with LF line endings (Git for Windows shows them with CRLF
  in the working folder).
- 4-space indent, lines up to about 120 columns, members end in `_`. Comments
  say why, not what.
- Each file pair has the same section headers (`// ---- name`) in the same
  order, so a function sits at the same place in both versions.
