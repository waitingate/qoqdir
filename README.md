# Qoqdir

A small, fast file explorer for Windows 10/11 with tabs, a folder tree and a
detail list, meant to be started from Windows Terminal. Written in C++ with plain
Win32. Everything is in one portable exe: no runtime, no installer.

## Build

You need MinGW-w64 GCC 13 at `C:\mingw64` (CMake and Ninja on the PATH). Nothing
is installed on the system.

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH
cmake -S . -B build -G Ninja
cmake --build build                      # -> build\qoqdir.exe (static, manifest embedded)
```

The build turns on GCC's runtime checks (`_GLIBCXX_ASSERTIONS`). A bad index stops
the program and writes the reason to `%TEMP%\qoqdir-crash.log` instead of quietly
reading the wrong memory.

## Test

```powershell
ctest --test-dir build --output-on-failure   # unit tests + self-test
build\unit_tests.exe                         # paths, command line, sort, filter, listing
build\qoqdir.exe --selftest                  # drives the real app; log: %TEMP%\qoqdir-selftest.log
```

The self-test opens a window without taking focus and never sends keystrokes to
other windows. It replaces the clipboard and leaves one small file in the Recycle
Bin.

Two scripts check what the self-test can't. Run them from the repository folder:

```powershell
powershell -File tests\cli-checks.ps1 -Exe build\qoqdir.exe     # command-line switches
powershell -File tests\watch-windows.ps1 -Exe build\qoqdir.exe  # popup watch
```

`cli-checks.ps1` runs a private copy of the exe in a temp folder, so your
`qoqdir.ini` and Bookmarks are never touched. `watch-windows.ps1` runs the
self-test and lists every window that appears meanwhile, from any program, so a
Windows error window gets noticed. It never touches those windows.

## Run from a terminal

```
qoqdir [switches] [folders...]

  {folder}     Open the folder in a tab (relative paths work: qoqdir .)
  -t{folder}   The same, as a switch: -tD:\logs  -t"C:\My Files"
  -h, -?       Show this help
```

Quote folders that contain spaces, but don't end them with `\` before the closing
quote, because Windows reads `\"` as a literal quote. Write `-t"D:\My Files"`, not
`-t"D:\My Files\"`.

Each folder opens once, and the first one is shown. With no folders, the home
folder opens. `D:` means `D:\`. Put `qoqdir.exe` in a folder on your PATH to type
just `qoqdir`.

```
qoqdir .
qoqdir -tD:\projects -tD:\logs
```

## Bookmarks

Tabs are not remembered between runs. For folders you use often, keep shortcuts in
the `Bookmarks` folder next to `qoqdir.exe`. It is created if missing and shown at
the top of the tree. Alt+drag a folder into it, or copy shortcuts in. Rename or
delete them like any file. Subfolders in it work too.

## Keyboard

Anywhere in the window:

| Keys | Action |
|---|---|
| Ctrl+T / Ctrl+W | new tab / close tab |
| Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1…9 | switch tab |
| Ctrl+L, Alt+D, F4 | go to the address bar |
| Ctrl+F, F3 | go to the filter box |
| F5, Ctrl+R | refresh |
| Ctrl+H | show/hide hidden files |
| Alt+Left / Alt+Right / Alt+Up | back / forward / up |
| Mouse back/forward buttons | back / forward |
| Tab, Shift+Tab | cycle focus: address, filter, tree, list |

In the file list:

| Keys | Action |
|---|---|
| Enter / Ctrl+Enter | open / open in a new background tab |
| Backspace | up |
| Delete / Shift+Delete | recycle / delete permanently |
| F2 | rename |
| Ctrl+Shift+N | new folder |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Alt+Enter | properties |
| Esc | clear the filter |
| any character | start filtering |

Middle-click a folder in the list or the tree to open it in a new background tab.
Drop files on a tab to copy them into that tab's folder.

## Files it writes

Everything goes next to `qoqdir.exe`, except the crash log:

- `qoqdir.ini` holds the window position and size, tree width, column widths, sort
  order and whether hidden files are shown. It never stores tabs or paths. If the
  folder is read-only, nothing is saved and nothing complains.
- `Bookmarks\` is your bookmarks folder.

Nothing goes in the registry or AppData, except `%TEMP%\qoqdir-crash.log`: written
only if the program stops on an internal bug, it records the reason.

## Code map

| File | What it does |
|---|---|
| `src/folder.cpp` | Paths, command line, folder reading, sorting, filtering (no UI; unit-tested) |
| `src/shell.cpp` | Windows shell: menus, copy/paste/delete/rename, icons, change watching, drag and drop |
| `src/filelist.cpp` | The virtual file list |
| `src/tree.cpp` | The folder tree |
| `src/app.cpp` | Main window: layout, tabs, navigation, keyboard, settings |
| `src/selftest.cpp` | The `--selftest` scripted session |
| `src/main.cpp` | Command line, help, message loop |
| `src/crashlog.cpp` | The crash log of spec 4.10 |
| `tests/tests.cpp` | Unit tests |
| `rust/` | The earlier Rust version (same features, same tests) |
| `tests/*.ps1` | Command-line checks and popup watch |

Design rules that keep bugs out:

- Windows does the file operations, with the same code Explorer uses.
- Folders load on a background thread. Only the newest request is shown, and a tab
  changes only after its folder has loaded.
- COM objects, menus and fonts free themselves (`common.h`).
- The file list shows rows from one index list that is only rebuilt together with
  the items, so a row can't point at the wrong file.
