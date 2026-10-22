# Both versions must take the same self-test steps with the same results (docs\DEVELOPING.md): compares the two
# self-test logs, the notes and timings at the line ends aside. Used by scripts\build.ps1 -Test and
# scripts\release.ps1, so the rule lives here once.
#
#   powershell -File tests\compare-selftests.ps1
param([string]$CppLog = (Join-Path $env:TEMP 'qoqdir-selftest.log'),
      [string]$RustLog = (Join-Path $env:TEMP 'qoqdir-rs-selftest.log'))

foreach ($log in $CppLog, $RustLog) {
  if (-not (Test-Path -LiteralPath $log)) { "no self-test log: $log"; exit 1 }
}
$cpp, $rust = $CppLog, $RustLog | ForEach-Object {
  ((Get-Content -LiteralPath $_ -Encoding UTF8) -replace '   \(slow step: \d+ ms\)$' -replace '   \[.*\]$') -join "`n"
}
if ($cpp -ne $rust) { 'The two versions did not take the same self-test steps.'; exit 1 }
'both versions took the same self-test steps'
