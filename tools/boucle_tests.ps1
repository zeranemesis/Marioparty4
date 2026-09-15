# Runs a campaign over and over, without end, until it is told to stop.
#
# A campaign finishes when its scenarios are done. That is the wrong shape for a
# night, or a week: every finished pass leaves the machine idle. This wraps the
# campaign in a loop so the tests keep going by themselves.
#
# What it does NOT do, and will not do: keep a game alive across a divergence.
# When two peers stop agreeing, the run is over - continuing would mean deciding
# that one of them is right, and this repository does not resynchronise to keep
# going. So the loop restarts a session only after a session has genuinely
# ended. Making the game run longer is the job of the budget and of fixing the
# divergences, not of this script.
#
# Stopping it: create the file named by -StopFile (work/ARRET by default). The
# loop finishes the pass it is on and stops. Ctrl-C works too, but leaves the
# campaign it interrupted without a verdict.

param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$DiscPath,
    [string]$BinaryDirectory = 'build/aexp/RelWithDebInfo',
    [string]$Label = 'boucle',
    [string]$HookTickGate = '',
    [string]$MemFill = '',
    [string]$StopFile = 'work/ARRET',
    # 0 = no limit. A number of passes, for a bounded experiment.
    [int]$MaxPasses = 0
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$stop = Join-Path $root $StopFile
$campaign = Join-Path $PSScriptRoot 'netplay_campaign.ps1'

if (Test-Path -LiteralPath $stop) {
    Write-Output "Le fichier d'arret $stop existe deja : rien ne demarre. Supprime-le pour lancer la boucle."
    exit 2
}

$pass = 0
while ($true) {
    $pass++
    if ($MaxPasses -gt 0 -and $pass -gt $MaxPasses) {
        Write-Output ("[{0}] passe {1} : limite de {2} atteinte, arret." -f (Get-Date -Format 'HH:mm:ss'), $pass, $MaxPasses)
        break
    }
    if (Test-Path -LiteralPath $stop) {
        Write-Output ("[{0}] arret demande : {1}" -f (Get-Date -Format 'HH:mm:ss'), $stop)
        break
    }

    Write-Output ("[{0}] passe {1} : debut" -f (Get-Date -Format 'HH:mm:ss'), $pass)
    # Its own process, so a campaign that dies on a harness failure cannot take
    # the loop with it - which is exactly what happened on 2026-09-12.
    #
    # Built as a list, and an empty switch is left OUT rather than passed as an
    # empty string: powershell -File drops the value of "-X ''" and the callee
    # then refuses to start on a missing argument.
    $callArgs = @(
        "-NoProfile", "-File", $campaign,
        "-DiscPath", $DiscPath,
        "-Manifest", $Manifest,
        "-BinaryDirectory", $BinaryDirectory,
        "-Label", ("{0}-p{1}" -f $Label, $pass)
    )
    if ($HookTickGate) { $callArgs += @("-HookTickGate", $HookTickGate) }
    if ($MemFill) { $callArgs += @("-MemFill", $MemFill) }
    & powershell $callArgs
    $code = $LASTEXITCODE
    Write-Output ("[{0}] passe {1} : fin, code {2}" -f (Get-Date -Format 'HH:mm:ss'), $pass, $code)

    # A campaign that fails instantly, over and over, would spin the disk and
    # fill the results directory with nothing. Give it a moment and say so.
    if ($code -ne 0) {
        Write-Output ("[{0}] passe {1} terminee anormalement ; pause de 30 s avant la suivante." -f (Get-Date -Format 'HH:mm:ss'), $pass)
        Start-Sleep -Seconds 30
    }
}
Write-Output ("[{0}] boucle terminee apres {1} passe(s)." -f (Get-Date -Format 'HH:mm:ss'), $pass)
