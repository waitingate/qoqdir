# Builds both versions of Qoqdir; with -Test it then runs every check, as is due before calling a change done
# (docs\BUILDING.md) and as the build on GitHub does. The tools come from the PATH, or else from where the guide puts
# them (find-tools.ps1).
#   powershell -File scripts\build.ps1          C++: build\qoqdir.exe, build\qoqdir-test.exe; Rust: rust\target\
#   powershell -File scripts\build.ps1 -Test    then every check (docs\BUILDING.md, section 3)
param([switch]$Test)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)   # the repository
. "$PSScriptRoot\find-tools.ps1"
function Step([string]$name, [scriptblock]$run) {
    "== $name"
    & $run
    if ($LASTEXITCODE) { throw "$name failed" }
}
$rust = '--manifest-path', 'rust\Cargo.toml', '--locked'   # build the crates in Cargo.lock, don't rewrite it
$selftest = '--features', 'selftest', '--target-dir', 'rust\target\selftest'   # its own folder: never replaces your exe

Step 'C++ build' {   # any warning fails it (-Werror in CMakeLists.txt)
    cmake -S . -B build -G Ninja > $null
    if (-not $LASTEXITCODE) { cmake --build build }
}
Step 'Rust build' { cargo build --release @rust }
Step 'Rust build with the self-test' { cargo build --release @rust @selftest }
if (-not $Test) { return }

Step 'C++ unit tests' { build\unit_tests.exe }
Step 'exe checks, C++' { RunScript tests\exe-checks.ps1 -Exe build\qoqdir.exe }
Step 'Rust unit tests' { cargo test @rust }
Step 'exe checks, Rust' { RunScript tests\exe-checks.ps1 -Exe rust\target\release\qoqdir.exe }
Step 'clippy' { cargo clippy --all-targets @rust '--' -D warnings }
Step 'clippy with the self-test' { cargo clippy --all-targets @rust @selftest '--' -D warnings }
Step 'command-line checks, C++' { RunScript tests\cli-checks.ps1 -Exe build\qoqdir.exe }
Step 'command-line checks, Rust' { RunScript tests\cli-checks.ps1 -Exe rust\target\release\qoqdir.exe }
# One self-test at a time, as they share the clipboard. Then both versions must have taken the same steps with the
# same results (the notes and timings at the line ends aside).
Step 'self-test, C++' { RunScript tests\watch-windows.ps1 -Exe build\qoqdir-test.exe -Log $selfTestLogs[0] }
Step 'self-test, Rust' {
    RunScript tests\watch-windows.ps1 -Exe rust\target\selftest\release\qoqdir.exe -Log $selfTestLogs[1]
}
Step 'the same self-test steps in both' {
    RunScript tests\compare-selftests.ps1 -CppLog $selfTestLogs[0] -RustLog $selfTestLogs[1]
}
'all checks passed'
