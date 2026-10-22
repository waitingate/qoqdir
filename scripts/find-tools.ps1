# Puts the build tools on the PATH when they aren't there yet, from where the build guide puts them (no admin rights
# needed), and keeps this PC's folders out of the Rust exe. Dot-sourced by build.ps1 and release.ps1.
# MinGW-w64 (g++, with CMake, Ninja and windres): %LOCALAPPDATA%\Programs\mingw64 or C:\mingw64.
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $mingw = @("$env:LOCALAPPDATA\Programs\mingw64\bin", 'C:\mingw64\bin') |
        Where-Object { Test-Path "$_\g++.exe" } | Select-Object -First 1
    if (-not $mingw) { throw 'MinGW-w64 not found: see docs\BUILDING.md, section 1' }
    $env:PATH = "$mingw;" + $env:PATH
}
# Rust: %LOCALAPPDATA%\Programs\rust or C:\rust.
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $rustEnv = @("$env:LOCALAPPDATA\Programs\rust\rust-env.ps1", 'C:\rust\rust-env.ps1') |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $rustEnv) { throw 'Rust not found: see docs\BUILDING.md, section 1' }
    . $rustEnv
}
# The panic messages of the crates used carry their source paths, which lie under CARGO_HOME: under C:\Users on a
# per-user install. Name that folder "cargo" in the exe instead, as CMake does for the compiler's folder.
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { "$env:USERPROFILE\.cargo" }
if (-not $env:RUSTFLAGS) { $env:RUSTFLAGS = "--remap-path-prefix=$cargoHome=cargo" }
