# Puts MinGW-w64 (g++, with CMake and Ninja) on the PATH when it isn't there yet, from where the build guide puts it
# (no admin rights needed): %LOCALAPPDATA%\Programs\mingw64 or C:\mingw64. Dot-sourced by build.ps1 and release.ps1.
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $mingw = @("$env:LOCALAPPDATA\Programs\mingw64\bin", 'C:\mingw64\bin') | Where-Object { Test-Path "$_\g++.exe" } | Select-Object -First 1
    if (-not $mingw) { throw 'MinGW-w64 not found: see docs\BUILDING.md, section 1' }
    $env:PATH = "$mingw;" + $env:PATH
}
