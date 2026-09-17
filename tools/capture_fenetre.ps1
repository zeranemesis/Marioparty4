param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'partyboard',
    [string]$PathPrefix = ''
)

# Captures the game window to a PNG.
#
# This project has never been able to look at its own output. Every rendering
# defect on docs/defauts_graphiques.md was reported by a human describing what
# he saw, and answered by someone reading code and guessing. A picture ends
# that: the canonical hash excludes presentation by construction, so an image is
# the ONLY evidence a rendering defect can ever produce.
#
# Everything happens in C#, deliberately. Marshalling a RECT through PowerShell
# and [ref] hands the fields back as Object[], and `$rect.R - $rect.L` then
# fails with op_Subtraction - which cost two attempts before this rewrite.
#
# PrintWindow with PW_RENDERFULLCONTENT rather than CopyFromScreen: the latter
# captures whatever is on top of the window, so a covered or background window
# silently produces a picture of something else entirely.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$source = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class WinCap {
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
    const uint PW_RENDERFULLCONTENT = 0x2;

    public static string Capture(IntPtr window, string path) {
        RECT r;
        if (!GetClientRect(window, out r)) return "GetClientRect a echoue";
        int w = r.Right - r.Left, h = r.Bottom - r.Top;
        if (w <= 0 || h <= 0) return "taille invalide " + w + "x" + h;

        using (Bitmap bmp = new Bitmap(w, h)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr dc = g.GetHdc();
                bool ok = PrintWindow(window, dc, PW_RENDERFULLCONTENT);
                g.ReleaseHdc(dc);
                if (!ok) return "PrintWindow a echoue";
            }
            // A uniform image means the capture missed the swapchain, not that
            // the game drew nothing. Say which, rather than save a lie.
            int distinct = 0;
            int[] seen = new int[25];
            int n = 0;
            int[] xs = { 0, w / 3, w / 2, 2 * w / 3, w - 1 };
            int[] ys = { 0, h / 3, h / 2, 2 * h / 3, h - 1 };
            foreach (int x in xs) foreach (int y in ys) {
                int c = bmp.GetPixel(x, y).ToArgb();
                bool already = false;
                for (int i = 0; i < n; i++) if (seen[i] == c) { already = true; break; }
                if (!already) { seen[n++] = c; distinct++; }
            }
            bmp.Save(path, ImageFormat.Png);
            return "OK " + path + " (" + w + "x" + h + ", " + distinct
                + " teintes distinctes sur 25 points)"
                + (distinct <= 1 ? " ATTENTION: image uniforme, la capture a rate le rendu" : "");
        }
    }
}
'@
Add-Type -TypeDefinition $source -Language CSharp -ReferencedAssemblies System.Drawing | Out-Null

$candidates = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 -and
        ($PathPrefix -eq '' -or ($_.Path -and $_.Path.StartsWith($PathPrefix, 'OrdinalIgnoreCase'))) })
if ($candidates.Count -eq 0) { Write-Output 'AUCUNE FENETRE'; exit 2 }

$result = [WinCap]::Capture($candidates[0].MainWindowHandle, $Out)
Write-Output $result
if ($result -notlike 'OK *') { exit 2 }
