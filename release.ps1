# Makes a release from the last commit: builds it in a clean copy, runs the unit tests, the CLI checks on the
# exe that ships and the self-test on qoqdir-test.exe (the same compiled app plus the self-test), and leaves in
# release\ the zip to publish (its SHA-256 is printed) and qoqdir-<version>-debug.exe to keep: the shipped exe
# with its symbols, which turns crash-log offsets into source lines (see src/crashlog.cpp).
# Needs MinGW (with CMake and Ninja) on the PATH.      powershell -File release.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (git status --porcelain) { throw 'Commit your changes first: a release is built from the last commit.' }
$version = [regex]::Match((Get-Content src\version.h -Raw), 'QOQDIR_VERSION "([^"]+)"').Groups[1].Value
$out = Join-Path $PSScriptRoot 'release'
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
