param(
    [Parameter(Mandatory = $true)][string]$Key,   # RETURN, X, Z, UP, DOWN, LEFT, RIGHT
    [int]$Repeat = 1,
    [int]$HoldMs = 90,
    [int]$GapMs = 700,
    [string]$Out = '',
    [int]$SettleSeconds = 3
)

# Injects a key at the driver level, then optionally photographs the result.
#
# SendKeys does NOT work here. Dolphin's pad on this machine is
# "DInput/0/Keyboard Mouse", and DirectInput reads the physical keyboard state
# rather than window messages - so SendKeys' WM_KEYDOWN is invisible to it. That
# is why twenty synthetic Starts left the title screen untouched and the game
# fell back to its attract loop.
#
# SendInput with SCAN CODES injects below that line and is seen by DirectInput.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Inj {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
        public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
        public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo;
    }
    // INPUT is a UNION. Its size comes from MOUSEINPUT - 40 bytes on x64.
    // Declaring only the keyboard arm yields 32, and SendInput then rejects
    // every call, returning 0 silently. That cost an afternoon.
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
        down[0].type = 1; down[0].ki.wVk = 0; down[0].ki.wScan = scan; down[0].ki.dwFlags = flags;
        if (SendInput(1, down, Marshal.SizeOf(typeof(INPUT))) == 0)
            throw new Exception("SendInput a rejete l appui (taille de INPUT ?)");
        System.Threading.Thread.Sleep(holdMs);
        INPUT[] up = new INPUT[1];
        up[0].type = 1; up[0].ki.wVk = 0; up[0].ki.wScan = scan; up[0].ki.dwFlags = flags | KEYUP;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@ -Language CSharp | Out-Null

$map = @{
    'RETURN' = @(0x0D, $false); 'X' = @(0x58, $false); 'Z' = @(0x5A, $false)
    'C' = @(0x43, $false); 'S' = @(0x53, $false); 'D' = @(0x44, $false)
    'UP' = @(0x26, $true); 'DOWN' = @(0x28, $true); 'LEFT' = @(0x25, $true); 'RIGHT' = @(0x27, $true)
    # D-Pad, read from Config/GCPadNew.ini: Up=T Down=G Left=F Right=H.
    # These had never been tested. The arrows above are the MAIN STICK, and
    # a menu that polls only the d-pad ignores them completely - which is
    # exactly what 'RIGHT and DOWN change nothing' looks like.
    'T' = @(0x54, $false); 'G' = @(0x47, $false)
    'F' = @(0x46, $false); 'H' = @(0x48, $false)
    # Triggers: L=Q, R=W. C-Stick: Up=I Down=K Left=J Right=L.
    'Q' = @(0x51, $false); 'W' = @(0x57, $false)
    'I' = @(0x49, $false); 'K' = @(0x4B, $false)
    'J' = @(0x4A, $false); 'L' = @(0x4C, $false)
}
$k = $Key.ToUpperInvariant()
if (-not $map.ContainsKey($k)) { Write-Output "touche inconnue: $Key"; exit 2 }

$pids = @(Get-Process -Name Dolphin -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
if (-not $pids) { Write-Output 'Dolphin ne tourne pas'; exit 2 }
$render = [IntPtr]::Zero
$cb = [Inj+Proc] {
    param($h, $l)
    $owner = 0; [Inj]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
    if ($pids -contains $owner -and [Inj]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 300
        [Inj]::GetWindowTextA($h, $sb, 300) | Out-Null
        if ($sb.ToString() -match 'FPS:') { $script:render = $h }
    }
    return $true
}
[Inj]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($render -eq [IntPtr]::Zero) { Write-Output 'fenetre de rendu introuvable'; exit 2 }

[Inj]::SetForegroundWindow($render) | Out-Null
Start-Sleep -Milliseconds 400
if ([Inj]::GetForegroundWindow() -ne $render) { Write-Output 'ABANDON: la fenetre de rendu n a pas le focus'; exit 2 }

for ($i = 0; $i -lt $Repeat; $i++) {
    [Inj]::Tap([uint16]$map[$k][0], $HoldMs, [bool]$map[$k][1])
    Start-Sleep -Milliseconds $GapMs
}

if ($Out) {
    Start-Sleep -Seconds $SettleSeconds
    $r = New-Object Inj+RECT
    [Inj]::GetWindowRect($render, [ref]$r) | Out-Null
    $w = [int]$r.Right - [int]$r.Left; $h2 = [int]$r.Bottom - [int]$r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h2)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen([int]$r.Left, [int]$r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h2)))
    $g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Output ("OK {0} apres {1} x {2}" -f $Out, $Repeat, $k)
} else {
    Write-Output ("{0} x {1} injecte" -f $Repeat, $k)
}
