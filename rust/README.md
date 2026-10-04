# Qoqdir, Rust version

Qoqdir written in Rust with Microsoft's `windows` crate. Since 5 October 2026 it
has the same features as the C++ version, which is the main one, and the same
self-test steps.

## Portable Rust setup

Everything lives in `C:\rust`; nothing else on the system changes.

```powershell
New-Item -ItemType Directory -Force C:\rust
Invoke-WebRequest https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe -OutFile C:\rust\rustup-init.exe
$env:RUSTUP_HOME = 'C:\rust\rustup'; $env:CARGO_HOME = 'C:\rust\cargo'
C:\rust\rustup-init.exe -y --no-modify-path --profile minimal --default-host x86_64-pc-windows-gnu --default-toolchain stable
```

The `dlltool` that comes with Rust fails on this machine, so
`C:\rust\cargo\config.toml` points at MinGW's:

```toml
[target.x86_64-pc-windows-gnu]
rustflags = ["-C", "dlltool=C:\\mingw64\\bin\\dlltool.exe"]
```

`C:\rust\rust-env.ps1` sets up a terminal:

```powershell
$env:RUSTUP_HOME = Join-Path $PSScriptRoot 'rustup'
$env:CARGO_HOME  = Join-Path $PSScriptRoot 'cargo'
$env:PATH = (Join-Path $PSScriptRoot 'cargo\bin') + ';' + $env:PATH
```

Run `. C:\rust\rust-env.ps1` in each new terminal. `rustup update` updates Rust;
deleting `C:\rust` removes it.

## Build and test

```powershell
cargo build --release                  # target\release\qoqdir.exe
cargo test
target\release\qoqdir.exe --selftest   # log: %TEMP%\qoqdir-rs-selftest.log
```

The scripts in `..\tests` work with this exe too (`-Exe target\release\qoqdir.exe`).

## Code

- `folder.rs`: paths, folder reading, sorting, filtering (unit-tested)
- `shell.rs`: the Windows shell: menus, clipboard, delete, rename, icons, drag and drop
- `filelist.rs`, `tree.rs`: the file list and the folder tree
- `app.rs`: main window, tabs, navigation, keys, settings
- `selftest.rs`: the `--selftest` session
- `crashlog.rs`: writes `%TEMP%\qoqdir-crash.log` when the app stops on a bug
- `util.rs`: UTF-16 strings and owners that free Windows resources

Shared state is never borrowed across a call into Windows, because Windows can
call straight back into the app. A mistake there stops the app (`panic = "abort"`)
instead of corrupting data, and the crash log says where.
