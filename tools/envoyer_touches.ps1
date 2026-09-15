param(
    [Parameter(Mandatory = $true)][string]$ProcessName,
    [Parameter(Mandatory = $true)][string]$Keys,
    [int]$RepeatCount = 1,
    [int]$GapMilliseconds = 120
)

# Sends keystrokes to another program's window.
#
# Needed to drive Dolphin, which has no scripting interface: the only way to
# reach a given minigame is to play the menus, and the only way to play them
# from here is to synthesise the keys a person would press. Dolphin's GameCube
# pad is mapped to the keyboard on this machine (A = X, B = Z, Start = Return,
# stick = arrows), so the game itself is reachable the same way.
#
# SendKeys goes to whatever has focus, so the window is brought forward first
# and the result is verified rather than assumed.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Focuser {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public const int SW_RESTORE = 9;
}
'@ -Language CSharp | Out-Null

$target = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $target) { Write-Output "AUCUNE FENETRE pour $ProcessName"; exit 2 }

[Focuser]::ShowWindow($target.MainWindowHandle, [Focuser]::SW_RESTORE) | Out-Null
[Focuser]::SetForegroundWindow($target.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400

if ([Focuser]::GetForegroundWindow() -ne $target.MainWindowHandle) {
    Write-Output 'AVERTISSEMENT: la fenetre n a pas pris le focus, les touches iront ailleurs'
    exit 2
}

for ($i = 0; $i -lt $RepeatCount; $i++) {
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
    Start-Sleep -Milliseconds $GapMilliseconds
}
Write-Output ("envoye {0} x {1} a {2}" -f $RepeatCount, $Keys, $ProcessName)
