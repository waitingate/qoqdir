# Qoqdir

A small, fast file explorer for Windows 10/11 with tabs, a folder tree and a
detail list. It is meant to be started from Windows Terminal. One portable exe,
written in C++ and plain Win32: no runtime, no installer, and no network code (no
updates, no telemetry).

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
home folder. `D:` means `D:\`. All after `-t=` or `--tab=` is the folder, even
a name that starts with `-`. Quote the folder after `-t=`: PowerShell reads an
unquoted `-t=` as a switch of its own and splits `-t=my.folder` at the dot.
`--tab=` works unquoted in cmd, PowerShell and Git Bash (Git Bash leaves `~` as
it is, so write `"$HOME"`). Don't end a quoted folder with `\`: cmd and Windows
PowerShell 5.1 read `\"` as a quote. Put `qoqdir.exe` in a folder on your PATH
to type just `qoqdir`.

## Bookmarks

Keep shortcuts to the folders you use often in the `Bookmarks` folder next to
`qoqdir.exe`. It is created if missing and shown at the top of the tree. Alt+drag
a folder into it, or copy a folder and use Paste shortcut there. Rename or delete
bookmarks like any file. Subfolders group them, and open in the tree too.

Tabs aren't saved by themselves. Ctrl+S (or "Save tabs" on a tab's right-click
menu) saves them as shortcuts in `Bookmarks\Tabs`, replacing the ones there, and
Ctrl+O ("Open saved tabs") opens them again after your open tabs. To keep a set,
rename that folder.

## Keys

| Keys | Action |
|---|---|
| Ctrl+T / Ctrl+W | new tab / close tab |
| Ctrl+S / Ctrl+O | save the tabs / open the saved tabs |
| Ctrl+Tab, Ctrl+1 to 9 | switch tab |
| Ctrl+Shift+PgUp / PgDn | move the tab left / right |
| Ctrl+L, Alt+D, F4 | address bar |
| Ctrl+F, F3 | filter box (or just start typing in the list) |
| F5, Ctrl+R | refresh the list and the tree (or the Refresh button) |
| Ctrl+H | show or hide hidden files |
| Alt+Left / Alt+Right / Alt+Up | back / forward / up |
| Enter / Ctrl+Enter | open / open in a background tab |
| Backspace | up |
| Delete / Shift+Delete | recycle / delete for good |
| F2 | rename |
| Ctrl+Shift+N | new folder |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Alt+Enter | properties (of the folder when nothing is selected) |
| Esc | clear the filter |
| F1 | the main keys |

Middle-click a folder to open it in a background tab. New tabs always go at the
end. Drag a tab along the bar to move it, and turn the mouse wheel over the tabs
to switch between them. Drop files on a tab, or on a folder or bookmark in the
tree, to move or copy them there, as Explorer would; a closed folder in the tree
opens when you hold the drag over it. (The tree takes drops on local drives only.)

Qoqdir has no undo of its own: Windows keeps one undo list for all programs. To
take back a rename, move, copy or delete done in Qoqdir, click the desktop (or an
Explorer window) and press Ctrl+Z. A Shift+Delete can't be undone.

## Files it writes

Next to `qoqdir.exe`: `qoqdir.ini` (window size, tree and column widths, sort
order, hidden files on or off; never tabs or paths) and the `Bookmarks` folder.
Nothing goes in the registry or AppData. The only other file is
`%TEMP%\qoqdir-crash.log`, written if the program stops on a bug.

## Bugs

If Qoqdir stops on a bug, the last line of `%TEMP%\qoqdir-crash.log` says what
happened. Please open an issue with that line (it holds the Qoqdir version) and
your Windows version.

## Build

Needs MinGW-w64 GCC 13 with CMake and Ninja; the winlibs UCRT build
`gcc-13.2.0-mingw-w64ucrt-11.0.1-r8` from winlibs.com has all three. With its
`bin` folder on the PATH (here it is in `C:\mingw64`):

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH
cmake -S . -B build -G Ninja
cmake --build build
```

That gives `build\qoqdir.exe`, statically linked. `docs/BUILDING.md` covers the
rest: setting up the tools, the Rust version, the tests, `scripts\build.ps1`
(which builds and checks both versions) and making a release.

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
