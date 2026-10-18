# Builds both versions of Qoqdir; with -Test it then runs every check, as is due before calling a change done
# (docs\BUILDING.md). The tools come from the PATH, or else from where the guide puts them (no admin rights
# needed): MinGW in %LOCALAPPDATA%\Programs\mingw64 or C:\mingw64, Rust in %LOCALAPPDATA%\Programs\rust or C:\rust.
#   powershell -File scripts\build.ps1          C++: build\qoqdir.exe, build\qoqdir-test.exe; Rust: rust\target\
#   powershell -File scripts\build.ps1 -Test    then the unit tests, clippy, the command-line checks and both self-tests
param([switch]$Test)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)   # the repository
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $mingw = @("$env:LOCALAPPDATA\Programs\mingw64\bin", 'C:\mingw64\bin') | Where-Object { Test-Path "$_\g++.exe" } | Select-Object -First 1
    if (-not $mingw) { throw 'MinGW-w64 not found: see docs\BUILDING.md, section 1' }
    $env:PATH = "$mingw;" + $env:PATH
}
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $rustEnv = @("$env:LOCALAPPDATA\Programs\rust\rust-env.ps1", 'C:\rust\rust-env.ps1') | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $rustEnv) { throw 'Rust not found: see docs\BUILDING.md, section 1' }
    . $rustEnv
}
# The panic messages of the crates used carry their source paths, which lie under CARGO_HOME: under C:\Users on a
# per-user install. Name that folder "cargo" in the exe instead, as CMake does for the compiler's folder.
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { "$env:USERPROFILE\.cargo" }
if (-not $env:RUSTFLAGS) { $env:RUSTFLAGS = "--remap-path-prefix=$cargoHome=cargo" }
function Step([string]$name, [scriptblock]$run) {
    "== $name"
    & $run
    if ($LASTEXITCODE) { throw "$name failed" }
}
$rust = '--manifest-path', 'rust\Cargo.toml'
$selftest = '--features', 'selftest', '--target-dir', 'rust\target\selftest'   # its own folder: never replaces your exe

Step 'C++ build' {
    cmake -S . -B build -G Ninja > $null
    if (-not $LASTEXITCODE) {
        $out = cmake --build build
        $out
        if ($out -match 'warning:') { throw 'The C++ build has warnings.' }
    }
}
Step 'Rust build' { cargo build --release @rust }
Step 'Rust build with the self-test' { cargo build --release @rust @selftest }
if (-not $Test) { return }

Step 'C++ unit tests' { build\unit_tests.exe }
Step 'exe checks' { powershell -NoProfile -ExecutionPolicy Bypass -File tests\exe-checks.ps1 -Exe build\qoqdir.exe }
Step 'Rust unit tests' { cargo test @rust }
Step 'clippy' { cargo clippy --all-targets @rust '--' -D warnings }
Step 'clippy with the self-test' { cargo clippy --all-targets @rust @selftest '--' -D warnings }
Step 'command-line checks, C++' { powershell -NoProfile -ExecutionPolicy Bypass -File tests\cli-checks.ps1 -Exe build\qoqdir.exe }
Step 'command-line checks, Rust' {
    powershell -NoProfile -ExecutionPolicy Bypass -File tests\cli-checks.ps1 -Exe rust\target\release\qoqdir.exe
}
# One self-test at a time, as they share the clipboard. Then both versions must have taken the same steps with the
# same results (the notes and timings at the line ends aside).
$logs = "$env:TEMP\qoqdir-selftest.log", "$env:TEMP\qoqdir-rs-selftest.log"
Step 'self-test, C++' {
    powershell -NoProfile -ExecutionPolicy Bypass -File tests\watch-windows.ps1 -Exe build\qoqdir-test.exe -Log $logs[0]
}
Step 'self-test, Rust' {
    powershell -NoProfile -ExecutionPolicy Bypass -File tests\watch-windows.ps1 -Exe rust\target\selftest\release\qoqdir.exe -Log $logs[1]
}
$cpp, $rs = $logs | ForEach-Object { ((Get-Content $_ -Encoding UTF8) -replace '   \(slow step: \d+ ms\)$' -replace '   \[.*\]$') -join "`n" }
if ($cpp -ne $rs) { throw 'The two versions did not take the same self-test steps.' }
'all checks passed'
