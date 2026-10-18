# Qoqdir

A small, fast file explorer for Windows 10/11 with tabs, a folder tree and a
detail list. It is meant to be started from Windows Terminal. One portable exe,
written in C++ and plain Win32: no runtime, no installer, and no network code (no
updates, no telemetry).

![The Qoqdir window: tabs, a folder tree with bookmarks, and the file list](docs/screenshot.png)

## Download

Get `qoqdir-<version>.zip` from the [Releases](../../releases) page and unzip it
into a folder you can write to: settings and Bookmarks are saved next to the exe,
so not in Program Files. Then run `qoqdir.exe`. It needs Windows 10 or 11, x64.

The exe isn't signed, so the first start may show "Windows protected your PC":
choose More info, then Run anyway. Each release lists the zip's SHA-256, which
`Get-FileHash qoqdir-<version>.zip` should match.

## Command line

```
qoqdir [options] [folder ...]

  {folder}           open the folder in a tab (qoqdir . works too)
  --tab="{folder}"   the same: --tab="D:\logs"
  -t="{folder}"      short for --tab=: -t="D:\logs" -t="C:\My Files"
  -h, --help, -?     print the help: options, examples, notes for scripts
```

Each folder opens once, and the first one is shown. With no folders you get the
home folder. `D:` means `D:\`. A folder that can't be opened yet, like a network
drive that isn't connected, still gets its tab. All after `-t=` or `--tab=` is
the folder, even a name that starts with `-`. Quote the folder after `-t=`:
PowerShell reads an unquoted `-t=` as a switch of its own and splits
`-t=my.folder` at the dot. `--tab=` works unquoted in cmd, PowerShell and Git
Bash (Git Bash leaves `~` as it is, so write `"$HOME"`). Don't end a quoted
folder with `\`: cmd and Windows PowerShell 5.1 read `\"` as a quote. Put
`qoqdir.exe` in a folder on your PATH to type just `qoqdir`.

## Getting around

The tree on the left starts with Bookmarks, then your Desktop, Downloads,
Documents, Pictures, Music and Videos, then This PC with the drives (This PC
shows each drive's free space in the Type column). Click a folder to open it in
the current tab, middle-click it for a new background tab, or right-click it for
Windows' own menu.

The address bar (Ctrl+L) takes more than a path:

- A folder path opens here. `D:` means `D:\`, `%USERPROFILE%` and forward
  slashes work, and so do `..` and the name of a subfolder of the current folder.
- A file path opens the file as a double-click would; the tab stays where it is.
- Anything else runs as a command, the way the Run box (Win+R) runs it, in the
  current folder: `cmd` opens a terminal there, `notepad notes.txt` opens that
  file, a web address opens the browser. `%VARIABLES%` are expanded. A program
  named without a folder (`cmd`, `code`) is the one on your PATH, never a file
  of that name in the current folder.

A path that doesn't work stays selected in the box so it can be fixed, and Esc
puts the current path back.

To filter the list, just start typing: the text goes into the filter box, and
the list shows the names that contain it, ignoring case. With `*` or `?` it is
a pattern, and `;` separates several: `*.jpg; *.png`. Esc shows all again.
Going to another folder clears the filter; refreshing keeps it.

Folders come before files, numbers in names sort naturally (`file2` before
`file10`), and a click on a column header sorts by it. Opening a shortcut
(`.lnk`) to a folder opens that folder in the tab; other shortcuts, `.url` links
and files are opened by Windows.

## Bookmarks

Keep shortcuts to the folders you use often in the `Bookmarks` folder next to
`qoqdir.exe`. It is created if missing and shown at the top of the tree. Right-click
a tab and choose Bookmark this folder, Alt+drag a folder into it, or copy a folder
and use Paste shortcut there. Rename or delete bookmarks like any file. Subfolders
group them, and open in the tree too. On a
flash drive they keep working when the drive gets another letter: a shortcut
made on the same drive also remembers the way from itself to its target.

Tabs aren't saved by themselves. Ctrl+S (or "Save tabs" on a tab's right-click
menu) saves them as shortcuts in `Bookmarks\Tabs`, replacing the ones there, and
Ctrl+O ("Open saved tabs") opens them again after your open tabs. To keep a set,
rename that folder. Closing a window with four or more tabs open asks whether to
save them first.

## Keys

| Keys | Action |
|---|---|
| Ctrl+T / Ctrl+W | new tab / close tab |
| Ctrl+S / Ctrl+O | save the tabs / open the saved tabs |
| Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+1 to 9 | switch tab |
| Ctrl+Shift+PgUp / PgDn | move the tab left / right |
| Ctrl+L, Alt+D, F4 | address bar |
| Ctrl+F, F3 | filter box (or just start typing in the list) |
| F5, Ctrl+R | refresh the list and the tree (or the Refresh button) |
| Ctrl+H | show or hide hidden files |
| Alt+Left / Alt+Right / Alt+Up | back / forward / up |
| Tab, Shift+Tab | move between the address bar, filter, tree and list |
| Enter / Ctrl+Enter | open / open in a background tab |
| Backspace | up |
| Delete / Shift+Delete | recycle / delete for good |
| F2 | rename |
| Ctrl+Shift+N | new folder |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Alt+Enter | properties (of the folder when nothing is selected) |
| Esc | clear the filter |
| Ctrl+Backspace | delete the word before the cursor (address bar, filter) |
| F1 | the main keys |

The mouse's back and forward buttons go back and forward. Middle-click a folder
to open it in a background tab, or double-click the empty part of the tab bar
for a new tab. New tabs always go at the end. Drag a tab along the bar to move
it, and turn the mouse wheel over the tabs to switch between them. A tab's
right-click menu also has Duplicate tab and Close other tabs. Drop files on a
tab, or on a folder or bookmark in the tree, to move or copy them there, as
Explorer would; a closed folder in the tree opens when you hold the drag over
it. (The tree takes drops on local drives only.)

Right-click on files gives Windows' full context menu, so Send to, Open with and
the entries of other programs are all there. For what Qoqdir doesn't do,
thumbnails and searching inside subfolders, right-click the empty space: Open in
Explorer and Search in Explorer open that folder there.

Qoqdir has no undo of its own: Windows keeps one undo list for all programs. To
take back a rename, move, copy or delete done in Qoqdir, click the desktop (or an
Explorer window) and press Ctrl+Z. A Shift+Delete can't be undone.

## Files it writes

Next to `qoqdir.exe`: `qoqdir.ini` (window size, tree and column widths, sort
order, hidden files on or off; never tabs or paths) and the `Bookmarks` folder.
Nothing goes in the registry or AppData. The only other file is
`%TEMP%\qoqdir-crash.log`, written if the program stops on a bug or hangs.

## Bugs

If Qoqdir stops on a bug, the last line of `%TEMP%\qoqdir-crash.log` says what
happened. A window that answers nothing for 5 seconds gets a line there too,
saying where it was stuck (often inside a shell extension of another program),
and then goes on. Please open an issue with that line (it holds the Qoqdir
version) and your Windows version.

## Build

Needs MinGW-w64 GCC 13 with CMake and Ninja, on Windows 10 or 11 x64. No
Visual Studio and nothing to install: the winlibs build the releases are made
with has all three in one archive.

1. Download `winlibs-x86_64-posix-seh-gcc-13.2.0-mingw-w64ucrt-11.0.1-r8.zip`
   (216 MB; the `.7z` is 94 MB). On https://winlibs.com/ it is under "GCC
   13.2.0 (with POSIX threads) + LLVM/Clang/LLD/LLDB 18.1.5 + MinGW-w64 11.0.1
   (UCRT) - release 8": take the Win64 link marked without LLVM/Clang/LLD/LLDB.
   The links there go to the same files on GitHub:
   https://github.com/brechtsanders/winlibs_mingw/releases/tag/13.2.0posix-18.1.5-11.0.1-ucrt-r8
2. Unpack it. The archive holds one folder, `mingw64`: put it in your own
   programs folder, `%LOCALAPPDATA%\Programs` (that is
   `C:\Users\<you>\AppData\Local\Programs`, where per-user programs go), so
   that `%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe` exists. No admin rights
   are needed. Any other folder works too, such as `C:\mingw64` (use that if
   your user name has a space in it: MinGW's tools dislike spaces in paths);
   then write its `bin` in the first line below instead. To remove the
   compiler later, delete the folder.
3. In PowerShell, in this folder:

```powershell
$env:PATH = "$env:LOCALAPPDATA\Programs\mingw64\bin;" + $env:PATH
cmake -S . -B build -G Ninja
cmake --build build
```

That gives `build\qoqdir.exe`, statically linked. `docs/BUILDING.md` covers the
rest: setting up the tools, the Rust version, the tests, `scripts\build.ps1`
(which builds and checks both versions) and making a release. GitHub Actions
builds the C++ version on every push and runs its tests
(`.github/workflows/build.yml`).

## Source

- `src/` the program, `res/` its manifest and icon
- `rust/` the same program in Rust
- `tests/` the unit tests and the two check scripts
- `scripts/` `build.ps1` builds and checks both versions, `release.ps1` makes a
  release, `make-icon.ps1` draws the icon
- `docs/` `BUILDING.md` (tools, building, testing, releases), `DEVELOPING.md`
  (notes for working on the code) and `SPEC.md` (what Qoqdir must do and
  how it must behave)

## License

MIT, see `LICENSE`. The exe also contains parts of MinGW-w64's runtime, under the
licenses in `THIRD-PARTY-NOTICES.txt`.
