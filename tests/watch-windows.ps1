# Runs "qoqdir-test.exe --selftest" and lists every new window that appears meanwhile, from ANY
# program, so a Windows error window caused by the app (an Explorer "can't access" window, a
# dialog) is caught. It only reports: it never closes or touches any window. Other programs you
# use during the run show up too; only Qoqdir's own windows (and Windows' copy-progress windows
# from the test's own process) are expected.
#
#   powershell -File tests\watch-windows.ps1 -Exe build\qoqdir-test.exe
#   powershell -File tests\watch-windows.ps1 -Exe rust\target\selftest\release\qoqdir.exe -Log $env:TEMP\qoqdir-rs-selftest.log
param([Parameter(Mandatory)][string]$Exe, [string]$Log = (Join-Path $env:TEMP 'qoqdir-selftest.log'))

Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public class QoqdirWindows {
  delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public static Dictionary<long, string> Visible() {
    var r = new Dictionary<long, string>();
    EnumWindows((h, p) => {
      if (IsWindowVisible(h)) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        var c = new StringBuilder(64); GetClassName(h, c, 64);
        var t = new StringBuilder(256); GetWindowText(h, t, 256);
        r[h.ToInt64()] = "pid " + pid + " [" + c + "] '" + t + "'";
      }
      return true;
    }, IntPtr.Zero);
    return r;
  }
}
'@

# A run empties its log as it starts, which fails while something else has the file open: the last run's lines would
# stay in it and spoil the comparison of the two logs. So the old log goes first, with time for that to let go.
for ($i = 0; $i -lt 50 -and (Test-Path -LiteralPath $Log); $i++) {
  Remove-Item -LiteralPath $Log -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $Log) { Start-Sleep -Milliseconds 100 }
}
$before = [QoqdirWindows]::Visible()
$p = Start-Process $Exe -ArgumentList '--selftest' -PassThru
if (-not $p) { "could not start $Exe"; exit 1 }   # a bad path or a quarantined exe: fail, don't spin on a null
$seen = [ordered]@{}
$busy = 0
while (-not $p.HasExited) {
  foreach ($w in [QoqdirWindows]::Visible().GetEnumerator()) {
    if ($before.ContainsKey($w.Key) -or $seen.Contains($w.Key) -or $w.Value -match "^pid $($p.Id) \[Qoqdir\]") { continue }
    $seen[$w.Key] = $w.Value
    if ($w.Value -match '\[Ghost\] ') { $busy++ }   # Windows' stand-in for a window busy 5+ s (titled as the folder shown)
  }
  [void]$p.WaitForExit(200)
}
"self-test (pid $($p.Id)) exit code: $($p.ExitCode)"
if ($busy) { "Qoqdir's window was busy for 5+ seconds $busy time(s) (the self-test's own setup of 10,000 files, and of 10,000 folders, does this)" }
$others = @($seen.Values | Where-Object { $_ -notmatch '\[Ghost\] ' })
if ($others) { 'new windows during the run:'; $others | ForEach-Object { "  $_" } } else { 'no new windows' }
if (Test-Path $Log) { Get-Content -Encoding UTF8 $Log | Select-String 'FAIL|passed' }
# A dialog or message box from the test's own process is a step that touched the desktop (7.2): the run fails on it,
# as it does on a failed step. Other programs' windows can't be told from the user's own and are only listed.
$dialogs = @($others | Where-Object { $_ -match "^pid $($p.Id) \[#32770\]" })
if ($dialogs) { "FAIL  the self-test opened $($dialogs.Count) dialog(s) of its own"; if (-not $p.ExitCode) { exit 1 } }
exit $p.ExitCode
