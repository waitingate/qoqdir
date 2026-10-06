# Qoqdir, Rust version

Qoqdir written in Rust with Microsoft's `windows` crate. Since 5 October 2026 it
has the same features as the C++ version, which is the main one, and the same
self-test steps.

From this folder, with Rust and `C:\mingw64\bin` (for the icon) on the PATH:

```powershell
cargo build --release                                                    # target\release\qoqdir.exe
cargo build --release --features selftest --target-dir target\selftest   # the same with the self-test
cargo test
```

`../docs/BUILDING.md` says how to set up Rust and how to test this version along
with the C++ one; `../docs/DEVELOPING.md` describes the code.
