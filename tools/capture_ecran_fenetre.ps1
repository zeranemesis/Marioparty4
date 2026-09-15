param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'Dolphin'
)

# Captures a window by reading the SCREEN over its rectangle.
#
# PrintWindow returns black for a hardware-accelerated surface, which is what an
# emulator draws into: Dolphin's render window photographs as a uniform image.
# Reading the screen works, at the cost of capturing whatever overlaps - so the
# window is brought to the front first and the result is reported rather than
# assumed.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ScreenCap {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@ -Language CSharp | Out-Null

$target = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $target) { Write-Output "AUCUNE FENETRE pour $ProcessName"; exit 2 }

[ScreenCap]::SetForegroundWindow($target.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500

$r = New-Object ScreenCap+RECT
[ScreenCap]::GetWindowRect($target.MainWindowHandle, [ref]$r) | Out-Null
$w = [int]$r.Right - [int]$r.Left
$h = [int]$r.Bottom - [int]$r.Top
if ($w -le 0 -or $h -le 0) { Write-Output "taille invalide ${w}x${h}"; exit 2 }

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen([int]$r.Left, [int]$r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("OK {0} ({1}x{2} a {3},{4})" -f $Out, $w, $h, $r.Left, $r.Top)
