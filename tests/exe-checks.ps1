# Checks an exe that ships (spec 3): one file under 1 MB, no path in it that names the user's folder, and no DLL it
# imports can be planted beside it. Windows protects its KnownDLLs, comctl32 through the manifest, ntdll (always
# loaded first) and the API sets (api-ms-win-*, which it resolves itself); any other import it might look for next to
# the exe first (docs\DEVELOPING.md). So for each other one, a stand-in of that name goes beside a copy of the exe,
# which must still start: Windows then used its own. Needs MinGW's objdump on the PATH. Used by scripts\build.ps1
# -Test, scripts\release.ps1 and the build on GitHub, so the rules live here once.
#
#   powershell -File tests\exe-checks.ps1 -Exe build\qoqdir.exe
#   powershell -File tests\exe-checks.ps1 -Exe rust\target\release\qoqdir.exe
param([Parameter(Mandatory)][string]$Exe)
# A missing exe or an unreadable one must fail the check, not let the size and user-path checks pass on a null.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\check.ps1"

$protected ='^(KERNEL32|USER32|GDI32|ole32|OLEAUT32|SHELL32|SHLWAPI|COMCTL32|combase|ntdll|api-ms-win-[a-z0-9-]+)\.dll$'
$file = Get-Item $Exe
$size = $file.Length
$imports = @(objdump -p $Exe | Select-String 'DLL Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$others = @($imports | Where-Object { $_ -notmatch $protected } | Sort-Object -Unique)

# A copy of the exe with another Windows DLL beside it under the import's name: that one has none of the functions
# the exe asks for, so if Windows took it the copy could not start. --help into a pipe prints and exits: no window.
Add-Type -Namespace ExeChecks -Name Errors -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
[void][ExeChecks.Errors]::SetErrorMode(0x8003)   # a copy that can't start says so in its exit code, not in a box
function StartsWithStandIn([string]$dll) {
  $dir = Join-Path $env:TEMP 'qoqdir-exe-checks'
  if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
  New-Item -ItemType Directory $dir | Out-Null
  $copy = Join-Path $dir (Split-Path $Exe -Leaf)
  Copy-Item $Exe $copy
  $standIn = if ($dll -ieq 'version.dll') { 'wtsapi32.dll' } else { 'version.dll' }
  Copy-Item (Join-Path $env:SystemRoot "System32\$standIn") (Join-Path $dir $dll)
  $si = New-Object Diagnostics.ProcessStartInfo $copy, '--help'
  $si.UseShellExecute = $false; $si.RedirectStandardOutput = $true; $si.CreateNoWindow = $true
  $p = [Diagnostics.Process]::Start($si)
  [void]$p.StandardOutput.ReadToEnd()
  $exited = $p.WaitForExit(10000)
  if (-not $exited) { $p.Kill() }
  $started = $exited -and $p.ExitCode -eq 0
  for ($i = 0; $i -lt 20 -and (Test-Path $dir); $i++) {   # a virus scanner may hold the new copy a moment
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $dir) { Start-Sleep -Milliseconds 100 }
  }
  $started
}

Check ($size -lt 1MB) "the exe is one file under 1 MB ($size bytes)"
# A path under the user's folder holds the user's name, and the exe goes out to anyone. The builds name the compiler's
# and cargo's folders by short names instead (docs\BUILDING.md, section 2). Its 8-bit strings and its UTF-16 ones (at
# either byte offset), with either kind of slash.
$bytes = [IO.File]::ReadAllBytes($file.FullName)
$texts = [Text.Encoding]::UTF8.GetString($bytes), [Text.Encoding]::Unicode.GetString($bytes),
  [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
$userDirs = $env:USERPROFILE, ($env:USERPROFILE -replace '\\', '/')
$named = @(foreach ($t in $texts) {
  $userDirs | Where-Object { $t.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 }
})
Check ($named.Count -eq 0) 'no path in it names the user folder (%USERPROFILE%)'
Check ($imports.Count -gt 0) "objdump lists its imports ($($imports.Count); none means MinGW is not on the PATH)"
$taken = @($others | Where-Object { -not (StartsWithStandIn $_) })   # imports Windows took from beside the exe
$what = 'it imports only DLLs Windows protects'
if ($others) { $what += ", and $($others -join ', '), which Windows takes from System32 even with one beside the exe" }
Check ($taken.Count -eq 0) "$what$(if ($taken) { ': not ' + ($taken -join ', ') })"
FinishChecks
