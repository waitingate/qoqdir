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
qoqdir [switches] [folders...]

  {folder}     open the folder in a tab (qoqdir . works too)
  -t{folder}   the same: -tD:\logs  -t"C:\My Files"
  -h, -?       help
```

Each folder opens once, and the first one is shown. With no folders you get the
home folder. `D:` means `D:\`. Don't end a quoted folder with `\`: Windows reads
`\"` as a quote, so write `-t"D:\My Files"`. Put `qoqdir.exe` in a folder on your
PATH to type just `qoqdir`.

## Bookmarks

Keep shortcuts to the folders you use often in the `Bookmarks` folder next to
`qoqdir.exe`. It is created if missing and shown at the top of the tree. Alt+drag
a folder into it, or copy a folder and use Paste shortcut there. Rename or delete
bookmarks like any file.

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
| Ctrl+L, Alt+D, F4 | address bar |
| Ctrl+F, F3 | filter box (or just start typing in the list) |
| F5, Ctrl+R | refresh |
| Ctrl+H | show or hide hidden files |
| Alt+Left / Alt+Right / Alt+Up | back / forward / up |
| Enter / Ctrl+Enter | open / open in a background tab |
| Backspace | up |
| Delete / Shift+Delete | recycle / delete for good |
| F2 | rename |
| Ctrl+Shift+N | new folder |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Alt+Enter | properties |
| Esc | clear the filter |

Middle-click a folder to open it in a background tab. Drop files on a tab to copy
them into that tab's folder.

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
`gcc-13.2.0-mingw-w64ucrt-11.0.1-r8` from winlibs.com has all three. Put its `bin`
folder on the PATH (here it is in `C:\mingw64`):

```powershell
$env:PATH = 'C:\mingw64\bin;' + $env:PATH
cmake -S . -B build -G Ninja
cmake --build build
```

That gives `build\qoqdir.exe`, statically linked. GCC's runtime checks are on, so
a bad index or a signed integer overflow stops the program and writes the reason
to `%TEMP%\qoqdir-crash.log` instead of running on with wrong data.

## Test

```powershell
ctest --test-dir build --output-on-failure   # unit tests and the self-test
powershell -File tests\cli-checks.ps1 -Exe build\qoqdir.exe
powershell -File tests\watch-windows.ps1 -Exe build\qoqdir-test.exe
```

The build makes two exes from the same compiled app: `qoqdir.exe`, what users
get, and `qoqdir-test.exe`, the same plus the self-test. The self-test
(`qoqdir-test.exe --selftest`) drives the real app in a window that
doesn't take the focus, and never sends keys to other windows. It replaces the
clipboard and leaves one small file in the Recycle Bin. `cli-checks.ps1` runs a
copy of the exe in a temp folder, so your settings and Bookmarks stay untouched.
`watch-windows.ps1` runs the self-test and lists any window that pops up meanwhile.

## Release

Raise the version in `src/version.h` and commit, then run
`powershell -File release.ps1`. It builds that commit in a clean copy, runs the
tests (the self-test on `qoqdir-test.exe`, the rest on the exe that ships) and
writes `release\qoqdir-<version>.zip` with its SHA-256. Keep `release\qoqdir-<version>-debug.exe`: it turns the offsets in a
crash log from that version into source lines.

## Source

- `src/` the program
- `tests/` unit tests and the two check scripts
- `rust/` the same program in Rust (see its README)
- `release.ps1` makes a release
- `QOQDIR-SPEC.md` what Qoqdir must do and how it must behave
- `DEVELOPING.md` notes for working on the code

## License

MIT, see `LICENSE`. The exe also contains parts of MinGW-w64's runtime, under the
licenses in `THIRD-PARTY-NOTICES.txt`.
