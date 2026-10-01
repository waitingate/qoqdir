# Qoqdir — build specification

This document is the full specification of **Qoqdir**, enough to build it again from an empty folder. It says
*what* the program must do and *how it must behave*. It deliberately contains no code
and no prescribed implementation: choose the simplest design that meets every rule
below, and explain your main choices.

Read the whole file before writing anything. Sections 4 (behavior rules) and 7
(acceptance tests) are the contract; everything you build is judged against them.

---

## 1. What Qoqdir is

A light, fast, reliable file explorer for Windows 10/11, meant to replace Windows
Explorer for everyday work, and to be started from Windows Terminal as a routine tool.
It is inspired by CubicExplorer (tabs, a folder tree, a fast detail list) but is a
new, much smaller program.

The user's priorities, in order:

1. **Reliable.** It never freezes, never loses a tab, never shows surprise popups,
   never corrupts or loses files.
2. **Simple.** Small code, simple logic, few moving parts. Bugs are avoided by design,
   not patched one by one. When in doubt, leave a feature out.
3. **Fast.** Faster than Explorer to start, to open folders and to filter.
4. **Ergonomic.** Keyboard-first, Explorer-compatible shortcuts, no learning curve.
5. **Portable and easy to maintain.** One exe, nothing installed, readable code.

## 2. Working rules

- **Keep it small.** Prefer one clear mechanism over several special cases. If a rule
  here would need a lot of code, say so and propose a simpler rule instead of
  building it silently.
- **Do not add features** that are not in this file (no themes, plugins, previews,
  search indexes, saved sessions...). Ask first.
- **Let Windows do the file work.** Copy, move, delete, rename, paste, context menus,
  properties, "open with", icons and type names must come from the Windows shell, so
  they behave exactly like Explorer (progress windows, conflict dialogs, undo, Recycle
  Bin). Never write your own copy engine or your own context menu entries for these.
- **Never touch the user's desktop session in tests:** no global keystrokes, no global
  mouse input, no stealing focus, no dialogs, no sounds. Tests drive the app from
  inside its own process.
- **Test before claiming.** Build, run the unit tests and the self-test, and report the
  real results, including failures. When fixing a bug, first add a test that fails,
  then fix it.
- **Never close windows you did not open**, and never delete files outside the test
  folders you created.

## 3. Hard constraints

- **Platform:** Windows 10 and 11, x64. Native Win32 UI (standard Windows controls,
  so it looks and feels like part of Windows). No web view, no UI framework, no .NET,
  no runtime to install.
- **Language:** C++ or Rust; pick one and say why. The build machine has, without any
  system installation:
  - MinGW-w64 GCC 13 at `C:\mingw64` (with CMake and Ninja available), and
  - a portable Rust toolchain (GNU target) under `C:\rust`, activated per terminal by
    `C:\rust\rust-env.ps1`.
  The build must not install anything system-wide, and must not need Visual Studio.
- **Output:** a single `qoqdir.exe` with no DLLs beside it (statically linked), with an
  embedded manifest: per-monitor DPI awareness (v2), long-path awareness, modern
  common controls. Small: well under 1 MB is the goal.
- **Portable:** everything the program writes lives next to the exe:
  - `qoqdir.ini` — settings (created when first saved; a read-only location just
    means nothing is saved, never an error).
  - `Bookmarks\` — the bookmarks folder (created at start if missing).
  - Nothing in the registry, nothing in AppData. (The only exception: an optional
    crash log in `%TEMP%`, see 4.10.)
- **Names:** the app is "Qoqdir" in window titles and messages; every file name is
  lowercase `qoqdir` (`qoqdir.exe`, `qoqdir.ini`, logs `qoqdir-*.log`).
- **Unicode everywhere:** any file name in any language must show, sort, rename and
  open correctly. Paths longer than 260 characters must never crash or hang the app;
  they work as far as Windows allows (Windows' long-path setting is off by default, and
  Explorer cannot open such folders either).

### 3.1 Experience from two earlier builds (to help choose the language)
Qoqdir was built before, twice in parallel (C++ and Rust, same features, same tests).
Facts from that, not instructions:
- **Result:** both met every rule here, with the same speed (differences were within
  measurement noise). Exe size: C++ ~730 KB, Rust ~570 KB.
- **Code size:** the Rust version needed ~45% more lines for the same app (~3,700 vs
  ~2,500, without tests), mostly from Win32 calls and pointer handling through the
  `windows` crate.
- **Build time:** C++ clean build under a minute; Rust's first build ~7 minutes (the
  `windows` crate), then about a minute.
- **Safety:** Rust turned one real bug (a list row pointing at an item that had just
  been replaced) into a clean stop with a clear message; the same bug in C++ was
  silent memory misuse that went unnoticed. In Rust the main trap is the opposite
  kind: Windows often calls straight back into the app while it is inside a call to
  Windows, so shared state must never be held borrowed across such a call.
- **Toolchain pitfalls on this machine:** the Rust GNU toolchain's bundled `dlltool`
  fails here; Rust must be pointed at MinGW's (`C:\mingw64\bin\dlltool.exe`) in the
  portable cargo config. Implementing COM interfaces in Rust (drop targets, etc.) needs
  the `windows-core` crate alongside `windows`. The C++ side needed only MinGW,
  static linking and the manifest in a resource file.
- **Two versions at once doubled every change and every test run.** The second version
  found exactly one hidden bug (the one above); all others were found by the tests and
  by code reviews, which work on one version too. So build one, and make it stop
  loudly on such mistakes: Rust does by itself; in C++, build and test with the
  compiler's runtime checks on (e.g. GCC's `_GLIBCXX_ASSERTIONS`, which turns an
  out-of-range index into a clear stop).

## 4. Behavior rules (the reliability contract)

These describe what the user experiences. Each must hold in every situation.

### 4.1 Never freeze
- The window stays responsive while any folder is read, however slow: a sleeping USB
  disk, a slow NAS, a network share that does not answer at all (Windows can take
  20–60 s to give up). Pressing a key or clicking must react at once (well under
  100 ms), including Enter on such a path.
- This applies everywhere a path is touched: opening a folder, the address bar, the
  tree expanding, a shortcut whose target is on the network, icons of files on a slow
  disk, and refreshes.
- If loading is slow (over about a quarter second), the status bar says "Loading…";
  it disappears when the folder arrives or the load is replaced.

### 4.2 The newest request wins
- Starting a new navigation (another folder, Back, another tab...) replaces any load
  still in progress; the old result, when it finally arrives, is ignored.
- A tab's displayed folder, history and title change only when its new folder has
  actually loaded. A failed or slow load never leaves a tab half-updated.
- Repeated refreshes never pile up: refreshing while the folder is still loading starts
  nothing new (holding F5 on a slow network folder starts one load, not one per key
  repeat).
- An automatic refresh (folder changed on disk) never cancels a navigation in progress
  and never interrupts a rename in progress; it waits until they are done.

### 4.3 Tabs keep their folder
- A tab never changes its folder by itself. If its folder is unavailable (deleted,
  drive removed, network not connected yet), the tab stays on that folder, shows an
  empty list, and the status bar says:
  `"<folder>" is not available: <Windows error text>   (F5 tries again)`.
  When the folder is back, F5 shows it normally and the message goes away.
- While a folder is not available there is nothing to act on: paste, new folder and the
  empty-space menu do nothing, its tab takes no drops, and none of these waits on the
  unreachable server (asking Windows about one can block for 20–40 s).
- Navigating to a folder that cannot be opened (typed, Back, a link) does not move the
  tab: it stays where it was, the status bar shows the error, and an error beep sounds.
- A history entry whose folder no longer exists is dropped when Back/Forward fails on
  it, so Back/Forward can move past it instead of getting stuck.

### 4.4 No surprise windows
- Qoqdir itself shows no message boxes during normal work. Errors go to the status bar
  (with a short beep). The only dialogs are the ones Windows shows for file operations
  the user started (progress, overwrite questions, delete confirmation for permanent
  delete, Properties, Open With), plus the command-line help and the one confirmation
  in 5.4 (opening many files).
- An unreachable place never causes a Windows "can't access" window: e.g. a shortcut
  whose folder is on an unreachable server shows the status-bar message, it is not
  handed to Windows.
- Folders always open inside Qoqdir, never in a separate Explorer window — including
  folders opened through context-menu "Open", "Open in new window" or shortcuts.

### 4.5 Live but calm
- Changes made by other programs (files created, deleted, renamed, changed) appear in
  the current folder by themselves, within about half a second, keeping the selection,
  focus and scroll position. Many changes in a burst cause one refresh, not hundreds.
- If the current folder itself is deleted or renamed by another program, the tab
  notices (it shows the "not available" state of 4.3).
- Drives plugged in or removed update the tree and the This PC view.

### 4.6 Selection and focus feel right
- Going up (or back to a parent) selects the folder you came from.
- After deleting, the item that followed the deleted ones is selected.
- After creating a folder or renaming, the new or renamed item is selected.
- Something in the list always has the keyboard focus, so arrow keys work at once.
- Switching tabs restores each tab's selection, focus and scroll position.
- When the window is re-activated, keyboard focus returns to the control that had it.

### 4.7 Fast with big folders
Measured on an ordinary SSD laptop with a folder of 10,000 files (targets, not
limits — faster is welcome):
- Open the folder and show it: under 1 s (reference: ~0.4 s).
- Filter it as you type: under 0.1 s per keystroke.
- Re-sort it by any column: under 0.1 s.
- Copy all 10,000 selected files to the clipboard (Ctrl+C): under 3 s, without
  freezing longer than that (reference: ~1.5–2.5 s). Starting a drag, Delete or the
  context menu on 10,000 selected files also takes at most a few seconds.
- The copy or delete itself then runs at Windows' speed (it can take minutes for
  10,000 small files), and the window keeps responding meanwhile.
- The app starts and shows its window in well under a second.

### 4.8 Never lose user data or state silently
- Deleting goes to the Recycle Bin (Shift+Delete deletes permanently, with Windows'
  own confirmation).
- Renaming to an empty name, or to the same name, does nothing.
- Files dragged out of Qoqdir and dropped back on the same folder do nothing.

### 4.9 Quiet and polite
- A beep only for user errors (bad path, impossible action). Never during self-tests.
- Background work (copies started from Qoqdir, Properties windows) is allowed to
  finish when Qoqdir closes; closing the main window must not kill them.

### 4.10 Crashes
- If the program hits an internal bug, it stops cleanly (no silent memory corruption)
  and writes the reason to `%TEMP%\qoqdir-crash.log` when possible.

## 5. Features

### 5.1 Window layout (top to bottom)
1. **Toolbar row:** Back, Forward, Up buttons (simple arrow glyphs), the **address
   bar** (stretches), the **filter box** (shows the hint text
   "Filter  (just start typing)").
2. **Tab row:** the tabs, and a "+" button at the right that opens a new tab.
3. **Body:** the **folder tree** on the left, a draggable splitter, the **file list**
   on the right.
4. **Status bar:** left part = item counts and selection size; right part = messages.

Window title: `<folder name> - Qoqdir` (`This PC - Qoqdir` for This PC).
Back/Forward/Up are disabled when they cannot do anything.

### 5.2 Look
- Background of window, list and tree: soft gray **RGB(235, 235, 235)** (`#EBEBEB`),
  less glare than white. No dark mode.
- Tabs look like flat buttons: the selected tab is shown by Windows' normal flat-button
  look only — no permanent colored highlight. A tab is highlighted (blue) only while
  files are dragged over it, to show where they will drop.
- Tab captions are the folder's name; long names are shortened with "…".
- List and tree use the modern Explorer visual style and the system font at the
  current DPI; everything scales correctly when moved between monitors with
  different DPI (fonts, row heights, column widths).

### 5.3 Places and paths
- A location is either a folder path (local, `D:\`, UNC `\\server\share\...`) or
  **This PC**. This PC lists the drives with their label and icon; the Type column shows
  the drive type plus "· <free> free of <total>".
- The address bar shows the full path (or "This PC").
- The address bar accepts:
  - a folder path → opens it (in the current tab);
  - a file path → opens the file with Windows, the tab stays where it was;
  - `%ENVIRONMENT%` variables, forward slashes, quotes and spaces around the text,
    `D:` (means `D:\`), "This PC", and `..`-style segments;
  - `..` (up, like the Up button) or the name of a subfolder of the current folder →
    opened in this tab;
  - anything else → run as a command, the way the Windows Run box (Win+R) would:
    `cmd`, `notepad file.txt`, a web address. The working folder is the current
    folder. If Windows cannot run it: status bar "Can't find "<text>"" and a beep.
  - An invalid path leaves the text selected so it can be corrected.
- Address bar autocompletes folder names as you type (Windows' standard autocomplete).
- Esc in the address bar restores the current path and returns to the list.

### 5.4 File list
- Detail view with columns **Name**, **Date modified**, **Type**, **Size** (size right
  aligned; empty for folders). Icons are the real Windows icons: the type's icon
  appears instantly; files that carry their own icon (programs, shortcuts, icon
  files...) get it a moment later, without delaying the list.
- Folders are always listed before files.
- Sorting: click a column header; clicking again reverses. Name and Type start A→Z,
  Date and Size start newest/largest first. "Natural" order for numbers in names
  (`file2` before `file10`). Ties are ordered by name. Type sorts by the type text that
  is shown in the column. The header shows the sort arrow.
- Hidden files are hidden unless "show hidden" is on (Ctrl+H toggles it; the status bar
  says "Showing hidden files" / "Hiding hidden files").
- **Filter:** typing any printable character while the list has focus moves it into
  the filter box and filters at once. The filter matches anywhere in the name,
  case-insensitively; with `*` or `?` it is a wildcard pattern (`*.pdf`). The list
  shows matches from the top. Esc clears it. Down arrow moves to the list; Enter in
  the filter box opens the focused item. Changing folder clears the filter; a refresh
  of the same folder keeps it.
- Rename in place (F2 or the context menu): the name is pre-selected without its
  extension, like Explorer. Drives cannot be renamed here.
- Opening (Enter / double-click):
  - one folder → opens it in this tab;
  - one shortcut (`.lnk`) to a folder → opens that folder in this tab (Ctrl+Enter: in
    a new background tab); a shortcut to anything else (file, program) and `.url` web
    links → opened by Windows. The context menu's "Open" on a shortcut does the same;
  - files → opened by Windows; opening more than 15 files at once asks for
    confirmation first;
  - several folders → each in a new background tab;
  - Ctrl+Enter / Ctrl+double-click → folders in new background tabs.
- Middle-click on a folder → opens it in a new background tab.
- Status bar left part: `N items` (`N items (of M)` when filtered), and when something
  is selected `K selected  <total size of selected files>`.

### 5.5 Folder tree
- Roots, in order:
  1. **Bookmarks** — the `Bookmarks` folder next to the exe (see 5.6);
  2. the user's Desktop, Downloads, Documents, Pictures, Music, Videos (only those
     that exist; shown with Windows' names and icons);
  3. **This PC**, expanded at start, with the drives.
- Subfolders are read only when a node is expanded (never blocking, see 4.1), sorted
  naturally; hidden folders are not shown. A node shows an expand arrow until it is
  known to have no subfolders.
- Collapsing and expanding a node again shows its subfolders as they are now (new
  ones appear, deleted ones go).
- Clicking a node (or moving to it with the keyboard) opens it in the current tab;
  clicking the already selected node opens it again. Middle-click → new background tab.
- Right-click → the Windows context menu for that folder ("Open" opens here, "Open in
  new tab/window" opens a new background tab).
- Backspace in the tree goes up.
- The tree's width is set by dragging the splitter and is remembered.

### 5.6 Bookmarks
- Bookmarks are ordinary files: `.lnk` shortcuts and `.url` links (plus plain
  subfolders for grouping) inside the `Bookmarks` folder next to `qoqdir.exe`. No
  bookmark database, no special format: the user adds, renames and deletes them with
  normal file operations (e.g. Alt+drag a folder into Bookmarks to make a shortcut).
- The Bookmarks node is expanded at start and lists, one level deep, its subfolders
  first and then its shortcuts. Shortcuts show the name and icon Windows gives them
  ("Work", not "Work.lnk") and have no expand arrow.
- Choosing a bookmark shortcut in the tree behaves like opening it in the list (5.4):
  a folder target opens in the current tab, anything else is opened by Windows, an
  unreachable folder gives the status message (never a Windows popup).
  Middle-click (or the context menu's "Open in new tab") opens a folder target in a
  new background tab.
- The Bookmarks folder is created if missing; if it cannot be created, the app still
  runs.

### 5.7 Tabs
- Any number of tabs; each has its own folder, Back/Forward history and remembered
  view (selection, focus, scroll).
- New tab (Ctrl+T or "+") opens the current folder and switches to it.
- Close tab: Ctrl+W, middle-click on the tab, or the tab's context menu. The last tab
  cannot be closed.
- Tab context menu (right-click): "Duplicate tab", "Close tab   Ctrl+W",
  "Close other tabs" (disabled when there is only one).
- Switch: click, Ctrl+Tab / Ctrl+Shift+Tab (wraps around), Ctrl+1…Ctrl+8 (that tab
  or the last), Ctrl+9 (last tab).
- **Drop onto a tab:** dragging files over a tab highlights it; dropping copies/moves
  them into that tab's folder exactly like dropping on that folder in Explorer (Shift
  /Ctrl/Alt change the effect as usual). Not onto This PC, and not back into the
  folder the files are being dragged from.
- Tabs are **not remembered between runs** (see 5.10).

### 5.8 File operations (all done by Windows)
- **Copy / Cut / Paste:** Ctrl+C, Ctrl+X, Ctrl+V; the clipboard is shared with
  Explorer in both directions. Paste uses Windows' own paste (progress, conflicts,
  undo). Status bar: "N item(s) copied" / "N item(s) cut".
- **Delete:** Delete → Recycle Bin without a question; Shift+Delete → permanent, with
  Windows' confirmation.
- **Rename:** F2 (see 5.4).
- **New folder:** Ctrl+Shift+N creates "New folder" (or "New folder (2)", "(3)"...),
  selects it and starts renaming it.
- **Context menus:** right-click on items → the full Windows context menu for the
  selection (Shift+right-click → extended menu). Right-click on empty space → Paste
  (greyed when no files were copied; it does what Ctrl+V does), then the folder's own
  menu from Windows (New, Properties...). Windows' folder menu has no Paste of its own:
  Explorer adds it from its window, so Qoqdir must add it too. Menu key / Shift+F10 opens
  the menu for the focused item. Owner-drawn and cascading menus (e.g. "Send to",
  "New", "Open with", third-party entries) must work.
- **Properties:** Alt+Enter.
- **Drag and drop:** drag files out of the list to any app or folder (copy, move, link);
  drop files from anywhere onto the list (into the current folder) or onto a tab.
  Right-drag gives Windows' drop menu.

### 5.9 Keyboard and mouse summary
Anywhere in the window:
| Keys | Action |
|---|---|
| Ctrl+T / Ctrl+W | new tab / close tab |
| Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1…9 | switch tab |
| Ctrl+L, Alt+D, F4 | go to the address bar (text selected) |
| Ctrl+F, F3 | go to the filter box |
| F5, Ctrl+R | refresh |
| Ctrl+H | show/hide hidden files |
| Alt+Left / Alt+Right / Alt+Up | back / forward / up |
| Mouse back/forward buttons | back / forward |
| Tab, Shift+Tab | cycle focus: address → filter → tree → list |

In the file list:
| Keys | Action |
|---|---|
| Enter / Ctrl+Enter | open / open in new background tab |
| Backspace | up |
| Delete / Shift+Delete | recycle / delete permanently |
| F2 | rename |
| Ctrl+Shift+N | new folder |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Alt+Enter | properties |
| Esc | clear the filter |
| any character | start filtering |

Keys are only handled for Qoqdir's own window; dialogs opened from it (Properties,
copy progress) keep their own keys.

### 5.10 Command line (for Windows Terminal)
7-Zip-style switches; no console window appears.
```
qoqdir [switches] [folders...]

  {folder}     open the folder in a tab (relative paths work: qoqdir .)
  -t{folder}   the same, as a switch: -tD:\logs  -t"C:\My Files"
  -h, -?       show the help
```
- Each folder opens once in the given order (the same folder given twice, in any letter
  case or with a trailing backslash, opens one tab); the first is shown.
- With no folders, the user's home folder opens.
- `qoqdir .` opens the terminal's current folder; `D:` means `D:\`.
- A folder that cannot be opened still gets its tab, in the "not available" state
  (4.3) — useful for network drives that connect later.
- `-h`/`-?` shows the help text in a small window and exits with code 0. An unknown
  switch (or `-t` without a folder) shows "Unknown switch: -x" (or a clear error)
  followed by the help, and exits with code 1.
- The help text explains: quote folders with spaces, but don't end them with `\` before
  the closing quote (Windows reads `\"` as a literal quote); tabs are not remembered
  between runs; keep folders you use often as shortcuts in the Bookmarks folder next to
  `qoqdir.exe`. Examples: `qoqdir .` and `qoqdir -tD:\projects -tD:\logs`.
- `--selftest` runs the self-test (section 7.2); it is not shown in the help.

### 5.11 Settings (`qoqdir.ini`)
Saved when the window closes, loaded at start:
- window position and size (the normal, restored rectangle) and whether it was
  maximized (also when closed while minimized from maximized);
- tree width;
- column widths (stored independent of DPI);
- sort column and direction;
- show hidden files.
Nothing else — in particular **no tabs and no paths** (a saved network path could be
unavailable at next start, and the user prefers a predictable start). Missing or
broken values fall back to defaults; a missing or unwritable file is never an error.

## 6. Out of scope (do not build)
Saving tabs or sessions; dark mode or themes; a custom copy/move engine or custom
progress UI; file previews or thumbnails view; search across subfolders; archive
browsing; dual panes; plugins or scripting; a bookmark manager UI; translations; an
installer; auto-update; telemetry.

## 7. Tests (acceptance criteria)

### 7.1 Unit tests (pure logic, no window)
Cover at least, with concrete examples:
- Path rules: parent of a path (including `C:\` → This PC, UNC share roots); joining;
  leaf names; normalizing user input (quotes, spaces, forward slashes, `%VAR%`, `D:`,
  `..`, trailing backslash, "This PC"); deciding whether text is a path or a command;
  same-path comparison ignoring case; "which child of A leads to B" (used to select the
  folder you came from).
- Command-line parsing: plain folders, `-t`, duplicates removed (case and trailing
  backslash), `.`, `-h`, `-?`, `--selftest`, unknown switches and `-t` without a
  folder are errors.
- Sorting: folders first; natural number order; each column in both directions; ties
  by name; Type sorts by the shown type text.
- Filtering: substring, case-insensitive, wildcards (`*.pdf` must not match
  `x.pdf.txt`), empty filter.
- Listing a real folder: no `.`/`..` entries; a missing folder gives an error; This PC
  lists at least one drive; a path that is a file is reported as such.
- Shortcut detection by extension (`.lnk`, `.url`, any letter case).

### 7.2 Self-test (`qoqdir --selftest`)
Runs the real app on a fresh test folder in `%TEMP%` (e.g. `%TEMP%\qoqdir-selftest`),
drives it **from inside the process** (calling the app's own key handling and
controls, never global input), writes one line per step to a log next to that folder
(`ok`/`FAIL`, plus timings), and exits with code 0 only if every step passed. It must:
- never take focus from the user (the window opens without activating), never beep,
  never show a window that waits for an answer, never open an Explorer window
  (Windows' own copy-progress windows may appear briefly during copy steps);
- not read or write the real `qoqdir.ini` or the real Bookmarks folder (it uses a
  Bookmarks folder inside the test folder);
- use the reserved documentation address `192.0.2.1` for "unreachable network"
  cases (nothing ever answers there), and steps that cancel such loads must start
  while the load is still in progress;
- give each step a timeout, and continue after a failing step;
- leave nothing behind except the test folder and log in `%TEMP%` (one small file in
  the Recycle Bin from the delete test is acceptable); it may replace the clipboard.

Scenarios (each: action → expected result). The test folder starts with a few files
(including one with a Chinese name), a subfolder `sub`, and the test Bookmarks folder.
1. Start → the test folder is shown with the expected item count; the Chinese name is
   shown correctly.
2. Typing letters in the list → focus moves to the filter box, the list is filtered;
   Esc → filter cleared, focus back in the list.
3. Ctrl+C on a file → the clipboard holds exactly that file (readable by Explorer).
4. Filter "sub" + Enter → `sub` opens.
5. Ctrl+V in `sub` → the file is pasted by Windows' copy engine and appears in the list
   by itself.
6. Backspace → goes up and `sub` is selected. Alt+Left → back; Alt+Right → forward.
7. Ctrl+Shift+N → "New folder" is created and in rename mode; typing a name + Enter →
   renamed, and the new name is selected.
8. F2 on `name.txt` → only `name` is pre-selected; typing a new name keeps `.txt`.
9. Delete → the file goes to the Recycle Bin and disappears from the list.
10. Another program creates a file in the shown folder → it appears by itself.
11. Another program deletes the shown folder → the tab stays on it, list empty, status
    "not available … (F5 tries again)"; the folder is recreated and F5 → shown again,
    message gone.
12. Ctrl+T → second tab; Ctrl+W → back to one tab, same folder.
13. Typing a missing path + Enter → status error, the tab stays where it was.
14. Typing a folder path + Enter → it opens.
15. Typing `\\192.0.2.1\share` + Enter → Enter returns within 100 ms; opening another
    folder right away cancels it, and that folder is shown.
16. Typing `sub` + Enter → `sub` opens in this tab; typing `..` + Enter → back up. No
    Explorer window.
17. Open a folder, go back, delete that folder, press Forward/Back onto it → the dead
    entry is dropped from history; the tab does not get stuck.
18. Enter on a shortcut to a folder → that folder opens in this tab.
19. Ctrl+Enter on that shortcut → a new background tab with that folder; the current
    tab is unchanged. No Explorer window.
20. A tab whose network folder is not available (the test may put it into that state
    directly: a real failure can take minutes to arrive) → Ctrl+V, Ctrl+Shift+N and
    dragging a file over its tab together return within 100 ms.
21. Enter on a shortcut to a folder that does not exist → the status bar names that
    folder and the tab stays where it was; no window from any process appears.
22. Holding F5 (20 repeats) while a folder loads → only one load is started.
23. A change notification arriving while a folder is being opened → the open is not
    cancelled; the new folder is shown.
24. A slow refresh shows "Loading…", which disappears afterwards.
25. A folder containing a program → its own icon arrives from the background.
26. Sorting by Type → the Type column is in order.
27. The Bookmarks folder exists and is the first tree root, labelled "Bookmarks".
28. Context menus can be built for This PC, a drive and a file.
29. Dropping a file (simulated drag inside the process) on a second tab → the file is
    copied into that tab's folder; no tab stays highlighted afterwards.
30. Middle-click on a folder in the list → a new background tab with that folder; the
    current tab is unchanged.
31. Middle-click on a tree node → a new background tab with that folder.
32. The tree lists the drives under This PC.
33. Expanding a tree folder lists exactly its (non-hidden) subfolders.
34. With a subfolder `one` and a shortcut `sub.lnk` in Bookmarks, rebuilding the tree
    as at startup → Bookmarks is open and lists "one", then "sub" (no `.lnk`).
35. Moving to the "sub" bookmark with the keyboard → the `sub` folder opens.
36. Creating `Bookmarks\two`, then collapsing and expanding Bookmarks (as the user
    would, with Left/Right keys) → "one, two, sub".
37. A 10,000-file folder loads within the target of 4.7.
38. Filtering while scrolled to the bottom → the matches are shown from the top.
39. Filtering and sorting 10,000 files are within the targets of 4.7.
40. Copying 10,000 selected files to the clipboard is within the target of 4.7 and the
    clipboard then holds 10,000 files.

### 7.3 Command-line checks (on a private copy of the exe in a temp folder)
- `-tC:\Users\Public -tc:\users\public` → one window titled "Public - Qoqdir"; after
  closing, `qoqdir.ini` contains no tabs/paths and a `Bookmarks` folder exists next to
  the exe.
- No arguments → the home folder is shown.
- `-h` → help, exit code 0; `-x` → error + help, exit code 1.

### 7.4 Watching for popups
When running the self-test, a helper script should list every new visible window that
appears from **any** process during the run (not only Qoqdir's), so a Windows error
window caused by the app is caught. Only the test's own main window (and Windows'
copy-progress windows from the test's own process) is expected. The script only
reports: it must never close or touch another program's window, because the user keeps
working while tests run and other programs' windows show up in the list too.

## 8. Build order
Build and test in this order; each step must pass its tests before the next.
1. **Skeleton:** exe with manifest, empty main window, layout, DPI handling, settings
   load/save, command line + help. Unit tests for paths and the command line.
2. **Folder reading and the list:** background loading with "newest wins", the list
   with columns, icons, sorting, filtering, status bar. Unit tests for sort/filter.
3. **Navigation:** address bar, Back/Forward/Up, history, "not available" state,
   keyboard handling, and the in-process self-test framework.
4. **Shell integration:** context menus, clipboard, paste, delete, rename, new folder,
   properties, drag and drop, opening files and shortcuts.
5. **Live updates:** change notifications, drive changes, coalesced refresh.
6. **Tree**, then **tabs** (including drop onto tabs and middle-click), then
   **Bookmarks**.
7. **Speed pass:** the 10,000-file scenarios; fix anything over target.
8. **Finish:** all self-test scenarios, command-line checks, popup watch, README
   (build steps, command line, keyboard table, Bookmarks, where files are written).

Done means: every unit test and self-test step passes, the command-line checks pass,
no unexpected window appears during the self-test, and the code is small and readable.
