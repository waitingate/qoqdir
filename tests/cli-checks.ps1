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
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetDlgItemText(IntPtr h, int id, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct STARTUPINFO { public int cb; public string lpReserved, lpDesktop, lpTitle; public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  // Runs a command line with no console (DETACHED_PROCESS): what it starts has no terminal in reach, as from a shortcut.
  public static void StartDetached(string cmd) {
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); PROCESS_INFORMATION pi;
    if (CreateProcess(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, 8, IntPtr.Zero, null, ref si, out pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  }
  // A message box's text (its static control, id 0xFFFF), and its title.
  public static string Text(IntPtr box) { var s = new StringBuilder(8192); GetDlgItemText(box, 0xFFFF, s, s.Capacity); return s.ToString(); }
  public static string Title(IntPtr box) { var s = new StringBuilder(256); GetWindowText(box, s, s.Capacity); return s.ToString(); }
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
function HelpOutput([string]$arg) {   # runs the copy with its output going to a pipe, as in "qoqdir -h | more"
  $si = New-Object Diagnostics.ProcessStartInfo $copy, $arg
  $si.UseShellExecute = $false; $si.RedirectStandardOutput = $true; $si.StandardOutputEncoding = [Text.Encoding]::UTF8
  $p = [Diagnostics.Process]::Start($si)
  $text = $p.StandardOutput.ReadToEnd(); $p.WaitForExit()
  @{ Text = $text; Code = $p.ExitCode }
}
function HelpBox([string]$arg) {   # starts the copy as a shortcut would, with no terminal to print in; closes its box
  [QoqdirHelp]::StartDetached("cmd.exe /c start `"`" `"$copy`" $arg")   # a cmd with no console starts it
  Start-Sleep -Milliseconds 1500
  $p = Get-Process | Where-Object { $_.Path -eq $copy } | Select-Object -First 1
  if (-not $p) { return @{ Shown = $false; Code = 'none: it did not run' } }
  [void]$p.Handle   # opened while it runs, so that its exit code can be read after it ends
  $box = [QoqdirHelp]::Dialog([uint32]$p.Id)
  $text = if ($box -ne [IntPtr]::Zero) { [QoqdirHelp]::Text($box) } else { '' }
  $title = if ($box -ne [IntPtr]::Zero) { [QoqdirHelp]::Title($box) } else { '' }
  if ($box -ne [IntPtr]::Zero) { [void][QoqdirHelp]::PostMessage($box, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # WM_CLOSE
  [void]$p.WaitForExit(5000)
  @{ Shown = $box -ne [IntPtr]::Zero; Code = $p.ExitCode; Text = $text; Title = $title }
}

$title = TitleAfterStart @('--tab=C:\Users\Public', '-t="c:\users\public"')
Check ($title -eq 'Public') "the same folder given twice (--tab=, then -t= with quotes) opens one tab: '$title'"
$ini = Join-Path $dir 'qoqdir.ini'
Check (-not ((Test-Path $ini) -and (Select-String -Path $ini -Pattern ':\\' -Quiet))) 'qoqdir.ini holds no paths (saved tabs are shortcuts in Bookmarks)'
Check (Test-Path (Join-Path $dir 'Bookmarks') -PathType Container) 'a Bookmarks folder is created next to the exe'
$homeName = Split-Path ([Environment]::GetFolderPath('UserProfile')) -Leaf
$title = TitleAfterStart @()
Check ($title -eq $homeName) "no folders opens the home folder: '$title'"
$r = HelpOutput '--help'   # the switch F1's help names (-h and -? are the same: unit tests)
Check ($r.Code -eq 0 -and $r.Text -match '^Qoqdir \d' -and $r.Text -match 'Usage: qoqdir \[options\]' -and $r.Text -match '--tab="folder path"') `
  "--help writes the command-line help where its output goes (a pipe here) and exits with 0 (exit code $($r.Code))"
$r = HelpOutput '-x'
Check ($r.Code -eq 1 -and $r.Text.StartsWith('Unknown option: -x') -and $r.Text -match 'Usage: qoqdir') `
  "-x writes the error, then the help, and exits with 1 (exit code $($r.Code))"
# F1 in the window: posted to the file list as a key press would arrive (nothing global), then the box is closed.
$p = Start-Process $copy -ArgumentList '--tab=C:\Users\Public' -PassThru
Start-Sleep -Milliseconds 2000
$p.Refresh()
[void][QoqdirHelp]::PostMessage([QoqdirHelp]::GetDlgItem($p.MainWindowHandle, 108), 0x0100, [IntPtr]0x70, [IntPtr]::Zero)   # WM_KEYDOWN, VK_F1
Start-Sleep -Milliseconds 1000
$box = [QoqdirHelp]::Dialog([uint32]$p.Id)
$text = if ($box -ne [IntPtr]::Zero) { [QoqdirHelp]::Text($box) } else { '' }
if ($box -ne [IntPtr]::Zero) { [void][QoqdirHelp]::PostMessage($box, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
Start-Sleep -Milliseconds 300
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
Check ($text.StartsWith('Navigation') -and $text -match 'Refresh: F5' -and $text -match 'type qoqdir --help') `
  "F1 in the window shows the keys and points to qoqdir --help (shown: $($box -ne [IntPtr]::Zero))"
$r = HelpBox '-x'
Check ($r.Shown -and $r.Code -eq 1 -and $r.Text.StartsWith('Unknown option: -x') -and $r.Title -match '^Qoqdir \d') `
  "with no terminal (as from a shortcut), -x shows the error and help in a box and exits with 1 (shown: $($r.Shown), exit code $($r.Code))"
# A damaged qoqdir.ini: a number far off any screen gives the default (x + width must not overflow and stop it).
Set-Content -Path $ini -Value "[Window]`r`nX=2147483000`r`nWidth=1000`r`nHeight=600"
$title = TitleAfterStart @('--tab=C:\Users\Public')
Check ($title -eq 'Public') "a damaged qoqdir.ini (X=2147483000) falls back to the defaults: '$title'"

# A folder it can't write to, as Program Files is for most users: only adding files and folders is denied
# (to you, by your SID), so it can still start.
$ro = Join-Path $dir 'read-only'
New-Item -ItemType Directory $ro | Out-Null
Copy-Item $Exe $ro
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
[void](icacls $ro /deny "*${sid}:(OI)(CI)(WD,AD)")
$p = Start-Process (Join-Path $ro (Split-Path $Exe -Leaf)) -ArgumentList '--tab=C:\Users\Public' -PassThru
Start-Sleep -Milliseconds 2000
$p.Refresh(); $title = $p.MainWindowTitle; $box = [QoqdirHelp]::Dialog([uint32]$p.Id)
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
$written = @(Get-ChildItem -LiteralPath $ro -Force | Where-Object { $_.Name -ne (Split-Path $Exe -Leaf) }).Count
[void](icacls $ro /remove:d "*$sid")
Check ($title -eq 'Public' -and $box -eq [IntPtr]::Zero -and $p.ExitCode -eq 0 -and $written -eq 0) `
  "from a folder it can't write to, it runs and closes with no error box and writes nothing there ('$title', exit code $($p.ExitCode), files written: $written)"

Remove-Item -LiteralPath $dir -Recurse -Force
if ($failures) { "$failures check(s) failed"; exit 1 } else { 'all checks passed' }
