# Runs "qoqdir-test.exe --selftest" and lists every new window that appears meanwhile, from ANY
# program, so a Windows error window caused by the app (an Explorer "can't access" window, a
# dialog) is caught. It only reports: it never closes or touches any window. Other programs you
# use during the run show up too; only Qoqdir's own windows (and Windows' copy-progress windows
# from the test's own process) are expected.
#
#   powershell -File tests\watch-windows.ps1 -Exe build\qoqdir-test.exe
#   powershell -File tests\watch-windows.ps1 -Exe rust\target\release\qoqdir.exe -Log $env:TEMP\qoqdir-rs-selftest.log
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

$before = [QoqdirWindows]::Visible()
$p = Start-Process $Exe -ArgumentList '--selftest' -PassThru
$seen = [ordered]@{}
$busy = 0
while (-not $p.HasExited) {
  foreach ($w in [QoqdirWindows]::Visible().GetEnumerator()) {
    if ($before.ContainsKey($w.Key) -or $seen.Contains($w.Key) -or $w.Value -match "^pid $($p.Id) \[Qoqdir\]") { continue }
    $seen[$w.Key] = $w.Value
    if ($w.Value -match "\[Ghost\] '.*Qoqdir") { $busy++ }   # Windows' stand-in for a window busy 5+ s
  }
  Start-Sleep -Milliseconds 200
}
"self-test (pid $($p.Id)) exit code: $($p.ExitCode)"
if ($busy) { "Qoqdir's window was busy for 5+ seconds $busy time(s) (the self-test's own setup of 10,000 test files does this)" }
$others = @($seen.Values | Where-Object { $_ -notmatch "\[Ghost\] '.*Qoqdir" })
if ($others) { 'new windows during the run:'; $others | ForEach-Object { "  $_" } } else { 'no new windows' }
if (Test-Path $Log) { Get-Content -Encoding UTF8 $Log | Select-String 'FAIL|passed' }
exit $p.ExitCode
