# Command-line checks for qoqdir.exe. Uses a private copy of the exe in a temp folder (with its
# own qoqdir.ini and Bookmarks), so your real settings are never touched. Each check opens a
# Qoqdir window for about two seconds and closes it again.
#
#   powershell -File tests\cli-checks.ps1 -Exe build\qoqdir.exe
#   powershell -File tests\cli-checks.ps1 -Exe rust\target\release\qoqdir.exe
param([Parameter(Mandatory)][string]$Exe)

Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public class QoqdirHelp {
  delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  // The message box (help text) shown by the given process, if any.
  public static IntPtr Dialog(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (owner == pid && IsWindowVisible(h) && c.ToString() == "#32770") found = h;
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@

$dir = Join-Path $env:TEMP 'qoqdir-cli-checks'
if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
New-Item -ItemType Directory $dir | Out-Null
Copy-Item $Exe $dir
$copy = Join-Path $dir (Split-Path $Exe -Leaf)
$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:failures++ }
}
function TitleAfterStart([string[]]$argList) {   # starts the copy, reads its window title, closes it
  $p = if ($argList) { Start-Process $copy -ArgumentList $argList -PassThru } else { Start-Process $copy -PassThru }
  Start-Sleep -Milliseconds 2000
  $p.Refresh(); $title = $p.MainWindowTitle
  [void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
  $title
}
function HelpExitCode([string]$arg) {   # starts the copy with a help/error switch, closes its help box
  $p = Start-Process $copy -ArgumentList $arg -PassThru
  Start-Sleep -Milliseconds 1500
  $box = [QoqdirHelp]::Dialog([uint32]$p.Id)
  if ($box -ne [IntPtr]::Zero) { [void][QoqdirHelp]::PostMessage($box, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # WM_CLOSE
  [void]$p.WaitForExit(5000)
  @{ Shown = $box -ne [IntPtr]::Zero; Code = $p.ExitCode }
}

$title = TitleAfterStart @('-tC:\Users\Public', '-tc:\users\public')
Check ($title -eq 'Public - Qoqdir') "the same folder given twice opens one tab: '$title'"
$ini = Join-Path $dir 'qoqdir.ini'
Check (-not ((Test-Path $ini) -and (Select-String -Path $ini -Pattern ':\\' -Quiet))) 'qoqdir.ini holds no paths (tabs are not saved)'
Check (Test-Path (Join-Path $dir 'Bookmarks') -PathType Container) 'a Bookmarks folder is created next to the exe'
$homeName = Split-Path ([Environment]::GetFolderPath('UserProfile')) -Leaf
$title = TitleAfterStart @()
Check ($title -eq "$homeName - Qoqdir") "no folders opens the home folder: '$title'"
$r = HelpExitCode '-h'
Check ($r.Shown -and $r.Code -eq 0) "-h shows the help and exits with 0 (shown: $($r.Shown), exit code $($r.Code))"
$r = HelpExitCode '-x'
Check ($r.Shown -and $r.Code -eq 1) "-x shows the error with the help and exits with 1 (shown: $($r.Shown), exit code $($r.Code))"

Remove-Item -LiteralPath $dir -Recurse -Force
if ($failures) { "$failures check(s) failed"; exit 1 } else { 'all checks passed' }
