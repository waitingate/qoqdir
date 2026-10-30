# What exe-checks.ps1 and cli-checks.ps1 share: a line for each check, and an exit code for how they went.
$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:failures++ }
}
function FinishChecks {
  if ($failures) { "$failures check(s) failed"; exit 1 } else { 'all checks passed' }
}
