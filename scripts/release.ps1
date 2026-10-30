# Makes a release of both versions from the last commit. It builds them in a clean copy, runs the unit tests, the exe
# checks and the CLI checks on each exe that ships, and each self-test (C++: on qoqdir-test.exe, the same compiled app
# plus the self-test), whose two logs must match. It leaves in release\ the two zips to publish (their SHA-256s are
# printed) and a debug copy of each exe to keep: the shipped exe with its symbols, which turns crash-log offsets into
# source lines (see src/crashlog.cpp). Needs the build tools on the PATH, or where the build guide puts them
# (find-tools.ps1).
#     powershell -File scripts\release.ps1
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)   # the repository
# The C++ flags and RUSTFLAGS name the build folder (release\work) in lists that are split at spaces.
if ($PWD.Path.Contains(' ')) {
    throw "Move the repository to a path without spaces first: the build flags name a folder in it, split at spaces."
}
# A build setting left in the environment would change the shipped exe where no check can see it: a cargo profile
# override, rustc itself, or a compiler flag CMake reads. Refuse to build with any of them set.
foreach ($name in (Get-ChildItem Env:).Name) {
    if ($name -match '^(CARGO_PROFILE_|CARGO_BUILD_|CARGO_TARGET_\w+_(RUSTFLAGS|LINKER)$)' -or
        $name -in 'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'RUSTC', 'RUSTC_WRAPPER') {
        throw "Unset $name first: it would change the shipped exe, and no check would catch it."
    }
}
. "$PSScriptRoot\find-tools.ps1"
# Rust's flags stay, as the toolchain's setup needs them (docs\BUILDING.md): the one that keeps the cargo folder's path
# out of the exe, and the one that names dlltool. Any other would change the shipped exe in the same way.
$sep = [char]0x1F
$given = if ($env:CARGO_ENCODED_RUSTFLAGS) { $env:CARGO_ENCODED_RUSTFLAGS -split $sep } else { -split $env:RUSTFLAGS }
foreach ($flag in $given) {
    if ($flag -and $flag -notmatch '^(--remap-path-prefix=|-Cdlltool=)') {
        throw "Take $flag out of RUSTFLAGS or CARGO_ENCODED_RUSTFLAGS first: it would change the shipped exe."
    }
}
if (git status --porcelain) { throw 'Commit your changes first: a release is built from the last commit.' }
$header = Get-Content src\version.h -Raw
$version = [regex]::Match($header, 'QOQDIR_VERSION "([^"]+)"').Groups[1].Value
# The three places that hold the version must agree: both exes' Properties come from version.h's two lines, as do
# the C++ help box and crash log; the Rust ones come from Cargo.toml.
$numbers = [regex]::Match($header, 'QOQDIR_VERSION_NUMBERS (\d+), (\d+), (\d+)').Groups[1..3].Value -join '.'
$cargo = [regex]::Match((Get-Content rust\Cargo.toml -Raw), '(?m)^version = "([^"]+)"').Groups[1].Value
if ($numbers -ne $version -or $cargo -ne $version) {
    throw "The versions differ: version.h says $version and $numbers, rust\Cargo.toml says $cargo."
}
$out = Join-Path $PWD 'release'
# Don't overwrite a version that was already built: raise the version first (a released one is tagged, or its files
# are still in release\). Rebuilding the same number would make a different exe carry it and replace the debug copy.
$zip = Join-Path $out "qoqdir-$version.zip"
$tag = git tag --list "v$version"
if ((Test-Path $zip) -or ($tag -and (git rev-parse "v$version^{commit}") -ne (git rev-parse HEAD))) {
    throw "Version $version already built: raise it first (it is in release\, or tagged on another commit)."
}
$work = Join-Path $out 'work'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
$tests = "$work\src\tests"
$logs = "$env:TEMP\qoqdir-selftest.log", "$env:TEMP\qoqdir-rs-selftest.log"
function Check([string]$step) { if ($LASTEXITCODE) { throw "$step failed" } }
# The checks on an exe that ships, then the self-test of the build that has it.
function CheckExe([string]$name, [string]$exe, [string]$selftest, [string]$log) {
    powershell -NoProfile -ExecutionPolicy Bypass -File "$tests\exe-checks.ps1" -Exe $exe; Check "exe checks, $name"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$tests\cli-checks.ps1" -Exe $exe; Check "CLI checks, $name"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$tests\watch-windows.ps1" -Exe $selftest -Log $log
    Check "self-test, $name"
}

git clone --quiet . "$work\src"; Check 'git clone'
# C++: Release's -O2, plus -g for the debug copy. Paths in it start at the work folder, and GCC doesn't record its
# command line, so the debug copy names no folder on this PC.
$flags = "-O2 -g -DNDEBUG -gno-record-gcc-switches -ffile-prefix-map=$($work -replace '\\', '/')=."
cmake -S "$work\src" -B "$work\build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$flags"
Check 'cmake'
cmake --build "$work\build"; Check 'build'
& "$work\build\unit_tests.exe"; Check 'unit tests'
$exe = "$work\qoqdir.exe"
strip -o $exe "$work\build\qoqdir.exe"; Check 'strip'
CheckExe 'C++' $exe "$work\build\qoqdir-test.exe" $logs[0]

# Rust: the crates in Cargo.lock, and the release profile, but with line tables and symbols for the debug copy, whose
# paths start at the work folder too. The flags go in CARGO_ENCODED_RUSTFLAGS, one per 0x1F, which cargo takes
# whole: RUSTFLAGS would split a folder name with a space in it.
$remap = "--remap-path-prefix=$work=."
$given = @($given | Where-Object { $_ -and $_ -ne $remap }) + $remap   # once, if run twice in one shell
$env:CARGO_ENCODED_RUSTFLAGS = $given -join $sep
$rust = '--manifest-path', "$work\src\rust\Cargo.toml", '--locked'
$symbols = '--config', 'profile.release.debug=1', '--config', 'profile.release.strip=false'
cargo build --release @rust @symbols --target-dir "$work\rust"; Check 'Rust build'
$rsExe = "$work\rust\qoqdir.exe"
strip -o $rsExe "$work\rust\release\qoqdir.exe"; Check 'strip'
$rsDebug = "$work\rust\qoqdir-debug.exe"
Copy-Item "$work\rust\release\qoqdir.exe" $rsDebug   # before the self-test build takes its place
cargo test @rust --target-dir "$work\rust"; Check 'Rust unit tests'
# The self-test build in the same folder and with the same flags: only qoqdir itself is compiled again.
cargo build --release @rust @symbols --features selftest --target-dir "$work\rust"
Check 'Rust build with the self-test'
CheckExe 'Rust' $rsExe "$work\rust\release\qoqdir.exe" $logs[1]
powershell -NoProfile -ExecutionPolicy Bypass -File "$tests\compare-selftests.ps1" -CppLog $logs[0] -RustLog $logs[1]
Check 'the same self-test steps in both'

# Each zip holds the exe, README, LICENSE and the notices for what else is in that exe. The Rust exe holds Rust's
# standard library, whose notices come with the Rust that built it.
$std = "$work\rust\rust-std-notices.html"
Copy-Item "$(rustc --print sysroot)\share\doc\rust\COPYRIGHT-library.html" $std
$zips = "$out\qoqdir-$version.zip", "$out\qoqdir-rust-$version.zip"
$docs = "$work\src\README.md", "$work\src\LICENSE"
$cppFiles = $docs + $exe + "$work\src\THIRD-PARTY-NOTICES.txt"
$rsFiles = $docs + $rsExe + "$work\src\rust\THIRD-PARTY-NOTICES.txt" + $std
Compress-Archive -Force -DestinationPath $zips[0] -Path $cppFiles
Compress-Archive -Force -DestinationPath $zips[1] -Path $rsFiles
Copy-Item "$work\build\qoqdir.exe" "$out\qoqdir-$version-debug.exe" -Force
Copy-Item $rsDebug "$out\qoqdir-rust-$version-debug.exe" -Force
Remove-Item $work -Recurse -Force
foreach ($zip in $zips) { "$zip"; "SHA-256: $((Get-FileHash $zip -Algorithm SHA256).Hash)" }
