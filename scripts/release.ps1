# Makes a release from the last commit: builds it in a clean copy, runs the unit tests, the CLI checks on the
# exe that ships and the self-test on qoqdir-test.exe (the same compiled app plus the self-test), and leaves in
# release\ the zip to publish (its SHA-256 is printed) and qoqdir-<version>-debug.exe to keep: the shipped exe
# with its symbols, which turns crash-log offsets into source lines (see src/crashlog.cpp).
# Needs MinGW (with CMake and Ninja) on the PATH, or where the build guide puts it: %LOCALAPPDATA%\Programs\mingw64
# or C:\mingw64.
#     powershell -File scripts\release.ps1
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)   # the repository
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $mingw = @("$env:LOCALAPPDATA\Programs\mingw64\bin", 'C:\mingw64\bin') | Where-Object { Test-Path "$_\g++.exe" } | Select-Object -First 1
    if (-not $mingw) { throw 'MinGW-w64 not found: see docs\BUILDING.md, section 1' }
    $env:PATH = "$mingw;" + $env:PATH
}
if (git status --porcelain) { throw 'Commit your changes first: a release is built from the last commit.' }
$version = [regex]::Match((Get-Content src\version.h -Raw), 'QOQDIR_VERSION "([^"]+)"').Groups[1].Value
# The three places that hold the version must agree: the exe's Properties and the crash log come from version.h's
# two lines, the Rust version from Cargo.toml.
$numbers = [regex]::Match((Get-Content src\version.h -Raw), 'QOQDIR_VERSION_NUMBERS (\d+), (\d+), (\d+)').Groups[1..3].Value -join '.'
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
# One exe under 1 MB that imports only DLLs Windows protects (spec 3): no MinGW runtime, nothing Windows would look
# for next to the exe first.
$size = (Get-Item $exe).Length
if ($size -ge 1MB) { throw "qoqdir.exe is $size bytes: 1 MB or more" }
$allowed = '^(KERNEL32|USER32|GDI32|ole32|OLEAUT32|SHELL32|SHLWAPI|COMCTL32|api-ms-win-crt-[a-z0-9-]+)\.dll$'
$imports = @(objdump -p $exe | Select-String 'DLL Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$strangers = @($imports | Where-Object { $_ -notmatch $allowed })
if ($strangers) { throw "qoqdir.exe imports DLLs Windows doesn't protect: $($strangers -join ', ')" }
"$size bytes, imports: $($imports -join ', ')"
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
