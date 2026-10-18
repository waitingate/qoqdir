# Checks the exe that ships (spec 3): one file under 1 MB that imports only DLLs Windows protects (its KnownDLLs,
# comctl32 through the manifest, the CRT's API sets). Any other import, Windows would look for next to the exe
# first (docs\DEVELOPING.md). Needs MinGW's objdump on the PATH. Used by scripts\build.ps1 -Test, scripts\release.ps1
# and the build on GitHub, so the rule lives here once.
#
#   powershell -File tests\exe-checks.ps1 -Exe build\qoqdir.exe
param([Parameter(Mandatory)][string]$Exe)

$allowed = '^(KERNEL32|USER32|GDI32|ole32|OLEAUT32|SHELL32|SHLWAPI|COMCTL32|api-ms-win-crt-[a-z0-9-]+)\.dll$'
$size = (Get-Item $Exe).Length
$imports = @(objdump -p $Exe | Select-String 'DLL Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$strangers = @($imports | Where-Object { $_ -notmatch $allowed })
$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:failures++ }
}
Check ($size -lt 1MB) "the exe is one file under 1 MB ($size bytes)"
Check ($imports.Count -gt 0) "objdump lists its imports ($($imports.Count); none means MinGW is not on the PATH)"
Check ($strangers.Count -eq 0) "it imports only DLLs Windows protects$(if ($strangers) { ': not ' + ($strangers -join ', ') })"
if ($failures) { "$failures check(s) failed"; exit 1 } else { 'all checks passed' }
