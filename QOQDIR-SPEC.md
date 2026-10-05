# Qoqdir specification

This document describes what Qoqdir does and how it must behave, in enough detail
to build it again from an empty folder. It has no code and doesn't prescribe a
design: use the simplest one that meets every rule here.

Sections 4 (behavior) and 7 (tests) are what a build is judged against.

## 1. What Qoqdir is

A light, fast, reliable file explorer for Windows 10/11, meant to replace Explorer
for everyday work and to be started from Windows Terminal. It takes the idea of
CubicExplorer (tabs, a folder tree, a fast detail list), but it is a new and much
smaller program.

Priorities, in order:

1. Reliable: it never freezes, never loses a tab, never shows surprise popups,
   never corrupts or loses files.
2. Simple: small code, simple logic, few moving parts. Bugs are avoided by design,
   not patched one by one. When in doubt, leave a feature out.
3. Fast: faster than Explorer to start, to open folders and to filter.
4. Easy to use: keyboard first, Explorer's shortcuts, nothing new to learn.
5. Portable and easy to maintain: one exe, nothing installed, readable code.

## 2. Working rules

- Keep it small. One clear mechanism is better than several special cases. If a
  rule here would need a lot of code, look for a simpler rule first.
- Don't add features that aren't in this document (no themes, plugins, previews,
  search indexes, tabs that come back by themselves).
- Let Windows do the file work. Copy, move, delete, rename, paste, context menus,
  properties, Open With, icons and type names all come from the Windows shell, so
  they behave exactly like Explorer: progress windows, conflict questions, undo,
  Recycle Bin. No copy engine or context menu entries of our own for these.
- Tests never touch the desktop session: no global keystrokes or mouse input, no
  taking the focus, no dialogs, no sounds. They drive the app from inside its own
  process, and they never close windows or delete files they didn't create.
- Run the unit tests and the self-test before calling a change done. For a bug,
  first add a test that fails, then fix it.

## 3. Hard constraints

- Platform: Windows 10 and 11, x64, with the native Win32 UI (standard Windows
  controls, so it looks like part of Windows). No web view, no UI framework, no
  .NET, no runtime to install.
- Language: C++ or Rust (see 3.1). Build tools: MinGW-w64 GCC 13 (with CMake and
  Ninja) and, for the Rust version, a Rust toolchain for the GNU target. Both run
  from their own folders, with nothing installed system-wide and no Visual Studio.
- Output: one `qoqdir.exe`, statically linked, with no DLLs beside it. Its
  embedded manifest asks for per-monitor DPI awareness (v2), long paths and the
  modern common controls. Well under 1 MB.
- Portable: everything it writes lives next to the exe:
  - `qoqdir.ini` for the settings, created on the first save. If the folder is
    read-only, nothing is saved, and that is not an error.
  - `Bookmarks\`, created at start if missing.
  - Nothing in the registry or AppData. The one exception is the crash log in
    `%TEMP%` (4.10).
- Names: "Qoqdir" in window titles and messages, lowercase `qoqdir` in every file
  name (`qoqdir.exe`, `qoqdir.ini`, `qoqdir-*.log`).
- Unicode everywhere: file names in any language show, sort, rename and open
  correctly. Paths over 260 characters must never crash or hang the app; they work
  as far as Windows allows (its long-path setting is off by default, and Explorer
  can't open such folders either).

### 3.1 Notes from two earlier builds

Qoqdir was first built twice, in C++ and in Rust, with the same features and tests.

- Both met every rule here at the same speed. Exe size: about 730 KB for C++,
  570 KB for Rust.
- The Rust version needed about 45% more lines (3,700 against 2,500 without
  tests), mostly Win32 calls and pointer handling through the `windows` crate.
- A clean C++ build took under a minute. Rust's first build took about 7 minutes
  (the `windows` crate), later ones about a minute.
- Rust turned one real bug (a list row pointing at an item that had just been
  replaced) into a clean stop with a message, where C++ silently misused memory.
  Rust's own trap is the other way round: Windows often calls back into the app
  while it is inside a Windows call, so shared state must never stay borrowed
  across one.
- Rust's own `dlltool` failed on the build machine, so the cargo config pointed
  at MinGW's `dlltool.exe` instead. COM objects in Rust (drop targets) also need
  the `windows-core` crate. C++ needed only MinGW, static linking and the manifest
  in a resource file.
- Two versions double every change and every test run, and the second version
  found only that one bug; the tests and code reviews found the rest. Both are
  still kept and level, but only the C++ one ships. Each must stop loudly on such
  mistakes: Rust does that by itself (with `overflow-checks` on for release
  builds); C++ needs GCC's library checks (`_GLIBCXX_ASSERTIONS`) and its signed
  overflow trap.

## 4. Behavior rules

These describe what the user sees. Each must hold in every situation.

### 4.1 Never freeze
- The window stays responsive while any folder is read, however slow: a sleeping
  USB disk, a slow NAS, a network share that doesn't answer at all (Windows can
  take 20 to 60 s to give up). A key press or click reacts at once (well under
  100 ms), including Enter on such a path.
- That goes for every place a path is touched: opening a folder, the address bar,
  expanding the tree, a shortcut to a network folder, icons of files on a slow
  disk, refreshes.
- If a load takes more than about a quarter second, the status bar says
  "Loading…" until the folder arrives or the load is replaced.

### 4.2 The newest request wins
- A new navigation (another folder, Back, another tab) replaces any load still
  running. When the old result arrives, it is ignored.
- A tab's folder, history and title change only once its new folder has loaded.
  A failed or slow load never leaves a tab half updated.
- Refreshes never pile up: refreshing while the folder is still loading starts
  nothing new, so holding F5 on a slow network folder starts one load.
- An automatic refresh (the folder changed on disk) never cancels a navigation or
  interrupts a rename. It waits until they are done.

### 4.3 Tabs keep their folder
- A tab never changes its folder by itself. If the folder is unavailable
  (deleted, drive removed, network not connected yet), the tab stays on it with an
  empty list, and the status bar says
  `"<folder>" is not available: <Windows error text>   (F5 tries again)`.
  When the folder is back, F5 shows it and the message goes away.
- While a folder is not available there is nothing to act on: paste, new folder
  and the empty-space menu do nothing, its tab takes no drops, and none of them
  waits on the unreachable server (asking Windows about one can block for 20 to
  40 s).
- Going to a folder that can't be opened (typed, Back, a link) doesn't move the
  tab. It stays where it was, the status bar shows the error, and it beeps.
- A history entry whose folder is gone is dropped when Back or Forward fails on
  it, so Back and Forward can move past it.

### 4.4 No surprise windows
- Qoqdir shows no message boxes of its own in normal use. Errors go to the status
  bar with a short beep. The only dialogs are the ones Windows shows for file
  operations the user started (progress, overwrite questions, confirming a
  permanent delete, Properties, Open With), the command-line help, and the
  question before opening many files (5.4).
- An unreachable place never causes a Windows "can't access" window. A shortcut to
  a folder on a server that is off, for example, gives the status message; it is
  not handed to Windows.
- Folders always open inside Qoqdir, never in an Explorer window. That includes
  folders opened with the context menu's Open or Open in new window, and through
  shortcuts.

### 4.5 Live but calm
- Changes made by other programs (files created, deleted, renamed, changed) show
  up by themselves within about half a second, keeping the selection, focus and
  scroll position. A burst of changes causes one refresh, not hundreds.
- If the shown folder itself is deleted or renamed by another program, the tab
  notices and shows the "not available" state (4.3).
- Plugging in or removing a drive updates the tree and This PC.

### 4.6 Selection and focus
- Going up (or back to a parent) selects the folder you came from.
- After a delete, the item that followed the deleted ones is selected.
- After creating or renaming an item, that item is selected.
- Something in the list always has the keyboard focus, so arrow keys work at once.
- Switching tabs brings back each tab's selection, focus and scroll position.
- When the window is activated again, the focus goes back where it was.

### 4.7 Fast with big folders
Targets on an ordinary SSD laptop with a folder of 10,000 files (faster is fine):
- Open and show the folder: under 1 s (about 0.4 s measured).
- Filter as you type: under 0.1 s per key.
- Sort by any column: under 0.1 s (0.02 to 0.07 s measured). Names compare by sort
  keys made once per name, in exactly the order Explorer uses.
- Copy all 10,000 selected files to the clipboard (Ctrl+C): under 3 s, without
  freezing longer than that (1.5 to 2.5 s measured). Starting a drag, Delete or
  the context menu on 10,000 selected files also takes a few seconds at most.
- The copy or delete itself runs at Windows' speed (minutes for 10,000 small
  files), and the window keeps responding meanwhile.
- The window shows up well under a second after start.

### 4.8 Never lose data
- Delete goes to the Recycle Bin. Shift+Delete deletes for good, after Windows'
  own question.
- Renaming to an empty name, or to the same name, does nothing.
- Files dragged out of Qoqdir and dropped back on the same folder do nothing.

### 4.9 Quiet
- It beeps only for user errors (a bad path, an impossible action), and never in
  the self-test.
- Background work started from Qoqdir (copies, Properties windows) may finish
  after the main window closes. Closing the window doesn't kill it.

### 4.10 Crashes
- On an internal bug the program stops at once instead of running on with
  corrupt memory, and when it can, it writes the reason to
  `%TEMP%\qoqdir-crash.log`, after the time and the version.

## 5. Features

### 5.1 Window layout, top to bottom
1. Toolbar: Back, Forward and Up buttons (plain arrow glyphs), the address bar
   (it stretches), and the filter box with the hint "Filter  (just start typing)".
2. The tabs, with a "+" button at the right for a new tab.
3. The folder tree on the left, a splitter, the file list on the right.
4. Status bar: item counts and selection size on the left, messages on the right.

The window title is `<folder name> - Qoqdir` (`This PC - Qoqdir` for This PC).
Back, Forward and Up are disabled when they can't do anything.

### 5.2 Look
- Window, list and tree have a soft gray background, RGB(235, 235, 235)
  (`#EBEBEB`), easier on the eyes than white. No dark mode.
- Tabs look like flat buttons. The selected tab shows only Windows' normal flat
  button look, with no colored highlight. A tab turns blue only while files are
  dragged over it, to show where they will go.
- Tab captions are folder names; long ones are cut short with "…".
- List and tree use Explorer's visual style and the system font at the current
  DPI, and everything scales right when the window moves to a monitor with a
  different DPI (fonts, row heights, column widths).

### 5.3 Places and paths
- A location is a folder path (local, `D:\`, or UNC `\\server\share\...`) or
  This PC. This PC lists the drives with label and icon, and their Type column
  shows the drive type and "· <free> free of <total>".
- The address bar shows the full path, or "This PC".
- The address bar accepts:
  - a folder path: opens it in this tab;
  - a file path: Windows opens the file, and the tab stays where it is;
  - `%VARIABLES%`, forward slashes, quotes and spaces around the text, `D:` (for
    `D:\`), "This PC", and `..` inside a path;
  - `..` (up, like the Up button) or the name of a subfolder of the current
    folder: opened in this tab;
  - anything else runs as a command, the way the Windows Run box (Win+R) would:
    `cmd`, `notepad file.txt`, a web address. It runs in the current folder. If
    Windows can't run it, the status bar says `Can't find "<text>"` and it beeps.
  - A path that doesn't work stays selected in the box, so it can be fixed.
- Folder names autocomplete as you type (Windows' own autocomplete).
- Esc in the address bar puts the current path back and returns to the list.

### 5.4 File list
- Detail view with Name, Date modified, Type and Size (size right aligned, empty
  for folders). Icons are Windows' own: the type's icon shows at once, and files
  with their own icon (programs, shortcuts, icon files) get it a moment later
  without holding up the list.
- Folders always come before files.
- Click a column header to sort, and click again to reverse. Name and Type start A
  to Z, Date and Size start with the newest or largest. Numbers in names sort
  naturally (`file2` before `file10`), ties go by name, and Type sorts by the type
  text shown in the column. The header shows the sort arrow.
- Hidden files are hidden unless "show hidden" is on (Ctrl+H; the status bar says
  "Showing hidden files" or "Hiding hidden files").
- Filter: typing a character while the list has the focus moves it into the
  filter box, which filters at once. It matches anywhere in the name, ignoring
  case; with `*` or `?` it is a wildcard pattern (`*.pdf`). Matches show from the
  top, except that a highlighted item that matches stays highlighted and in view.
  Esc clears the filter, Down moves to the list, and Enter opens the focused item.
  Going to another folder clears the filter; refreshing the same folder keeps it.
- Rename in place with F2 or the context menu. The name is selected without its
  extension, as in Explorer. Drives can't be renamed here.
- Opening (Enter or double-click):
  - one folder opens in this tab;
  - one shortcut (`.lnk`) to a folder opens that folder in this tab (Ctrl+Enter:
    in a new background tab). A shortcut to anything else, and `.url` web links,
    are opened by Windows. The context menu's Open on a shortcut does the same.
  - files are opened by Windows; opening more than 15 at once asks first;
  - several folders open in new background tabs, one each;
  - Ctrl+Enter or Ctrl+double-click opens folders in new background tabs.
- Middle-click on a folder opens it in a new background tab.
- The status bar shows `N items` (`N items (of M)` when filtered) and, with a
  selection, `K selected  <total size of the selected files>`.

### 5.5 Folder tree
- Roots, in order:
  1. Bookmarks, the `Bookmarks` folder next to the exe (5.6);
  2. the user's Desktop, Downloads, Documents, Pictures, Music and Videos (those
     that exist, with Windows' names and icons);
  3. This PC with the drives, expanded at start.
- Subfolders are read when a node is expanded (never blocking, 4.1) and sorted
  naturally. Hidden folders aren't shown. A node has an expand arrow until it is
  known to have no subfolders.
- Collapsing and expanding a node again shows its subfolders as they are now.
- Clicking a node, or moving to it with the keyboard, opens it in this tab, and
  clicking the selected node opens it again. Middle-click opens a new background
  tab.
- Right-click gives Windows' context menu for that folder (Open opens here, Open
  in new tab or window opens a background tab).
- Backspace in the tree goes up.
- The splitter sets the tree's width, which is remembered.

### 5.6 Bookmarks
- Bookmarks are ordinary files: `.lnk` shortcuts and `.url` links, and plain
  subfolders for grouping, in the `Bookmarks` folder next to `qoqdir.exe`. There
  is no bookmark database or special format: they are added, renamed and deleted
  like any file (for example, Alt+drag a folder into Bookmarks).
- The Bookmarks node is open at start and lists, one level deep, its subfolders
  and then its shortcuts. Shortcuts show the name and icon Windows gives them
  ("Work", not "Work.lnk") and have no expand arrow.
- Choosing a bookmark shortcut in the tree works like opening it in the list
  (5.4): a folder opens in this tab, anything else is opened by Windows, and an
  unreachable folder gives the status message, never a Windows popup.
  Middle-click (or Open in new tab) opens a folder in a new background tab.
- The Bookmarks folder is created if missing. If it can't be created, the app
  still runs.

### 5.7 Tabs
- Any number of tabs. Each has its own folder, Back/Forward history and
  remembered view (selection, focus, scroll).
- New tab (Ctrl+T or "+") opens the current folder and switches to it.
- Close a tab with Ctrl+W, a middle-click on it, or its context menu. The last
  tab can't be closed.
- Tab context menu: "Duplicate tab", "Close tab   Ctrl+W", "Close other tabs"
  (disabled when there is only one), then "Save tabs   Ctrl+S" and "Open saved
  tabs   Ctrl+O".
- Switch with a click, Ctrl+Tab and Ctrl+Shift+Tab (they wrap around), Ctrl+1 to
  Ctrl+8 (that tab, or the last), Ctrl+9 (the last tab).
- Drop onto a tab: dragging files over a tab highlights it, and dropping copies or
  moves them into that tab's folder exactly as dropping on that folder in Explorer
  would (Shift, Ctrl and Alt work as usual). Not onto This PC, not onto a tab
  whose folder hasn't loaded here yet or failed, and not back into the folder the
  files come from.
- Tabs aren't remembered between runs by themselves. Ctrl+S saves them as
  shortcuts in `Bookmarks\Tabs`, numbered in tab order ("1 logs.lnk"); it replaces
  the shortcuts there and leaves anything else. Ctrl+O opens their folders as tabs
  after the open ones, skips any already open, and shows the first new one.
  Renaming the folder keeps a set. Both work on a worker thread, since a folder
  may be on a server that is off.

### 5.8 File operations (all done by Windows)
- Copy, cut and paste: Ctrl+C, Ctrl+X, Ctrl+V. The clipboard works with Explorer
  both ways. Paste is Windows' own paste (progress, conflicts, undo). The status
  bar says "N item(s) copied" or "N item(s) cut".
- Delete goes to the Recycle Bin without a question; Shift+Delete deletes for
  good after Windows' question.
- Rename: F2 (5.4).
- New folder: Ctrl+Shift+N creates "New folder" (or "New folder (2)" and so on),
  selects it and starts renaming it.
- Context menus: right-click on items gives Windows' full context menu for the
  selection (Shift+right-click gives the extended menu). Right-click on empty
  space gives Paste and Paste shortcut (greyed when no files were copied; Paste
  does what Ctrl+V does), then the folder's own menu from Windows (New,
  Properties...). Windows' folder menu has neither of those two, because Explorer
  adds them itself, so Qoqdir adds them too. Like Explorer, Ctrl+C marks the files
  "copy or link"; otherwise Windows refuses Paste shortcut. The Menu key or
  Shift+F10 opens the menu for the focused item. Owner-drawn and cascading menus
  (Send to, New, Open with, other programs' entries) must work.
- Properties: Alt+Enter.
- Drag and drop: drag files out of the list to any program or folder (copy, move,
  link), and drop files from anywhere onto the list (the current folder) or onto a
  tab. A right-drag gives Windows' drop menu.

### 5.9 Keys and mouse
Anywhere in the window:

| Keys | Action |
|---|---|
| Ctrl+T / Ctrl+W | new tab / close tab |
| Ctrl+S / Ctrl+O | save the tabs / open the saved tabs |
| Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1 to 9 | switch tab |
| Ctrl+L, Alt+D, F4 | address bar (text selected) |
| Ctrl+F, F3 | filter box |
| F5, Ctrl+R | refresh |
| Ctrl+H | show or hide hidden files |
| Alt+Left / Alt+Right / Alt+Up | back / forward / up |
| Mouse back and forward buttons | back / forward |
| Tab, Shift+Tab | move the focus: address, filter, tree, list |

In the file list:

| Keys | Action |
|---|---|
| Enter / Ctrl+Enter | open / open in a new background tab |
| Backspace | up |
| Delete / Shift+Delete | recycle / delete for good |
| F2 | rename |
| Ctrl+Shift+N | new folder |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Alt+Enter | properties |
| Esc | clear the filter |
| any character | start filtering |

Keys are handled only for Qoqdir's own window. Dialogs opened from it
(Properties, copy progress) keep their own keys.

### 5.10 Command line
7-Zip style switches. No console window appears.

```
qoqdir [switches] [folders...]

  {folder}     open the folder in a tab (relative paths work: qoqdir .)
  -t{folder}   the same, as a switch: -tD:\logs  -t"C:\My Files"
  -h, -?       show the help
```

- Each folder opens once, in the given order (the same folder twice, in any
  letter case or with a trailing backslash, gives one tab). The first is shown.
- With no folders, the home folder opens.
- `qoqdir .` opens the terminal's current folder, and `D:` means `D:\`.
- A folder that can't be opened still gets its tab, in the "not available" state
  (4.3). That helps with network drives that connect later.
- `-h` and `-?` show the version and the help in a small window and exit with
  code 0. An unknown switch (or `-t` without a folder) shows the error ("Unknown
  switch: -x") above the help and exits with code 1.
- The help says: quote folders with spaces, but don't end them with `\` before the
  closing quote (Windows reads `\"` as a quote); keep folders you use often as
  shortcuts in the Bookmarks folder next to `qoqdir.exe`; tabs aren't remembered
  by themselves, but Ctrl+S saves them and Ctrl+O opens them again. Examples: `qoqdir .` and `qoqdir -tD:\projects -tD:\logs`.
- `--selftest` runs the self-test (7.2) in `qoqdir-test.exe`; the exe users get
  treats it as an unknown switch. It isn't in the help.

### 5.11 Settings (`qoqdir.ini`)
Saved when the window closes and loaded at start:
- window position and size (the normal, restored size) and whether it was
  maximized (also when it was minimized from maximized);
- tree width;
- column widths (stored independent of DPI);
- sort column and direction;
- show hidden files.

Nothing else: no tabs and no paths. A saved network path could be unavailable at
the next start, and a predictable start is better. Missing or broken values fall
back to the defaults, and a missing or read-only file is never an error.

## 6. Out of scope

Not to be built: tabs or sessions that come back by themselves (saved tabs are
shortcuts, 5.7), dark mode or themes, a copy engine or progress window of our own,
previews or thumbnails, search across subfolders, archive browsing, dual panes,
plugins or scripting, a bookmark manager, other languages, an installer,
auto-update, telemetry.

## 7. Tests

### 7.1 Unit tests (logic only, no window)
At least these, with concrete examples:
- Paths: the parent of a path (including `C:\` to This PC, and UNC share roots),
  joining, leaf names, cleaning up typed input (quotes, spaces, forward slashes,
  `%VAR%`, `D:`, `..`, trailing backslash, "This PC"), telling a path from a
  command, comparing paths ignoring case, and which child of A leads to B (used to
  select the folder you came from).
- Command line: plain folders, `-t`, duplicates removed (letter case, trailing
  backslash), `.`, `-h`, `-?`, `--selftest`; unknown switches and `-t` without a
  folder are errors.
- Sorting: folders first, natural number order, each column both ways, ties by
  name, Type by the shown type text.
- Filtering: substring, ignoring case, wildcards (`*.pdf` must not match
  `x.pdf.txt`), an empty filter.
- Listing a real folder: no `.` or `..`, a missing folder gives an error, This PC
  lists at least one drive, and a path that is a file is reported as one.
- Spotting shortcuts by extension (`.lnk`, `.url`, any letter case).

### 7.2 Self-test (`qoqdir-test --selftest`)
`qoqdir-test.exe` is the same compiled app as `qoqdir.exe` plus the self-test,
which users don't get. It runs the real app on a fresh test folder in `%TEMP%`
(for example `%TEMP%\qoqdir-selftest`) and drives it from inside the process,
through the app's own key handling and controls, never global input. It writes
one line per step (`ok` or `FAIL`, with timings) to a log next to that folder,
and exits with code 0 only if every step passed. It must:
- never take the focus (the window opens without activating), never beep, never
  show a window that waits for an answer, and never open an Explorer window
  (Windows' copy progress may flash up during copy steps);
- leave the real `qoqdir.ini` and Bookmarks alone (it uses a Bookmarks folder in
  the test folder);
- use the documentation address `192.0.2.1` for an unreachable network (nothing
  ever answers there), and start the steps that cancel such a load while it is
  still running;
- give each step a timeout and carry on after a failed step;
- leave nothing behind but the test folder and log in `%TEMP%` (one small file in
  the Recycle Bin from the delete test is fine). It may replace the clipboard.

The test folder starts with a few files (one with a Chinese name), a subfolder
`sub` and the test Bookmarks folder. Each scenario is an action and the expected
result:

1. Start: the test folder shows the expected item count, and the Chinese name
   shows correctly.
2. Typing letters in the list moves the focus to the filter box and filters the
   list. Esc clears the filter and goes back to the list.
3. Ctrl+C on a file puts exactly that file on the clipboard (readable by
   Explorer).
4. Filter "sub", then Enter: `sub` opens.
5. Ctrl+V in `sub`: Windows' copy engine pastes the file, and it shows up in the
   list by itself.
6. Backspace goes up and selects `sub`. Alt+Left goes back, Alt+Right forward.
7. Ctrl+Shift+N creates "New folder" in rename mode. Typing a name and Enter
   renames it, and the new name is selected.
8. F2 on `name.txt` selects only `name`, and typing a new name keeps `.txt`.
9. Delete sends the file to the Recycle Bin and it leaves the list.
10. Another program creates a file in the shown folder: it shows up by itself.
11. Another program deletes the shown folder: the tab stays on it with an empty
    list and the "not available … (F5 tries again)" message. The folder comes
    back, and F5 shows it again without the message.
12. Ctrl+T opens a second tab; Ctrl+W goes back to one tab on the same folder.
13. Typing a missing path and Enter gives an error in the status bar, and the tab
    stays where it was.
14. Typing a folder path and Enter opens it.
15. Typing `\\192.0.2.1\share` and Enter returns within 100 ms. Opening another
    folder right away cancels it, and that folder is shown.
16. Typing `sub` and Enter opens `sub` in this tab; `..` and Enter goes back up.
    No Explorer window.
17. Open a folder, go back, delete that folder, then press Forward and Back onto
    it: the dead entry is dropped from history and the tab doesn't get stuck.
18. Enter on a shortcut to a folder opens that folder in this tab.
19. Ctrl+Enter on that shortcut opens a new background tab with that folder, and
    the current tab doesn't change. No Explorer window.
20. In a tab whose network folder isn't available (the test may set that state
    directly, since a real failure can take minutes), Ctrl+V, Ctrl+Shift+N and
    dragging a file over the tab together return within 100 ms.
21. Enter on a shortcut to a folder that doesn't exist: the status bar names that
    folder and the tab stays where it was. No window from any program appears.
22. Holding F5 (20 repeats) while a folder loads starts only one load.
23. A change notification while a folder is being opened doesn't cancel the
    open, and the new folder is shown.
24. A slow refresh shows "Loading…", which goes away afterwards.
25. In a folder with a program in it, the program's own icon arrives later.
26. Sorting by Type puts the Type column in order.
27. The Bookmarks folder exists and is the first tree root, labelled "Bookmarks".
28. Context menus can be built for This PC, a drive and a file.
29. Dropping a file (a drag simulated inside the process) on a second tab copies
    it into that tab's folder, and no tab stays highlighted afterwards.
30. Middle-click on a folder in the list opens a new background tab with that
    folder, and the current tab doesn't change.
31. Middle-click on a tree node opens a new background tab with that folder.
32. The tree lists the drives under This PC.
33. Expanding a tree folder lists exactly its subfolders (not the hidden ones).
34. With a subfolder `one` and a shortcut `sub.lnk` in Bookmarks, rebuilding the
    tree as at startup: Bookmarks is open and lists "one", then "sub" (no `.lnk`).
35. Moving to the "sub" bookmark with the keyboard opens the `sub` folder.
36. After creating `Bookmarks\two`, collapsing and expanding Bookmarks with the
    Left and Right keys shows "one", "two", "sub".
37. A 10,000-file folder loads within the target of 4.7.
38. Filtering while scrolled to the bottom shows the matches from the top.
39. Filtering and sorting 10,000 files are within the targets of 4.7.
40. Copying 10,000 selected files to the clipboard is within the target of 4.7,
    and the clipboard then holds 10,000 files.
41. With three tabs (the test folder, `sub`, This PC), Ctrl+S leaves one shortcut
    per tab in `Bookmarks\Tabs`, in tab order: an old shortcut there is gone and a
    text file there stays. After closing two tabs, Ctrl+O opens `sub` and This PC
    after the test folder (which is open already) and shows `sub`.

### 7.3 Command-line checks
Run on a copy of the exe in a temp folder:
- `-tC:\Users\Public -tc:\users\public` opens one window titled "Public - Qoqdir".
  After closing it, `qoqdir.ini` holds no tabs or paths, and a `Bookmarks` folder
  exists next to the exe.
- No arguments: the home folder is shown.
- `-h` shows the help and exits with code 0. `-x` shows the error with the help
  and exits with code 1.

### 7.4 Watching for popups
While the self-test runs, a script lists every new visible window from any
program, not only Qoqdir, so a Windows error window caused by the app gets
noticed. Only the test's own main window (and Windows' copy progress from the
test's process) is expected. The script only reports: it never closes or touches
another program's window, because other programs keep running and showing
windows during the test.

## 8. Build order

Build and test in this order, each step passing its tests before the next:

1. Skeleton: the exe with its manifest, an empty main window, layout, DPI
   handling, loading and saving settings, command line and help. Unit tests for
   paths and the command line.
2. Reading folders and the list: background loading where the newest request
   wins, columns, icons, sorting, filtering, status bar. Unit tests for sorting
   and filtering.
3. Navigation: address bar, Back/Forward/Up, history, the "not available" state,
   keys, and the in-process self-test.
4. Shell: context menus, clipboard, paste, delete, rename, new folder,
   properties, drag and drop, opening files and shortcuts.
5. Live updates: change notifications, drive changes, combined refreshes.
6. The tree, then tabs (with drops onto tabs and middle-click), then Bookmarks.
7. Speed: the 10,000-file scenarios; fix anything over target.
8. Finish: all self-test scenarios, the command-line checks, the popup watch, and
   a README (build steps, command line, keys, Bookmarks, which files it writes).

Done means every unit test and self-test step passes, the command-line checks
pass, no unexpected window appears during the self-test, and the code is small
and readable.
