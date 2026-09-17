param(
    [Parameter(Mandatory = $true)][string]$Keys,   # "DOWN,DOWN,X" - names from dolphin_touche.ps1
    [int]$HoldMs = 90,
    [int]$GapMs = 600,
    [string]$Out = '',
    [int]$SettleSeconds = 3
)

# Plays a SEQUENCE of pad keys into Dolphin's render window, then optionally
# photographs the result. One round trip instead of one per key, which matters
# when walking menus: reaching a given minigame is a dozen presses.
#
# Same two hard-won details as dolphin_touche.ps1, and for the same reasons:
#   - INPUT is a UNION, 40 bytes on x64. Declare only the keyboard arm and you
#     get 32, and SendInput rejects every call by returning 0 - silently.
#   - Dolphin's pad here is DirectInput, which reads the physical key state and
#     never sees SendKeys' window messages. Scan codes via SendInput do reach it.
#
# The focus is re-verified before EVERY key, not once at the start: a long
# sequence is exactly where a window can steal focus halfway through and the
# rest of the presses land in someone else's program.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Seq {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
        public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
        public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public MOUSEINPUT mi;
        [FieldOffset(8)] public KEYBDINPUT ki;
    }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int size);
    [DllImport("user32.dll")] public static extern uint MapVirtualKeyA(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool EnumWindows(Proc f, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextA(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool Proc(IntPtr h, IntPtr l);
    public const uint SCANCODE = 0x0008, KEYUP = 0x0002, EXTENDED = 0x0001;

    public static void Tap(ushort vk, int holdMs, bool extended) {
        ushort scan = (ushort)MapVirtualKeyA(vk, 0);
        uint flags = SCANCODE | (extended ? EXTENDED : 0u);
        INPUT[] down = new INPUT[1];
        down[0].type = 1; down[0].ki.wScan = scan; down[0].ki.dwFlags = flags;
        if (SendInput(1, down, Marshal.SizeOf(typeof(INPUT))) == 0)
            throw new Exception("SendInput a rejete l appui (taille de INPUT ?)");
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] up = new INPUT[1];
        up[0].type = 1; up[0].ki.wScan = scan; up[0].ki.dwFlags = flags | KEYUP;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@ -Language CSharp | Out-Null

# Names and codes mirror Config/GCPadNew.ini. Arrows are the MAIN STICK;
# the d-pad is T/G/F/H; triggers L=Q R=W; C-stick I/K/J/L.
$map = @{
    'RETURN' = @(0x0D, $false); 'X' = @(0x58, $false); 'Z' = @(0x5A, $false)
    'C' = @(0x43, $false); 'S' = @(0x53, $false); 'D' = @(0x44, $false)
    'UP' = @(0x26, $true); 'DOWN' = @(0x28, $true); 'LEFT' = @(0x25, $true); 'RIGHT' = @(0x27, $true)
    'T' = @(0x54, $false); 'G' = @(0x47, $false); 'F' = @(0x46, $false); 'H' = @(0x48, $false)
    'Q' = @(0x51, $false); 'W' = @(0x57, $false)
    'I' = @(0x49, $false); 'K' = @(0x4B, $false); 'J' = @(0x4A, $false); 'L' = @(0x4C, $false)
}

$wanted = @($Keys -split ',' | ForEach-Object { $_.Trim().ToUpperInvariant() } | Where-Object { $_ })
if (-not $wanted) { Write-Output 'aucune touche'; exit 2 }
foreach ($w in $wanted) {
    if (-not $map.ContainsKey($w)) { Write-Output "touche inconnue: $w"; exit 2 }
}

$pids = @(Get-Process -Name Dolphin -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
if (-not $pids) { Write-Output 'Dolphin ne tourne pas'; exit 2 }
$render = [IntPtr]::Zero
$cb = [Seq+Proc] {
    param($h, $l)
    $owner = 0; [Seq]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
    if ($pids -contains $owner -and [Seq]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 300
        [Seq]::GetWindowTextA($h, $sb, 300) | Out-Null
        if ($sb.ToString() -match 'FPS:') { $script:render = $h }
    }
    return $true
}
[Seq]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($render -eq [IntPtr]::Zero) { Write-Output 'fenetre de rendu introuvable'; exit 2 }

$sent = 0
foreach ($w in $wanted) {
    [Seq]::SetForegroundWindow($render) | Out-Null
    Start-Sleep -Milliseconds 150
    if ([Seq]::GetForegroundWindow() -ne $render) {
        Write-Output "ABANDON apres $sent touches: la fenetre de rendu a perdu le focus"
        exit 2
    }
    [Seq]::Tap([uint16]$map[$w][0], $HoldMs, [bool]$map[$w][1])
    $sent++
    Start-Sleep -Milliseconds $GapMs
}

if ($Out) {
    Start-Sleep -Seconds $SettleSeconds
    $r = New-Object Seq+RECT
    [Seq]::GetWindowRect($render, [ref]$r) | Out-Null
    $w2 = [int]$r.Right - [int]$r.Left; $h2 = [int]$r.Bottom - [int]$r.Top
    $bmp = New-Object System.Drawing.Bitmap($w2, $h2)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen([int]$r.Left, [int]$r.Top, 0, 0, (New-Object System.Drawing.Size($w2, $h2)))
    $g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Output ("OK {0} apres {1} touches: {2}" -f $Out, $sent, ($wanted -join ' '))
} else {
    Write-Output ("{0} touches injectees: {1}" -f $sent, ($wanted -join ' '))
}
