param(
    [string]$Keys = '',
    [int]$Repeat = 1,
    [int]$GapMs = 900,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$SettleSeconds = 3
)

# Presses keys into Dolphin's RENDER window and photographs the result.
#
# Dolphin has no scripting interface, so reaching a given minigame means playing
# the menus. On this machine its GameCube pad is on the keyboard: A = X, B = Z,
# Start = Return, stick = arrows.
#
# Two things this gets right, because both cost an hour when they were wrong:
#   - it targets the RENDER window, not the main GUI: Dolphin has two top-level
#     windows and the main one is the game list;
#   - it captures from the SCREEN, because PrintWindow returns black on a
#     hardware-accelerated surface.
#
# The focus is verified before every send. Synthetic keys go wherever the focus
# is, and on a machine with other programs open that is a way to type into
# something else entirely.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Dolph {
    [DllImport("user32.dll")] public static extern bool EnumWindows(Proc f, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextA(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool Proc(IntPtr h, IntPtr l);
}
'@ -Language CSharp | Out-Null

$pids = @(Get-Process -Name Dolphin -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
if (-not $pids) { Write-Output 'Dolphin ne tourne pas'; exit 2 }

$render = [IntPtr]::Zero
$cb = [Dolph+Proc] {
    param($h, $l)
    $owner = 0
    [Dolph]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
    if ($pids -contains $owner -and [Dolph]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 300
        [Dolph]::GetWindowTextA($h, $sb, 300) | Out-Null
        # The render window is the one whose title carries the emulation status.
        if ($sb.ToString() -match 'FPS:') { $script:render = $h }
    }
    return $true
}
[Dolph]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($render -eq [IntPtr]::Zero) { Write-Output 'fenetre de rendu introuvable (emulation arretee ?)'; exit 2 }

if ($Keys) {
    for ($i = 0; $i -lt $Repeat; $i++) {
        [Dolph]::SetForegroundWindow($render) | Out-Null
        Start-Sleep -Milliseconds 250
        if ([Dolph]::GetForegroundWindow() -ne $render) {
            Write-Output "ABANDON au tour $i : la fenetre de rendu n a pas le focus"
            exit 2
        }
        [System.Windows.Forms.SendKeys]::SendWait($Keys)
        Start-Sleep -Milliseconds $GapMs
    }
}

Start-Sleep -Seconds $SettleSeconds
$r = New-Object Dolph+RECT
[Dolph]::GetWindowRect($render, [ref]$r) | Out-Null
$w = [int]$r.Right - [int]$r.Left
$h = [int]$r.Bottom - [int]$r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen([int]$r.Left, [int]$r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("OK {0} ({1}x{2}) apres {3} x '{4}'" -f $Out, $w, $h, $Repeat, $Keys)
