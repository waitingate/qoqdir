# Puts the build tools on the PATH when they aren't there yet, from where the build guide puts them (no admin rights
# needed), and keeps this PC's folders out of the Rust exe. Dot-sourced by build.ps1 and release.ps1, along with what
# they both run the tests with.
# MinGW-w64 (g++, with CMake, Ninja and windres): %LOCALAPPDATA%\Programs\mingw64. Only that per-user folder is
# looked in; a folder at the root of C: is writable by other accounts on the PC, so tools kept there are trusted
# only when the owner has put them on the PATH (docs\BUILDING.md, section 1).
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $mingw = "$env:LOCALAPPDATA\Programs\mingw64\bin"
    if (-not (Test-Path "$mingw\g++.exe")) {
        throw 'MinGW-w64 not found: put it on the PATH, or in %LOCALAPPDATA%\Programs (docs\BUILDING.md, section 1)'
    }
    $env:PATH = "$mingw;" + $env:PATH
}
# Rust: %LOCALAPPDATA%\Programs\rust (same reasoning).
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $rustEnv = "$env:LOCALAPPDATA\Programs\rust\rust-env.ps1"
    if (-not (Test-Path $rustEnv)) {
        throw 'Rust not found: put cargo on the PATH, or install in %LOCALAPPDATA%\Programs (docs\BUILDING.md)'
    }
    . $rustEnv
}
# The panic messages of the crates used carry their source paths, which lie under CARGO_HOME: under C:\Users on a
# per-user install. Name that folder "cargo" in the exe instead, as CMake does for the compiler's folder. The flag goes
# in CARGO_ENCODED_RUSTFLAGS, which cargo takes whole: RUSTFLAGS would split a folder name with a space in it.
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { "$env:USERPROFILE\.cargo" }
if (-not ($env:RUSTFLAGS -or $env:CARGO_ENCODED_RUSTFLAGS)) {
    $env:CARGO_ENCODED_RUSTFLAGS = "--remap-path-prefix=$cargoHome=cargo"
}

function RunScript([string]$file) { powershell -NoProfile -ExecutionPolicy Bypass -File $file @args }
$selfTestLogs = "$env:TEMP\qoqdir-selftest.log", "$env:TEMP\qoqdir-rs-selftest.log"   # C++, Rust
