# Command-line checks for qoqdir.exe. Uses a private copy of the exe in a temp folder (with its
# own qoqdir.ini and Bookmarks), so your real settings are never touched. Each check opens a
# Qoqdir window until it shows what the check reads, and closes it again.
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
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
  // Runs a command line. flags 8 (DETACHED_PROCESS): no console, so what starts has no terminal in reach, as from a
  // shortcut. show 4 (SW_SHOWNOACTIVATE): a window that takes no focus, so the checks never pull the keyboard away
  // from whoever is typing (7.2). Returns the process id, 0 when it didn't start.
  public static int Start(string cmd, uint flags, short show) { return Start(cmd, flags, show, 0, 0, 0, 0); }
  // The same with a place and size for its window (w > 0), as Open in new window gives one (5.8).
  public static int Start(string cmd, uint flags, short show, int x, int y, int w, int h) {
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); PROCESS_INFORMATION pi;
    if (show != 0) { si.dwFlags = 1; si.wShowWindow = show; }   // STARTF_USESHOWWINDOW
    if (w > 0) { si.dwFlags |= 6; si.dwX = x; si.dwY = y; si.dwXSize = w; si.dwYSize = h; }   // _USESIZE, _USEPOSITION
    if (!CreateProcess(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, flags, IntPtr.Zero, null, ref si, out pi)) return 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return pi.dwProcessId;
  }
  [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
  [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT pt, uint flags);
  [DllImport("shcore.dll")] static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint dpiX, out uint dpiY);
  // Where a window is, and px pixels at 100% at the scaling of the monitor at (x, y), in the screen's own pixels: as a
  // program that follows each monitor's scaling sees them, as Qoqdir does (not scaled down for this one).
  public static string Place(IntPtr h) {
    IntPtr old = SetThreadDpiAwarenessContext(new IntPtr(-4));   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    RECT r; GetWindowRect(h, out r);
    SetThreadDpiAwarenessContext(old);
    return r.Left + "," + r.Top + "," + r.Right + "," + r.Bottom;
  }
  public static int Scaled(int px, int x, int y) {
    IntPtr old = SetThreadDpiAwarenessContext(new IntPtr(-4));
    var at = new POINT(); at.X = x; at.Y = y;
    uint dpiX, dpiY;
    GetDpiForMonitor(MonitorFromPoint(at, 2), 0, out dpiX, out dpiY);   // MONITOR_DEFAULTTONEAREST, MDT_EFFECTIVE_DPI
    SetThreadDpiAwarenessContext(old);
    return (int)(px * dpiX / 96);
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

# A copy an earlier run left open keeps its exe in use: then stop, rather than check that old exe in place of -Exe.
$dir = Join-Path $env:TEMP 'qoqdir-cli-checks'
try {
  if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction Stop }
  New-Item -ItemType Directory $dir -ErrorAction Stop | Out-Null
  Copy-Item $Exe $dir -ErrorAction Stop
} catch { "can't set up ${dir}: $($_.Exception.Message) (a copy from an earlier run still open?)"; exit 1 }
$copy = Join-Path $dir (Split-Path $Exe -Leaf)
$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { "ok    $what" } else { "FAIL  $what"; $script:failures++ }
}
function WaitFor([scriptblock]$what, [int]$ms) {   # asks every 50 ms until $what gives something, for $ms at most
  $clock = [Diagnostics.Stopwatch]::StartNew()   # not the time of day, which a clock change moves
  while ($true) {
    $got = & $what
    if ($got -or $clock.ElapsedMilliseconds -ge $ms) { return $got }
    Start-Sleep -Milliseconds 50
  }
}
function Box($p) {   # the message box a process shows, or nothing
  $box = [QoqdirHelp]::Dialog([uint32]$p.Id)
  if ($box -ne [IntPtr]::Zero) { $box }
}
function CloseBox($p) {   # waits for the process's message box, closes it, and gives what it said
  $box = WaitFor { Box $p } 5000
  if (-not $box) { return @{ Shown = $false; Text = ''; Title = '' } }
  $r = @{ Shown = $true; Text = [QoqdirHelp]::Text($box); Title = [QoqdirHelp]::Title($box) }
  [void][QoqdirHelp]::PostMessage($box, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
  [void](WaitFor { -not (Box $p) } 2000)
  $r
}
function StartQuiet([string]$exe, [string[]]$argList, [int[]]$place = @(0, 0, 0, 0)) {   # a window that takes no focus
  $id = [QoqdirHelp]::Start("`"$exe`" $($argList -join ' ')", 0, 4, $place[0], $place[1], $place[2], $place[3])
  $p = if ($id) { Get-Process -Id $id -ErrorAction SilentlyContinue }
  if ($p) { [void]$p.Handle }   # opened while it runs, so that its exit code can be read after it ends
  $p
}
# Starts the copy and waits until its window shows $title, or for a focus check $settle ms (time to take the focus,
# if it would), then reads its title and whether it took the focus, and closes it.
function TitleAfterStart([string[]]$argList, [string]$title, [int]$settle = 0) {
  $p = StartQuiet $copy $argList
  if ($settle) { Start-Sleep -Milliseconds $settle }
  elseif ($p) { [void](WaitFor { $p.Refresh(); $p.HasExited -or $p.MainWindowTitle -eq $title } 5000) }
  if (-not $p -or $p.HasExited) { return @{ Title = ''; Focused = $false } }   # it stopped by itself (the crash log says why)
  $p.Refresh()
  $r = @{ Title = $p.MainWindowTitle; Focused = [QoqdirHelp]::GetForegroundWindow() -eq $p.MainWindowHandle }
  [void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
  $r
}
function HelpOutput([string]$arg) {   # runs the copy with its output going to a pipe, as in "qoqdir -h | more"
  $si = New-Object Diagnostics.ProcessStartInfo $copy, $arg
  $si.UseShellExecute = $false; $si.RedirectStandardOutput = $true; $si.StandardOutputEncoding = [Text.Encoding]::UTF8
  $p = [Diagnostics.Process]::Start($si)
  $text = $p.StandardOutput.ReadToEnd(); $p.WaitForExit()
  @{ Text = $text; Code = $p.ExitCode }
}
function HelpBox([string]$arg) {   # starts the copy as a shortcut would, with no terminal to print in; closes its box
  [void][QoqdirHelp]::Start("cmd.exe /c start `"`" `"$copy`" $arg", 8, 0)   # a cmd with no console starts it
  $name = [IO.Path]::GetFileNameWithoutExtension($copy)
  $p = WaitFor { Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $copy } | Select-Object -First 1 } 5000
  if (-not $p) { return @{ Shown = $false; Code = 'none: it did not run' } }
  [void]$p.Handle   # opened while it runs, so that its exit code can be read after it ends
  $r = CloseBox $p
  [void]$p.WaitForExit(5000)
  $r + @{ Code = $p.ExitCode }
}

$r = TitleAfterStart @('--tab=C:\Users\Public', '-t="c:\users\public"') 'Public' 2000
Check ($r.Title -eq 'Public') "the same folder given twice (--tab=, then -t= with quotes) opens one tab: '$($r.Title)'"
Check (-not $r.Focused) 'started by a script that asks for no focus (SW_SHOWNOACTIVATE), the window takes none'
$ini = Join-Path $dir 'qoqdir.ini'
Check (-not ((Test-Path $ini) -and (Select-String -Path $ini -Pattern ':\\' -Quiet))) 'qoqdir.ini holds no paths (saved tabs are shortcuts in Bookmarks)'
Check (Test-Path (Join-Path $dir 'Bookmarks') -PathType Container) 'a Bookmarks folder is created next to the exe'
$homeName = Split-Path ([Environment]::GetFolderPath('UserProfile')) -Leaf
$r = TitleAfterStart @() $homeName
Check ($r.Title -eq $homeName) "no folders opens the home folder$(if ($r.Title -ne $homeName) { ": '$($r.Title)'" })"   # the name stays out of a log that passes
$r = TitleAfterStart @("`"$env:WINDIR\win.ini`"") 'Windows'
Check ($r.Title -eq 'Windows') "a file opens its folder, as explorer /select, does: '$($r.Title)'"
$r = HelpOutput '--help'   # the switch F1's help names (-h and -? are the same: unit tests)
Check ($r.Code -eq 0 -and $r.Text -match '^Qoqdir \d' -and $r.Text -match 'Usage: qoqdir \[options\]' -and $r.Text -match '--tab="folder path"') `
  "--help writes the command-line help where its output goes (a pipe here) and exits with 0 (exit code $($r.Code))"
$r = HelpOutput '-x'
Check ($r.Code -eq 1 -and $r.Text.StartsWith('Unknown option: -x') -and $r.Text -match 'Usage: qoqdir') `
  "-x writes the error, then the help, and exits with 1 (exit code $($r.Code))"
# F1 in the window: posted to the file list as a key press would arrive (nothing global), then the box is closed.
$p = StartQuiet $copy '--tab=C:\Users\Public'
[void](WaitFor { $p.Refresh(); $p.MainWindowTitle -eq 'Public' } 5000)
[void][QoqdirHelp]::PostMessage([QoqdirHelp]::GetDlgItem($p.MainWindowHandle, 108), 0x0100, [IntPtr]0x70, [IntPtr]::Zero)   # WM_KEYDOWN, VK_F1
$r = CloseBox $p
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
Check ($r.Text.StartsWith('Navigation') -and $r.Text -match 'Refresh: F5' -and $r.Text -match 'type qoqdir --help') `
  "F1 in the window shows the keys and points to qoqdir --help (shown: $($r.Shown))"
$r = HelpBox '-x'
Check ($r.Shown -and $r.Code -eq 1 -and $r.Text.StartsWith('Unknown option: -x') -and $r.Title -match '^Qoqdir \d') `
  "with no terminal (as from a shortcut), -x shows the error and help in a box and exits with 1 (shown: $($r.Shown), exit code $($r.Code))"
# A damaged qoqdir.ini: a number far off any screen gives the default (x + width must not overflow and stop it).
Set-Content -Path $ini -Value "[Window]`r`nX=2147483000`r`nWidth=1000`r`nHeight=600"
$r = TitleAfterStart @('--tab=C:\Users\Public') 'Public'
Check ($r.Title -eq 'Public') "a damaged qoqdir.ini (X=2147483000) falls back to the defaults: '$($r.Title)'"
# Started with a place and size, as Open in new window starts one (5.8): there, not at the place saved.
Set-Content -Path $ini -Value "[Window]`r`nX=300`r`nY=200`r`nWidth=600`r`nHeight=400"
$w = [QoqdirHelp]::Scaled(600, 100, 80); $h = [QoqdirHelp]::Scaled(400, 100, 80)   # over its least size, 520 by 320
$p = StartQuiet $copy '--tab=C:\Users\Public' @(100, 80, $w, $h)
[void](WaitFor { $p.Refresh(); $p.MainWindowTitle -eq 'Public' } 5000)
$place = [QoqdirHelp]::Place($p.MainWindowHandle)
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
Check ($place -eq "100,80,$(100 + $w),$(80 + $h)") `
  "started with a place and size, it opens there, not at the saved place (at $place)"

# Started with no place, it opens at the place saved in qoqdir.ini (5.11), made there directly so a monitor of other
# scaling does not resize it. Physical pixels, over the least size, on the primary monitor.
$sw = [QoqdirHelp]::Scaled(820, 120, 90); $sh = [QoqdirHelp]::Scaled(540, 120, 90)
Set-Content -Path $ini -Value "[Window]`r`nX=120`r`nY=90`r`nWidth=$sw`r`nHeight=$sh"
$p = StartQuiet $copy '--tab=C:\Users\Public'
[void](WaitFor { $p.Refresh(); $p.HasExited -or $p.MainWindowTitle -eq 'Public' } 5000)
$place = [QoqdirHelp]::Place($p.MainWindowHandle)
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
Check ($place -eq "120,90,$(120 + $sw),$(90 + $sh)") `
  "started with no place, it opens at the place saved in qoqdir.ini (at $place)"

# A folder it can't write to, as Program Files is for most users: only adding files and folders is denied
# (to you, by your SID), so it can still start.
$ro = Join-Path $dir 'read-only'
New-Item -ItemType Directory $ro | Out-Null
Copy-Item $Exe $ro
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
[void](icacls $ro /deny "*${sid}:(OI)(CI)(WD,AD)")
$p = StartQuiet (Join-Path $ro (Split-Path $Exe -Leaf)) '--tab=C:\Users\Public'
Start-Sleep -Milliseconds 2000
$p.Refresh(); $title = $p.MainWindowTitle; $box = [QoqdirHelp]::Dialog([uint32]$p.Id)
[void]$p.CloseMainWindow(); [void]$p.WaitForExit(5000)
$written = @(Get-ChildItem -LiteralPath $ro -Force | Where-Object { $_.Name -ne (Split-Path $Exe -Leaf) }).Count
[void](icacls $ro /remove:d "*$sid")
Check ($title -eq 'Public' -and $box -eq [IntPtr]::Zero -and $p.ExitCode -eq 0 -and $written -eq 0) `
  "from a folder it can't write to, it runs and closes with no error box and writes nothing there ('$title', exit code $($p.ExitCode), files written: $written)"

Remove-Item -LiteralPath $dir -Recurse -Force
if ($failures) { "$failures check(s) failed"; exit 1 } else { 'all checks passed' }
