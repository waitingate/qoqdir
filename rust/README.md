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
C:\rust\rustup-init.exe -y --no-modify-path --profile minimal -c clippy --default-host x86_64-pc-windows-gnu --default-toolchain stable
```

If the `dlltool` that comes with Rust fails (it did on the first build machine),
point cargo at MinGW's in `C:\rust\cargo\config.toml`:

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
cargo build --release                  # target\release\qoqdir.exe, the exe you use
cargo test
cargo clippy --all-targets             # extra checks, in both builds:
cargo clippy --all-targets --features selftest   # fix what they report before a commit
cargo build --release --features selftest --target-dir target\selftest
target\selftest\release\qoqdir.exe --selftest    # log: %TEMP%\qoqdir-rs-selftest.log
```

The self-test is only in a build with the `selftest` feature, as in the C++
version: the exe you use doesn't carry it. That build gets its own folder, so it
never replaces your exe. The scripts in `..\tests` work with these exes too:
`cli-checks.ps1 -Exe target\release\qoqdir.exe`, and `watch-windows.ps1` with
`-Exe target\selftest\release\qoqdir.exe -Log $env:TEMP\qoqdir-rs-selftest.log`.

## Code

- `folder.rs`: paths, folder reading, sorting, filtering (unit-tested)
- `shell.rs`: the Windows shell: menus, clipboard, delete, rename, icons, drag and drop
- `filelist.rs`, `tree.rs`: the file list and the folder tree
- `app.rs`: main window, tabs, navigation, keys, settings
- `selftest.rs`: the `--selftest` session (only with the `selftest` feature)
- `crashlog.rs`: writes `%TEMP%\qoqdir-crash.log` when the app stops on a bug
- `util.rs`: UTF-16 strings and owners that free Windows resources

Shared state is never borrowed across a call into Windows, because Windows can
call straight back into the app. A mistake there stops the app (`panic = "abort"`)
instead of corrupting data, and the crash log says where.
