# Makes a release from the last commit: builds it in a clean copy, runs the unit tests, the CLI checks on the
# exe that ships and the self-test on qoqdir-test.exe (the same compiled app plus the self-test), and leaves in
# release\ the zip to publish (its SHA-256 is printed) and qoqdir-<version>-debug.exe to keep: the shipped exe
# with its symbols, which turns crash-log offsets into source lines (see src/crashlog.cpp).
# Needs MinGW (with CMake and Ninja) on the PATH, or where the build guide puts it (find-mingw.ps1).
#     powershell -File scripts\release.ps1
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)   # the repository
. "$PSScriptRoot\find-mingw.ps1"
if (git status --porcelain) { throw 'Commit your changes first: a release is built from the last commit.' }
$header = Get-Content src\version.h -Raw
$version = [regex]::Match($header, 'QOQDIR_VERSION "([^"]+)"').Groups[1].Value
# The three places that hold the version must agree: the exe's Properties and the crash log come from version.h's
# two lines, the Rust version from Cargo.toml.
$numbers = [regex]::Match($header, 'QOQDIR_VERSION_NUMBERS (\d+), (\d+), (\d+)').Groups[1..3].Value -join '.'
$cargo = [regex]::Match((Get-Content rust\Cargo.toml -Raw), '(?m)^version = "([^"]+)"').Groups[1].Value
if ($numbers -ne $version -or $cargo -ne $version) {
    throw "The versions differ: version.h says $version and $numbers, rust\Cargo.toml says $cargo."
}
$out = Join-Path $PWD 'release'
$work = Join-Path $out 'work'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
function Check([string]$step) { if ($LASTEXITCODE) { throw "$step failed" } }

git clone --quiet . "$work\src"; Check 'git clone'
# Release's -O2, plus -g for the debug copy. Paths in it start at the work folder, and GCC doesn't record its
# command line, so the debug copy names no folder on this PC.
$flags = "-O2 -g -DNDEBUG -gno-record-gcc-switches -ffile-prefix-map=$($work -replace '\\', '/')=."
cmake -S "$work\src" -B "$work\build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$flags"
Check 'cmake'
cmake --build "$work\build"; Check 'build'
& "$work\build\unit_tests.exe"; Check 'unit tests'

$exe = "$work\qoqdir.exe"
strip -o $exe "$work\build\qoqdir.exe"; Check 'strip'
powershell -NoProfile -ExecutionPolicy Bypass -File "$work\src\tests\exe-checks.ps1" -Exe $exe; Check 'exe checks'
powershell -NoProfile -ExecutionPolicy Bypass -File "$work\src\tests\cli-checks.ps1" -Exe $exe; Check 'CLI checks'
powershell -NoProfile -ExecutionPolicy Bypass -File "$work\src\tests\watch-windows.ps1" -Exe "$work\build\qoqdir-test.exe"
Check 'self-test'

$zip = "$out\qoqdir-$version.zip"
$files = $exe, "$work\src\README.md", "$work\src\LICENSE", "$work\src\THIRD-PARTY-NOTICES.txt"
Compress-Archive -Force -DestinationPath $zip -Path $files
Copy-Item "$work\build\qoqdir.exe" "$out\qoqdir-$version-debug.exe" -Force
Remove-Item $work -Recurse -Force
"$zip"
"SHA-256: $((Get-FileHash $zip -Algorithm SHA256).Hash)"
