<#
.SYNOPSIS
Drives the game to a real scene and reports what the ray tracing did there.

.DESCRIPTION
Everything before this script measured the ray tracing on the title screen and
the mode select, which load the GPU least and look nothing like the scenes the
game is actually played in. This drives the game to a board or a mini-game,
captures frames, and prints the handful of numbers that say whether the
settings are calibrated for that scene or only for the one they were tuned on.

Three things about the game's menus are not obvious and are what previous
attempts kept failing on:

  * they move their cursor with the analog stick and ignore the d-pad;
  * they poll the stick with no edge detection, so a held pulse moves the
    cursor once per frame it is held -- stick pulses must last one frame;
  * the party setup screens poll a pad port per player and wait for all four
    to confirm, so a press sent only to port 0 can never satisfy them.

Input goes through the automation channel the port already has:
%APPDATA%\MarioPartyRD\Party Board\audio_diagnostics.enable, holding
"counter port buttons frames stickX stickY". One command is latched per poll,
so the four ports are written a frame or more apart.

.EXAMPLE
tools\test_raytracing.ps1 -Target board
tools\test_raytracing.ps1 -Target minigame -RayTracing off
#>
param(
    [ValidateSet('board', 'minigame')][string]$Target = 'board',
    [ValidateSet('on', 'off', 'ao', 'shadows', 'reflections')][string]$RayTracing = 'on',
    [string]$BinaryDirectory = 'build/windows-msvc-relwithdebinfo',
    [string]$OutputDirectory = 'work/raytracing',
    [int]$BootSeconds = 25,
    [int]$MaxSteps = 60,
    [int]$Frames = 4,
    [int]$Attempts = 3,
    # How many cards right of the first board to take on the selection carousel.
    [int]$BoardIndex = 0,
    # How far down the mini-game list to go before confirming. The list is
    # vertical, unlike the board carousel.
    [int]$MinigameIndex = 0,
    # Steps of actual play once the scene is up. Loading a board is not playing
    # one: the intro fly-through carries no interface, so the 2D mask is never
    # exercised until a turn starts and the HUD appears.
    [int]$PlaySteps = 0
)

$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent

function Resolve-TestPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectPath $path))
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class RtCapture {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  // The mode select is a fan of cards under a coloured banner. Each mode's
  // banner is a different hue -- Party pink, Story blue -- so the test is
  // saturation across the top centre band, not hue. Returned as a percentage.
  public static int BannerScore(Bitmap b) {
    int n = 0, tot = 0;
    int x0 = (int)(b.Width * 0.27), x1 = (int)(b.Width * 0.73);
    int y0 = (int)(b.Height * 0.10), y1 = (int)(b.Height * 0.19);
    for (int y = y0; y < y1; y += 2)
      for (int x = x0; x < x1; x += 2) {
        Color c = b.GetPixel(x, y);
        int mx = Math.Max(c.R, Math.Max(c.G, c.B));
        int mn = Math.Min(c.R, Math.Min(c.G, c.B));
        if (mx > 90 && mx - mn > 70) n++;
        tot++;
      }
    return tot == 0 ? 0 : (100 * n) / tot;
  }
  // Fraction of a box that is both bright and saturated, as a percentage.
  static double Frac(Bitmap b, double x0, double y0, double x1, double y1, int vmin, int smin) {
    int n = 0, tot = 0;
    for (int y = (int)(b.Height * y0); y < (int)(b.Height * y1); y += 2)
      for (int x = (int)(b.Width * x0); x < (int)(b.Width * x1); x += 2) {
        Color c = b.GetPixel(x, y);
        int mx = Math.Max(c.R, Math.Max(c.G, c.B));
        int mn = Math.Min(c.R, Math.Min(c.G, c.B));
        if (mx > vmin && mx - mn > smin) n++;
        tot++;
      }
    return tot == 0 ? 0 : (100.0 * n) / tot;
  }
  // The board carousel is the one screen carrying both a large colourful board
  // preview and a coloured board name in its description box. Either alone also
  // matches something else -- the character select is just as colourful, the
  // mode select's description is also coloured -- so both are required. Across
  // 32 captured frames of that overlay the pair selects exactly one.
  // The mini-game list: a bright banner across the top and a preview panel on
  // the right. The character select shares the banner but not the panel, so
  // both are needed.
  //
  // These fractions are of the whole window, title bar included, which is what
  // every Frac call here measures. Thresholds tuned against images cropped to
  // the client area do not transfer: the first pair, 60 and 60, came from
  // cropped frames and never fired once, because the same screen scores 46.5
  // and 70.8 uncropped.
  public static bool IsMinigameList(Bitmap b) {
    return Frac(b, 0.27, 0.10, 0.73, 0.19, 90, 70) > 40.0
        && Frac(b, 0.55, 0.24, 0.85, 0.50, 90, 40) > 60.0;
  }
  public static bool IsBoardCarousel(Bitmap b) {
    return Frac(b, 0.20, 0.15, 0.60, 0.42, 90, 70) > 30.0
        && Frac(b, 0.02, 0.79, 0.62, 0.93, 120, 60) > 3.0;
  }
}
"@ -ReferencedAssemblies System.Drawing

$binary = Resolve-TestPath $BinaryDirectory
$exe = Join-Path $binary 'partyboard.exe'
if (-not (Test-Path $exe)) { throw "partyboard.exe not found in $binary" }

$output = Resolve-TestPath $OutputDirectory
New-Item -ItemType Directory -Force -Path $output | Out-Null
$logPath = Join-Path $output "$Target-$RayTracing.log"
$channel = Join-Path $env:APPDATA 'MarioPartyRD\Party Board\audio_diagnostics.enable'
$configPath = Join-Path $env:APPDATA 'MarioPartyRD\Party Board\config.json'

# --- ray tracing settings -----------------------------------------------------
# The engine's master switch is the OR of the three terms, so "off" means all
# three off rather than a separate flag.
$terms = switch ($RayTracing) {
    'on'          { @($true,  $true,  $true) }
    'off'         { @($false, $false, $false) }
    'ao'          { @($true,  $false, $false) }
    'shadows'     { @($false, $true,  $false) }
    'reflections' { @($false, $false, $true) }
}
if (-not (Test-Path $configPath)) { throw "config.json not found at $configPath; run the game once first" }
$config = Get-Content $configPath -Raw | ConvertFrom-Json
$keys = @('video.enableRayTracedAo', 'video.enableRayTracedShadows', 'video.enableRayTracedReflections')
for ($i = 0; $i -lt 3; $i++) {
    $config | Add-Member -NotePropertyName $keys[$i] -NotePropertyValue $terms[$i] -Force
}
$config | ConvertTo-Json | Set-Content -Path $configPath -Encoding utf8
Write-Host "ray tracing: AO=$($terms[0]) shadows=$($terms[1]) reflections=$($terms[2])"

# --- input --------------------------------------------------------------------
$START = 0x1000
$A = 0x100
$script:counter = 0

function Send-Pad([int]$Buttons, [int]$StickX = 0, [int]$StickY = 0, [int]$Frames = 0, [int]$Port = 0) {
    # A stick deflection lasts one frame: the menus re-read it every frame it is
    # held and would move the cursor once per frame. Buttons come through
    # HuPadBtnDown and need the longer pulse to be caught at all. A caller that
    # passes a count means it -- the d-pad in these menus is polled like the
    # stick, not edge detected, so it wants one frame too.
    $frames = if ($Frames -gt 0) { $Frames } elseif ($StickX -ne 0 -or $StickY -ne 0) { 1 } else { 8 }
    $script:counter++
    $line = '{0} {1} {2:x} {3} {4} {5}' -f $script:counter, $Port, $Buttons, $frames, $StickX, $StickY
    for ($try = 0; $try -lt 5; $try++) {
        try { Set-Content -Path $channel -Value $line -Encoding ascii -ErrorAction Stop; return }
        catch { Start-Sleep -Milliseconds 120 }
    }
}

function Send-PadAllPorts([int]$Buttons, [int]$StickX = 0, [int]$StickY = 0) {
    foreach ($port in 0..3) { Send-Pad $Buttons $StickX $StickY 0 $port; Start-Sleep -Milliseconds 70 }
}

function Get-CurrentOverlay {
    $m = Select-String -Path $logPath -Pattern 'Link DLL:(\S+)' -ErrorAction SilentlyContinue
    if ($m) { return $m[-1].Matches[0].Groups[1].Value }
    return '?'
}

function Save-Frame([System.IntPtr]$Window, [string]$Name) {
    $r = New-Object RtCapture+RECT
    if (-not [RtCapture]::GetWindowRect($Window, [ref]$r)) { return -1 }
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -lt 16 -or $h -lt 16) { return -1 }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $score = [RtCapture]::BannerScore($bmp)
    $script:onCarousel = [RtCapture]::IsBoardCarousel($bmp)
    $script:onMinigameList = [RtCapture]::IsMinigameList($bmp)
    if ($Name) { $bmp.Save((Join-Path $output "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $g.Dispose(); $bmp.Dispose()
    return $score
}

# Party is the leftmost card, Story the next, Mini-Game the one after that.
$cardsRight = if ($Target -eq 'minigame') { 2 } else { 0 }
# Party setup, restarted whenever the overlay changes so the sequence cannot
# drift out of phase with however long the file select took.
$setup = @(
    @($START, 0, 0), @($A, 0, 0), @(0, 0, -100), @($A, 0, 0), @($A, 0, 0),
    @(0, 100, 0), @($A, 0, 0), @($START, 0, 0), @($A, 0, 0), @(0, 0, 100),
    @($A, 0, 0), @($A, 0, 0), @($A, 0, 0), @($A, 0, 0), @($A, 0, 0)
)

function Invoke-Run {
    Remove-Item $channel, $logPath -ErrorAction SilentlyContinue
    $env:AURORA_RT_DEBUG = '1'
    $process = Start-Process -FilePath $exe -WorkingDirectory $binary -PassThru `
        -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err"
    Start-Sleep -Seconds $BootSeconds
    if ($process.HasExited) { return @{ Reached = $false; Reason = 'exited during boot' } }

    $window = $process.MainWindowHandle
    $index = 0; $moved = 0; $boards = 0; $games = 0; $previous = ''; $reached = $false; $scene = ''
    $script:onCarousel = $false
    $script:onMinigameList = $false
    foreach ($step in 1..$MaxSteps) {
        $overlay = Get-CurrentOverlay
        if ($overlay -ne $previous) {
            $previous = $overlay; $index = 0
            # A freshly linked overlay is not ready for input; a press sent into
            # that gap is lost, and losing one shifts everything after it.
            Start-Sleep -Seconds 3
        }
        $banner = Save-Frame $window ('step{0:d3}_{1}' -f $step, ($overlay -replace '\.dll', ''))
        if ($overlay -match '^w\d' -or $overlay -match '^m\d') { $reached = $true; $scene = $overlay; break }

        if ($overlay -match 'boot') { Send-Pad $START }
        elseif ($overlay -match 'modesel') {
            if ($banner -ge 50 -and $moved -lt $cardsRight) { $moved++; Send-Pad 0 100 0 }
            else { Send-Pad $A }
        }
        elseif ($script:onMinigameList -and $games -lt $MinigameIndex) {
            $want = $MinigameIndex - $games
            Write-Host "  mini-jeu : $want crans vers le bas"
            # Stick and d-pad together, one frame each. The menus in this
            # overlay test both (HuPadStkY <= -5 || HuPadBtn & 4 in
            # mgmodedll/main.c) and neither is edge detected, so a longer pulse
            # would move as many notches as frames it is held. Three notches of
            # stick alone left the cursor on the first entry, so the list is
            # reading the d-pad.
            foreach ($i in 1..$want) { Send-Pad 0x0004 0 -100 1; Start-Sleep -Milliseconds 300 }
            $games = $MinigameIndex
        }
        elseif ($script:onCarousel -and $boards -lt $BoardIndex) {
            # All the moves in one visit. The carousel only stays up for about
            # two steps, so taking one card per visit put a ceiling of two on how
            # far right the sweep could ever reach. Each deflection is its own
            # one frame pulse, spaced so the game latches them separately.
            $want = $BoardIndex - $boards
            Write-Host "  plateau : $want cartes vers la droite"
            foreach ($i in 1..$want) { Send-Pad 0 100 0; Start-Sleep -Milliseconds 250 }
            $boards = $BoardIndex
        }
        else { $e = $setup[$index % $setup.Count]; Send-PadAllPorts $e[0] $e[1] $e[2]; $index++ }

        Start-Sleep -Milliseconds 1500
        if ($process.HasExited) { return @{ Reached = $false; Reason = "process died at step $step after $overlay" } }
    }

    if ($reached) {
        Start-Sleep -Seconds 4
        foreach ($k in 1..$Frames) { Save-Frame $window ('scene{0:d2}' -f $k) | Out-Null; Start-Sleep -Milliseconds 900 }
    }
    if ($reached -and $PlaySteps -gt 0) {
        Write-Host "  jeu en cours, $PlaySteps etapes"
        foreach ($k in 1..$PlaySteps) {
            if ($process.HasExited) { Write-Host "  *** mort pendant le jeu, etape $k"; break }
            Save-Frame $window ('play{0:d3}' -f $k) | Out-Null
            # A on every port clears the dialogues the turn starts with; the
            # occasional stick nudge moves whatever cursor is up.
            if ($k % 4 -eq 0) { Send-Pad 0 0 -100 } else { Send-PadAllPorts $A }
            Start-Sleep -Milliseconds 1200
        }
    }
    $died = $process.HasExited
    if (-not $died) { $process.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 3 }
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Remove-Item $channel -ErrorAction SilentlyContinue
    return @{ Reached = $reached; Scene = $scene; DiedInScene = $died; Reason = 'max steps reached' }
}

# The menu sequence is timing sensitive and misses roughly one run in three.
$result = $null
foreach ($attempt in 1..$Attempts) {
    $result = Invoke-Run
    if ($result.Reached) { Write-Host "reached $($result.Scene) on attempt $attempt"; break }
    Write-Host "attempt $attempt did not reach a $Target ($($result.Reason))"
}
if (-not $result.Reached) { throw "never reached a $Target in $Attempts attempts" }
if ($result.DiedInScene) { throw "the game died inside $($result.Scene)" }

# --- report -------------------------------------------------------------------
# Only the lines from the scene itself: the menus before it say nothing about
# how the settings behave where the game is played.
$lines = Get-Content $logPath
$from = ($lines | Select-String -Pattern "Link DLL:$($result.Scene)" | Select-Object -Last 1).LineNumber
$scene = $lines[($from - 1)..($lines.Count - 1)]

Write-Host ''
Write-Host "--- $($result.Scene), ray tracing $RayTracing ---"
foreach ($pattern in @('Ray tracing active', 'Composition ran', 'Perspective projections',
                       'Scene extent', 'Of \d+ captured draws', 'Positions rejected', 'still running when')) {
    $hit = $scene | Select-String -Pattern $pattern | Select-Object -Last 1
    if ($hit) { Write-Host ("  " + ($hit.ToString() -replace '^\[INFO \| aurora::rt\] ', '')) }
}

# Scene bounds. The occlusion radius and the shadow range are derived from
# them, so a non-finite or absurd extent means rays with a NaN or runaway reach.
# That happened on m402Dll and m405Dll and passed this script regardless: it
# only ever looked for ERROR lines, and the last report printed above can be a
# clean one while earlier reports were not. So every report is checked.
$extentLines = @($scene | Select-String -Pattern 'Scene extent (\S+) x (\S+) x (\S+);')
$nonFinite = 0
$absurd = 0
foreach ($line in $extentLines) {
    $groups = $line.Matches[0].Groups
    $values = @($groups[1].Value, $groups[2].Value, $groups[3].Value)
    if (($values -join ' ') -match 'nan|inf') { $nonFinite++; continue }
    if (@($values | Where-Object { [double]$_ -gt 1e6 }).Count -gt 0) { $absurd++ }
}
Write-Host ("  bornes de scene : {0} non finies et {1} absurdes sur {2} rapports" -f $nonFinite, $absurd, $extentLines.Count)

# One composition per frame. The 2D mask this used to report on is gone. A
# second eligible pass is the direct signature of the term being applied twice;
# the count per report is the coarse one -- reports come every 300 frames, and
# the double application measured 600.
$compositeLines = @($scene | Select-String -Pattern 'Composition ran (\d+) time\(s\) since the last report; (\d+) further passes')
$compositeMax = 0
$compositeExtra = 0
foreach ($line in $compositeLines) {
    $count = [int]$line.Matches[0].Groups[1].Value
    if ($count -gt $compositeMax) { $compositeMax = $count }
    # Cumulative in the engine, so the last report carries the total.
    $compositeExtra = [int]$line.Matches[0].Groups[2].Value
}
Write-Host ("  composition : au plus {0} par rapport de 300 frames, {1} passes eligibles en trop" -f $compositeMax, $compositeExtra)

$failures = @()
$errors = ($scene | Select-String -Pattern 'aurora::rt.*ERROR|device removed|DEVICE_HUNG').Count
if ($errors -gt 0) { $failures += "$errors ray tracing errors" }
if ($nonFinite -gt 0) { $failures += "$nonFinite reports with non-finite scene bounds" }
if ($absurd -gt 0) { $failures += "$absurd reports with scene bounds past 1e6" }
if ($compositeExtra -gt 0) { $failures += "$compositeExtra passes eligible for a second composition" }
if ($compositeMax -gt 450) { $failures += "composition ran $compositeMax times in one 300-frame report" }
Write-Host ''
if ($failures.Count -gt 0) { throw "$($result.Scene): $($failures -join '; ')" }
Write-Host "PASS: $($result.Scene) rendered with ray tracing $RayTracing, no errors."
Write-Host "Frames and log in $output"
