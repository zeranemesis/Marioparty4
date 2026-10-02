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
    # The disc image to boot. Empty keeps the one the configuration remembers,
    # which is how this script has always run on a development machine; given,
    # it reaches the game through PARTYBOARD_DISC_IMAGE, the launcher's way of
    # naming the disc, and run_all_tests.ps1 passes it here like to any script
    # that needs a disc.
    [string]$DiscPath = '',
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
    # Which category of that list to take first, counted to the right of the
    # first one. The cursor stops at the bottom of a category without wrapping
    # into the next, so without this only the first category can be reached;
    # left and right switch it, wrapping around, and put the cursor back at the
    # top (free_play.c in mgmodedll).
    [int]$MinigameCategory = 0,
    # Steps of actual play once the scene is up. Loading a board is not playing
    # one: the intro fly-through carries no interface, so the 2D mask is never
    # exercised until a turn starts and the HUD appears.
    [int]$PlaySteps = 0,
    # A/B bench: "name=value" traces one frame of the reached scene twice, B
    # with that one setting changed, and compares the two buffers pixel for
    # pixel. The null test -- B set to what A already has -- must find no
    # differing pixel. See compare_raytracing_ab.ps1.
    [string]$AB = '',
    # Consecutive frames of the final ray traced output -- accumulation and
    # filter included -- written once the scene is up and measured with
    # measure_raytracing_sequence.ps1. What the A/B pair cannot show: it holds
    # the accumulation off. Not together with -AB.
    [int]$Sequence = 0,
    # Samples for a converged reference traced beside each sequence frame. The
    # error against it counts noise and lag together, where the spread from one
    # frame to the next counts noise and motion. Costly: every dumped frame is
    # traced twice, the reference at this many samples.
    [int]$SequenceReference = 0,
    # Seconds between reaching the scene and arming the pair or the sequence.
    # Four lets a board or a mini-game finish opening; less catches the opening
    # itself, cuts included.
    [double]$ArmDelaySeconds = 4,
    # One exact scene, such as m401Dll: any other attempt fails and is retried.
    # Two sequences only compare on the same scene; the target alone accepts any
    # board or any mini-game.
    [string]$Scene = '',
    # End-to-end frame period, as a distribution the game logs every 600
    # frames (AURORA_FRAME_STATS). Meaningless under vsync: pair it with
    # -Uncapped, which turns vsync off and asks for 240 FPS -- the ceiling the
    # frame pacer allows -- and puts both settings back once the game is gone.
    [switch]$FrameStats,
    [switch]$Uncapped,
    # A temporal upscaler, forced through AURORA_UPSCALER whatever the game's
    # settings say. "test" is the plumbing alone: a bilinear enlargement done on
    # the ray tracing device, which tells a fault of the hand-over between the
    # two devices from a fault of an SDK. Empty leaves the settings in charge.
    [ValidateSet('', 'none', 'test', 'fsr3', 'xess', 'dlss')][string]$Upscaler = '',
    # 0 native size (anti-aliasing only), 1 quality, 2 balanced, 3 performance,
    # 4 ultra performance; -1 leaves the setting alone.
    [int]$UpscaleQuality = -1,
    # Consecutive frames of the upscaler's input and output written once the
    # scene is up, and measured with measure_upscaler.py.
    [int]$UpscaleDump = 0,
    # -1 hands the SDK the opposite sub-pixel offset. On a still picture the
    # right sign is the sharper one.
    [int]$UpscaleJitterSign = 0,
    # -1 hands the SDK the motion the other way round. On frames in motion the
    # right direction is the one that keeps the picture sharp.
    [int]$UpscaleMotionSign = 0
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
# Vsync and the frame-rate cap are the user's own settings, unlike the ray
# tracing terms this script has always set: they are recorded here and put back
# after the last attempt. An interrupted run leaves them changed.
$savedVsync = $config.'video.enableVsync'
$savedFrameRate = $config.'video.targetFrameRate'
if ($Uncapped) {
    $config | Add-Member -NotePropertyName 'video.enableVsync' -NotePropertyValue $false -Force
    $config | Add-Member -NotePropertyName 'video.targetFrameRate' -NotePropertyValue 240 -Force
    Write-Host "cadence : vsync coupee, 240 FPS demandes (reglages d'origine : vsync=$savedVsync, cible=$savedFrameRate)"
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
    if ($DiscPath) { $env:PARTYBOARD_DISC_IMAGE = (Resolve-Path -LiteralPath $DiscPath).Path }
    # A stale pair from an earlier run would compare as though this one had
    # written it, so it goes before the game starts.
    Remove-Item (Join-Path $binary 'rt_ab_a.pfm'), (Join-Path $binary 'rt_ab_b.pfm'),
        (Join-Path $binary 'rt_ab_arm') -ErrorAction SilentlyContinue
    if ($AB) {
        $env:AURORA_RT_AB = $AB
    } else {
        Remove-Item Env:\AURORA_RT_AB -ErrorAction SilentlyContinue
    }
    Remove-Item (Join-Path $binary 'rt_seq_*.pfm') -ErrorAction SilentlyContinue
    if ($Sequence -gt 0) {
        $env:AURORA_RT_SEQUENCE = "$Sequence"
    } else {
        Remove-Item Env:\AURORA_RT_SEQUENCE -ErrorAction SilentlyContinue
    }
    if ($Sequence -gt 0 -and $SequenceReference -gt 0) {
        $env:AURORA_RT_SEQUENCE_REF = "$SequenceReference"
    } else {
        Remove-Item Env:\AURORA_RT_SEQUENCE_REF -ErrorAction SilentlyContinue
    }
    if ($FrameStats) {
        $env:AURORA_FRAME_STATS = '1'
    } else {
        Remove-Item Env:\AURORA_FRAME_STATS -ErrorAction SilentlyContinue
    }
    # The upscaler's overrides. Each is removed when not asked for: the
    # environment outlives a run, and a leftover would force the next one.
    Remove-Item (Join-Path $binary 'upscale_in_*.bmp'), (Join-Path $binary 'upscale_out_*.bmp'),
        (Join-Path $binary 'upscale_dump.txt'), (Join-Path $binary 'upscale_depth.raw'),
        (Join-Path $binary 'upscale_motion.raw') -ErrorAction SilentlyContinue
    foreach ($pair in @(@('AURORA_UPSCALER', $Upscaler),
                        @('AURORA_UPSCALE_QUALITY', $(if ($UpscaleQuality -ge 0) { "$UpscaleQuality" } else { '' })),
                        @('AURORA_UPSCALE_DUMP', $(if ($UpscaleDump -gt 0) { "$UpscaleDump" } else { '' })),
                        @('AURORA_UPSCALE_JITTER_SIGN', $(if ($UpscaleJitterSign -lt 0) { '-1' } else { '' })),
                        @('AURORA_UPSCALE_MOTION_SIGN', $(if ($UpscaleMotionSign -lt 0) { '-1' } else { '' })))) {
        if ($pair[1]) { Set-Item -Path "Env:\$($pair[0])" -Value $pair[1] }
        else { Remove-Item "Env:\$($pair[0])" -ErrorAction SilentlyContinue }
    }
    $process = Start-Process -FilePath $exe -WorkingDirectory $binary -PassThru `
        -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err"
    Start-Sleep -Seconds $BootSeconds
    if ($process.HasExited) { return @{ Reached = $false; Reason = 'exited during boot' } }

    $window = $process.MainWindowHandle
    $index = 0; $moved = 0; $boards = 0; $games = 0; $categories = 0; $previous = ''; $reached = $false; $found = ''
    $script:onCarousel = $false
    $script:onMinigameList = $false
    # Once the list has been recognised in an overlay, it stays recognised until
    # the overlay changes. The test reads the preview panel's colour, and some
    # previews are grey -- Mario Speedwagons is a road under a pale sky -- so a
    # frame of the list could stop matching, and the step fell through to the
    # generic cycle, whose push right changed the category.
    $onList = $false
    $listSteps = 0
    foreach ($step in 1..$MaxSteps) {
        $overlay = Get-CurrentOverlay
        if ($overlay -ne $previous) {
            $previous = $overlay; $index = 0; $onList = $false; $listSteps = 0
            # A freshly linked overlay is not ready for input; a press sent into
            # that gap is lost, and losing one shifts everything after it.
            Start-Sleep -Seconds 3
        }
        $banner = Save-Frame $window ('step{0:d3}_{1}' -f $step, ($overlay -replace '\.dll', ''))
        if ($script:onMinigameList) { $onList = $true }
        if ($onList) { $listSteps++ }
        if ($overlay -match '^w\d' -or $overlay -match '^m\d') {
            # Only the kind of scene asked for counts. The menus take a wrong
            # turn now and then, and a mini-game run that landed on w01Dll was
            # once measured and passed as though it were the mini-game.
            # Not $scene: PowerShell does not distinguish it from the $Scene
            # parameter, and assigning the overlay to it made the test below
            # compare the overlay with itself -- which always passed.
            $found = $overlay
            $wanted = if ($Scene) { '^' + [regex]::Escape($Scene) }
                      elseif ($Target -eq 'minigame') { '^m\d' } else { '^w\d' }
            $reached = $overlay -match $wanted
            break
        }

        if ($overlay -match 'boot') { Send-Pad $START }
        elseif ($overlay -match 'modesel') {
            if ($banner -ge 50 -and $moved -lt $cardsRight) { $moved++; Send-Pad 0 100 0 }
            else { Send-Pad $A }
        }
        elseif ($onList -and $listSteps -lt 2) {
            # Nothing on the first frame the list is recognised: it may still be
            # sliding in, and reads no input while it does. Moves sent then were
            # lost, the run believed them made, and it confirmed the first game
            # of the first category -- which is how a BATTLE run measured
            # m401Dll.
        }
        elseif ($onList -and $categories -lt $MinigameCategory) {
            # Before any move down the list: a change of category puts the
            # cursor back at the top. Right on the stick and the d-pad together,
            # one frame each, as for the moves down; the R trigger would do the
            # same, but the game reads it from the analog value, which the
            # automation channel does not carry. Each change slides the list in
            # over twenty frames, so the presses are spaced to land after it.
            $want = $MinigameCategory - $categories
            Write-Host "  mini-jeu : $want categories vers la droite"
            foreach ($i in 1..$want) { Send-Pad 0x0002 100 0 1; Start-Sleep -Milliseconds 900 }
            $categories = $MinigameCategory
        }
        elseif ($onList -and $games -lt $MinigameIndex) {
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
        else {
            # On the mini-game list, once its moves are made, the same cycle
            # without its stick deflections: a push right changed the category
            # and put the cursor on the first 1vs3 game, a push up or down moved
            # it one entry, which is how runs meant for the 4P list kept landing
            # on m416Dll. Only the sticks go -- pressing A alone on the list, on
            # one port or on four, left three runs out of four on the rules screen
            # without ever taking START, where this cycle's rhythm had passed
            # every time.
            $e = $setup[$index % $setup.Count]
            if ($onList) { Send-PadAllPorts $e[0] 0 0 } else { Send-PadAllPorts $e[0] $e[1] $e[2] }
            $index++
        }

        Start-Sleep -Milliseconds 1500
        if ($process.HasExited) { return @{ Reached = $false; Reason = "process died at step $step after $overlay" } }
    }

    if ($reached) {
        Start-Sleep -Milliseconds ([int]($ArmDelaySeconds * 1000))
        if ($AB -or $Sequence -gt 0 -or $UpscaleDump -gt 0) {
            # The game polls for this file and traces its A/B pair on the next
            # frame it sees it. A triangle threshold cannot pick the scene: the
            # title sequence alone crosses any threshold a board would.
            New-Item -ItemType File -Path (Join-Path $binary 'rt_ab_arm') -Force | Out-Null
            Start-Sleep -Seconds 3
            # The upscaler's dump is armed by the same file and reads every
            # frame back: wait for the last one rather than guess how long.
            if ($UpscaleDump -gt 0) {
                $lastDump = Join-Path $binary ('upscale_out_{0:d3}.bmp' -f ($UpscaleDump - 1))
                $deadline = (Get-Date).AddSeconds(15 + $UpscaleDump)
                while (-not (Test-Path $lastDump) -and (Get-Date) -lt $deadline -and -not $process.HasExited) {
                    Start-Sleep -Milliseconds 500
                }
                Start-Sleep -Seconds 1
            }
            # Each frame waits for its readback, twice over with a reference, and
            # how long that takes depends on the trace size and on the disk: an
            # estimate cut a 90-frame sequence at 80. Wait for the last file.
            if ($Sequence -gt 0) {
                $lastFrame = Join-Path $binary ('rt_seq_{0:d3}.pfm' -f ($Sequence - 1))
                $perFrame = if ($SequenceReference -gt 0) { 3 } else { 1 }
                $deadline = (Get-Date).AddSeconds(10 + $Sequence * $perFrame)
                while (-not (Test-Path $lastFrame) -and (Get-Date) -lt $deadline -and -not $process.HasExited) {
                    Start-Sleep -Milliseconds 500
                }
                Start-Sleep -Seconds 1
            }
        }
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
    Remove-Item $channel, (Join-Path $binary 'rt_ab_arm') -ErrorAction SilentlyContinue
    $reason = if ($found -and -not $reached) { "landed on $found" } else { 'max steps reached' }
    return @{ Reached = $reached; Scene = $found; DiedInScene = $died; Reason = $reason }
}

# The menu sequence is timing sensitive and misses roughly one run in three.
$result = $null
foreach ($attempt in 1..$Attempts) {
    $result = Invoke-Run
    if ($result.Reached) { Write-Host "reached $($result.Scene) on attempt $attempt"; break }
    Write-Host "attempt $attempt did not reach a $Target ($($result.Reason))"
}
if ($Uncapped) {
    # Re-read rather than reuse: the game may have written its config on exit.
    $restored = Get-Content $configPath -Raw | ConvertFrom-Json
    foreach ($pair in @(@('video.enableVsync', $savedVsync), @('video.targetFrameRate', $savedFrameRate))) {
        if ($null -eq $pair[1]) { $restored.PSObject.Properties.Remove($pair[0]) }
        else { $restored | Add-Member -NotePropertyName $pair[0] -NotePropertyValue $pair[1] -Force }
    }
    $restored | ConvertTo-Json | Set-Content -Path $configPath -Encoding utf8
}
if (-not $result.Reached) { throw "never reached a $Target in $Attempts attempts" }
if ($result.DiedInScene) { throw "the game died inside $($result.Scene)" }

# --- report -------------------------------------------------------------------
# Only the lines from the scene itself: the menus before it say nothing about
# how the settings behave where the game is played.
$lines = Get-Content $logPath
$from = ($lines | Select-String -Pattern "Link DLL:$($result.Scene)" | Select-Object -Last 1).LineNumber
$sceneLines = $lines[($from - 1)..($lines.Count - 1)]

Write-Host ''
Write-Host "--- $($result.Scene), ray tracing $RayTracing ---"
foreach ($pattern in @('Ray tracing active', 'Composition ran', 'Perspective projections',
                       'Scene extent', 'Of \d+ captured draws', 'Positions rejected', 'still running when')) {
    $hit = $sceneLines | Select-String -Pattern $pattern | Select-Object -Last 1
    if ($hit) { Write-Host ("  " + ($hit.ToString() -replace '^\[INFO \| aurora::rt\] ', '')) }
}

if ($FrameStats) {
    # The scene's reports only: the menus before it pace differently.
    $intervalLines = @($sceneLines | Select-String -Pattern 'Frame intervals over (\d+) frames: mean ([0-9.]+) ms, p50 ([0-9.]+), p95 ([0-9.]+), p99 ([0-9.]+), max ([0-9.]+); (\d+) over')
    if ($intervalLines.Count -eq 0) {
        Write-Host "  cadence : aucun rapport dans la scene (moins de 600 frames ?)"
    } else {
        foreach ($line in $intervalLines | Select-Object -Last 3) {
            $g = $line.Matches[0].Groups
            Write-Host ("  cadence : moyenne {0} ms, p50 {1}, p95 {2}, p99 {3}, max {4} ; {5} frames au-dela du double de la mediane" -f $g[2].Value, $g[3].Value, $g[4].Value, $g[5].Value, $g[6].Value, $g[7].Value)
        }
    }
}

# Scene bounds. The occlusion radius and the shadow range are derived from
# them, so a non-finite or absurd extent means rays with a NaN or runaway reach.
# That happened on m402Dll and m405Dll and passed this script regardless: it
# only ever looked for ERROR lines, and the last report printed above can be a
# clean one while earlier reports were not. So every report is checked.
$extentLines = @($sceneLines | Select-String -Pattern 'Scene extent (\S+) x (\S+) x (\S+);')
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
#
# A report is only printed on a frame that traced, so one that falls on a frame
# without geometry is skipped and the next covers 600 frames: 598 compositions
# then read as twice per frame and failed a run that composited once. The
# count is taken against the frames the report really covers, which the
# "end_frame calls" line of the same report gives.
$compositeLines = @($sceneLines | Select-String -Pattern 'Composition ran (\d+) time\(s\) since the last report; (\d+) further passes')
$frameLines = @($sceneLines | Select-String -Pattern 'end_frame calls (\d+),')
$compositeMax = 0
$compositeExtra = 0
# The first report of the scene counts from the last one before it, which is
# in the menus: without that one it was taken to cover 300 frames, and a first
# report covering 600 failed a run that composited once per frame (an uncapped
# run on the board, 592 compositions).
$frameBefore = $null
if ($from -gt 1) {
    $frameBefore = $lines[0..($from - 2)] | Select-String -Pattern 'end_frame calls (\d+),' | Select-Object -Last 1
}
for ($i = 0; $i -lt $compositeLines.Count; $i++) {
    $line = $compositeLines[$i]
    $count = [int]$line.Matches[0].Groups[1].Value
    $covered = 300
    if ($frameLines.Count -eq $compositeLines.Count) {
        if ($i -gt 0) {
            $covered = [int]$frameLines[$i].Matches[0].Groups[1].Value - [int]$frameLines[$i - 1].Matches[0].Groups[1].Value
        } elseif ($frameBefore) {
            $covered = [int]$frameLines[0].Matches[0].Groups[1].Value - [int]$frameBefore.Matches[0].Groups[1].Value
        }
        if ($covered -lt 300) { $covered = 300 }
    }
    $per300 = [int][math]::Round($count * 300.0 / $covered)
    if ($per300 -gt $compositeMax) { $compositeMax = $per300 }
    # Cumulative in the engine, so the last report carries the total.
    $compositeExtra = [int]$line.Matches[0].Groups[2].Value
}
Write-Host ("  composition : au plus {0} par rapport de 300 frames, {1} passes eligibles en trop" -f $compositeMax, $compositeExtra)

$failures = @()
if ($AB) {
    # Searched in the whole log, not only the scene: the first frame past the
    # triangle threshold can be a 3D menu, and then that is what was compared.
    $pairHit = $lines | Select-String -Pattern 'A/B pair written' | Select-Object -Last 1
    $pairA = Join-Path $binary 'rt_ab_a.pfm'
    $pairB = Join-Path $binary 'rt_ab_b.pfm'
    if ($pairHit -and (Test-Path $pairA) -and (Test-Path $pairB)) {
        $linkBefore = $lines[0..($pairHit.LineNumber - 1)] | Select-String -Pattern 'Link DLL:(\S+)' | Select-Object -Last 1
        $where = if ($linkBefore) { $linkBefore.Matches[0].Groups[1].Value } else { 'unknown' }
        Copy-Item $pairA, $pairB -Destination $output -Force
        Write-Host ''
        Write-Host ("  A/B, frame in {0}: {1}" -f $where, ($pairHit.ToString() -replace '^\[INFO \| aurora::rt\] ', ''))
        $abResult = & (Join-Path $PSScriptRoot 'compare_raytracing_ab.ps1') -A (Join-Path $output 'rt_ab_a.pfm') -B (Join-Path $output 'rt_ab_b.pfm')
        if ($abResult -and $abResult.Uniform) {
            $failures += "A/B pair written on a uniform frame in ${where}: nothing was hit, so the comparison proves nothing"
        }
    } else {
        $failures += "A/B pair requested ($AB) but not written after the scene was reached"
    }
}
if ($Sequence -gt 0) {
    $sequenceHit = $lines | Select-String -Pattern 'Sequence written' | Select-Object -Last 1
    $written = @(Get-ChildItem (Join-Path $binary 'rt_seq_[0-9]*.pfm') -ErrorAction SilentlyContinue | Sort-Object Name)
    $references = @(Get-ChildItem (Join-Path $binary 'rt_seq_ref_*.pfm') -ErrorAction SilentlyContinue | Sort-Object Name)
    if ($SequenceReference -gt 0 -and $references.Count -ne $Sequence) {
        $failures += "$($references.Count) references written for $Sequence frames"
    }
    if ($sequenceHit -and $written.Count -eq $Sequence) {
        $linkBefore = $lines[0..($sequenceHit.LineNumber - 1)] | Select-String -Pattern 'Link DLL:(\S+)' | Select-Object -Last 1
        $where = if ($linkBefore) { $linkBefore.Matches[0].Groups[1].Value } else { 'unknown' }
        $sequenceDir = Join-Path $output 'sequence'
        New-Item -ItemType Directory -Path $sequenceDir -Force | Out-Null
        $written | Copy-Item -Destination $sequenceDir -Force
        $references | Copy-Item -Destination $sequenceDir -Force
        Write-Host ''
        Write-Host ("  sequence, in {0}: {1}" -f $where, ($sequenceHit.ToString() -replace '^\[INFO \| aurora::rt\] ', ''))
        $sequenceResult = & (Join-Path $PSScriptRoot 'measure_raytracing_sequence.ps1') -Directory $sequenceDir
        if ($sequenceResult -and $sequenceResult.Uniform) {
            $failures += "sequence written where nothing was hit in ${where}: every frame is uniform, so it measures nothing"
        }
    } else {
        $failures += "sequence of $Sequence frames requested but $($written.Count) written after the scene was reached"
    }
}
if ($Upscaler -and $Upscaler -ne 'none') {
    Write-Host ''
    foreach ($pattern in @('Temporal upscaler: ', '\d+x\d+ to \d+x\d+, quality', 'frames upscaled',
                           'traced for the upscaler')) {
        $hit = $lines | Select-String -Pattern $pattern | Select-Object -Last 1
        if ($hit) { Write-Host ("  " + ($hit.ToString() -replace '^\[INFO \| aurora::\w+\] ', '')) }
    }
    # The host reports its first frame and then every 600th, so a short stay
    # in the scene may hold no report of its own: the whole log is searched,
    # and it is the dump (-UpscaleDump), written only for frames that went
    # through the upscaler, that proves the scene's own frames did.
    $upscaled = @($lines | Select-String -Pattern '(\d+) frames upscaled')
    if ($upscaled.Count -eq 0) {
        $failures += "upscaler $Upscaler asked for but no frame was reported upscaled"
    }
    $gaveUp = @($lines | Select-String -Pattern 'turned off for this session|cannot run here')
    if ($gaveUp.Count -gt 0) {
        $failures += "the upscaler did not run: $($gaveUp[-1].ToString() -replace '^\[\w+ \| aurora::\w+\] ', '')"
    }
}
if ($UpscaleDump -gt 0) {
    $dumped = @(Get-ChildItem (Join-Path $binary 'upscale_out_[0-9]*.bmp') -ErrorAction SilentlyContinue)
    if ($dumped.Count -eq $UpscaleDump -and (Test-Path (Join-Path $binary 'upscale_dump.txt'))) {
        $upscaleDir = Join-Path $output 'upscale'
        New-Item -ItemType Directory -Path $upscaleDir -Force | Out-Null
        Get-ChildItem (Join-Path $binary 'upscale_*') | Move-Item -Destination $upscaleDir -Force
        Write-Host ''
        & python (Join-Path $PSScriptRoot 'measure_upscaler.py') $upscaleDir
        if ($LASTEXITCODE -ne 0) { $failures += 'the upscaler dump could not be measured' }
    } else {
        $failures += "$UpscaleDump upscaler frames requested but $($dumped.Count) written after the scene was reached"
    }
}
# Errors go to the game's error stream, a file of its own beside the log, and
# are written "[ERROR | module]": the pattern this used to look for, in the log
# alone, could match neither. Nothing was missed in the meantime -- no run kept
# in work/ has such a line -- but nothing would have been caught either. The
# error stream has no scene markers, so the whole run is held to it.
$errorStream = @()
if (Test-Path "$logPath.err") { $errorStream = @(Get-Content "$logPath.err") }
$errors = @($errorStream | Select-String -Pattern '^\[(ERROR|FATAL) \| aurora::(rt|upscale)\]').Count
$errors += @($sceneLines + $errorStream | Select-String -Pattern 'device removed|DEVICE_HUNG|DEVICE_REMOVED').Count
if ($errors -gt 0) {
    $first = @($errorStream | Select-String -Pattern '^\[(ERROR|FATAL) \| aurora::(rt|upscale)\]' | Select-Object -First 1)
    $failures += "$errors ray tracing errors" + $(if ($first.Count -gt 0) { " (first: $($first[0]))" } else { '' })
}
if ($nonFinite -gt 0) { $failures += "$nonFinite reports with non-finite scene bounds" }
if ($absurd -gt 0) { $failures += "$absurd reports with scene bounds past 1e6" }
if ($compositeExtra -gt 0) { $failures += "$compositeExtra passes eligible for a second composition" }
if ($compositeMax -gt 450) { $failures += "composition ran $compositeMax times in one 300-frame report" }
Write-Host ''
if ($failures.Count -gt 0) { throw "$($result.Scene): $($failures -join '; ')" }
Write-Host "PASS: $($result.Scene) rendered with ray tracing $RayTracing, no errors."
Write-Host "Frames and log in $output"
