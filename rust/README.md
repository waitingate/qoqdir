# Qoqdir (Rust version)

The same file manager as the C++ version one folder up, written in Rust with
Microsoft's official [`windows`](https://crates.io/crates/windows) crate.
Single portable exe (~490 KB), no runtime, no installer.

## Portable Rust setup (nothing installed on the system)

Everything lives in `C:\rust`. No PATH or registry changes; a terminal is set
up only when you ask it to be.

### One-time setup

```powershell
# 1. Download the official installer (GNU toolchain: needs no Visual Studio)
New-Item -ItemType Directory -Force C:\rust
Invoke-WebRequest https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe -OutFile C:\rust\rustup-init.exe

# 2. Install into C:\rust only
$env:RUSTUP_HOME = 'C:\rust\rustup'; $env:CARGO_HOME = 'C:\rust\cargo'
C:\rust\rustup-init.exe -y --no-modify-path --profile minimal --default-host x86_64-pc-windows-gnu --default-toolchain stable
```

3. Create `C:\rust\cargo\config.toml`. The `windows` crate needs `dlltool`, and
   the one bundled with Rust fails on this machine, so point it at the portable
   MinGW's:

   ```toml
   [target.x86_64-pc-windows-gnu]
   rustflags = ["-C", "dlltool=C:\\mingw64\\bin\\dlltool.exe"]
   ```

4. Create `C:\rust\rust-env.ps1` (and/or `rust-env.bat`) to set up a terminal:

   ```powershell
   $env:RUSTUP_HOME = Join-Path $PSScriptRoot 'rustup'
   $env:CARGO_HOME  = Join-Path $PSScriptRoot 'cargo'
   $env:PATH = (Join-Path $PSScriptRoot 'cargo\bin') + ';' + $env:PATH
   ```

### Every time you want to use Rust

```powershell
. C:\rust\rust-env.ps1        # PowerShell (note the dot and space)
call C:\rust\rust-env.bat     # cmd
```

### Update or remove

- Update Rust: `rustup update` (in a terminal set up as above).
- Remove Rust completely: delete `C:\rust`. Nothing else was touched.

## Run from a terminal

```
qoqdir [switches] [folders...]

  {folder}     Open the folder in a tab (relative paths work: qoqdir .)
  -t{folder}   The same, as a switch: -tD:\logs  -t"C:\My Files"
  -h, -?       Show this help
```

Quote folders that contain spaces, but don't end them with `\` before the closing
quote: Windows reads `\"` as a literal quote (write `-t"D:\My Files"`, not
`-t"D:\My Files\"`).

Each folder opens once; the first is shown. With no folders, the home folder opens.

Tabs are not remembered between runs. For folders you use often, keep shortcuts
in the `Bookmarks` folder next to `qoqdir.exe` (it is created if missing and shown
at the top of the tree): Alt+drag a folder into it, or copy shortcuts in. Rename or
delete them like any file. Examples:

```
qoqdir .
qoqdir -tD:\projects -tD:\logs
```

Put `qoqdir.exe` in a folder on your PATH to type just `qoqdir`.

## Build, test, run

```powershell
cargo build --release              # -> target\release\qoqdir.exe
cargo test                         # path/sort/filter/listing unit tests
target\release\qoqdir.exe --selftest  # drives the real app; log: %TEMP%\qoqdir-rs-selftest.log
```

The self-test opens a window without taking focus and never sends keystrokes
to other windows. It replaces the clipboard and leaves one small `file10.txt`
in the Recycle Bin.

If the app ever stops because of a bug, the reason is written to
`%TEMP%\qoqdir-crash.log`.

## Code map

| File | What it does |
|---|---|
| `folder.rs` | Paths, folder reading, sorting, filtering (no UI; unit-tested) |
| `shell.rs` | Windows shell: menus, copy/paste/delete/rename, icons, change watching, drag and drop |
| `filelist.rs` | The virtual file list |
| `tree.rs` | The folder tree |
| `app.rs` | Main window: layout, tabs, navigation, keyboard, settings |
| `selftest.rs` | `--selftest` scripted session |
| `util.rs` | UTF-16 strings and auto-freeing wrappers |

Design rules that keep bugs out:

- Windows does the file operations (the same code Explorer uses).
- Folders load on a background thread; only the newest request is shown, and a
  tab changes only after its folder loaded successfully.
- COM objects and Windows handles free themselves when dropped.
- "This PC" is its own `Location::ThisPc` case, not a magic empty path.
- Shared state is never borrowed across a call into Windows, which can call
  straight back into the app. A mistake here stops the app loudly
  (`panic = "abort"`) instead of corrupting data.
