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
  if ($p.HasExited) { return '' }   # it stopped by itself (the crash log says why)
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
Check ($title -eq 'Public') "the same folder given twice opens one tab: '$title'"
$ini = Join-Path $dir 'qoqdir.ini'
Check (-not ((Test-Path $ini) -and (Select-String -Path $ini -Pattern ':\\' -Quiet))) 'qoqdir.ini holds no paths (saved tabs are shortcuts in Bookmarks)'
Check (Test-Path (Join-Path $dir 'Bookmarks') -PathType Container) 'a Bookmarks folder is created next to the exe'
$homeName = Split-Path ([Environment]::GetFolderPath('UserProfile')) -Leaf
$title = TitleAfterStart @()
Check ($title -eq $homeName) "no folders opens the home folder: '$title'"
$r = HelpExitCode '-h'
Check ($r.Shown -and $r.Code -eq 0) "-h shows the help and exits with 0 (shown: $($r.Shown), exit code $($r.Code))"
$r = HelpExitCode '-x'
Check ($r.Shown -and $r.Code -eq 1) "-x shows the error with the help and exits with 1 (shown: $($r.Shown), exit code $($r.Code))"
# A damaged qoqdir.ini: a number far off any screen gives the default (x + width must not overflow and stop it).
Set-Content -Path $ini -Value "[Window]`r`nX=2147483000`r`nWidth=1000`r`nHeight=600"
$title = TitleAfterStart @('-tC:\Users\Public')
Check ($title -eq 'Public') "a damaged qoqdir.ini (X=2147483000) falls back to the defaults: '$title'"

# A folder it can't write to, as Program Files is for most users: only adding files and folders is denied
# (to you, by your SID), so it can still start.
$ro = Join-Path $dir 'read-only'
New-Item -ItemType Directory $ro | Out-Null
Copy-Item $Exe $ro
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
[void](icacls $ro /deny "*${sid}:(OI)(CI)(WD,AD)")
$p = Start-Process (Join-Path $ro (Split-Path $Exe -Leaf)) -ArgumentList '-tC:\Users\Public' -PassThru
Start-Sleep -Milliseconds 2000
$p.Refresh(); $title = $p.MainWindowTitle; $box = [QoqdirHelp]::Dialog([uint32]$p.Id)
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
$written = @(Get-ChildItem -LiteralPath $ro -Force | Where-Object { $_.Name -ne (Split-Path $Exe -Leaf) }).Count
[void](icacls $ro /remove:d "*$sid")
Check ($title -eq 'Public' -and $box -eq [IntPtr]::Zero -and $p.ExitCode -eq 0 -and $written -eq 0) `
  "from a folder it can't write to, it runs and closes with no error box and writes nothing there ('$title', exit code $($p.ExitCode), files written: $written)"

Remove-Item -LiteralPath $dir -Recurse -Force
if ($failures) { "$failures check(s) failed"; exit 1 } else { 'all checks passed' }
