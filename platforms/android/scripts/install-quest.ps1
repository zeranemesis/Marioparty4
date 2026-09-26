# Installs or updates Party Board on a Meta Quest from a Windows PC
# (install-quest.cmd runs it). Once installed, the headset finds and installs
# the next builds by itself (quest/QuestUpdater.java).
#
#   install-quest.ps1            the latest build from GitHub (quest-update.json)
#   install-quest.ps1 -Apk x.apk a local build instead
#
# Needs the headset plugged in with developer mode on. adb comes from the
# Android SDK when there is one, otherwise Google's platform-tools are
# downloaded once into %LOCALAPPDATA%\PartyBoard\quest.

param([string]$Apk)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is much slower with its progress bar
$Release = 'https://github.com/zeranemesis/Marioparty4/releases/download/partyboard-android-latest'
$Package = 'com.mariopartyrd.partyboard'
$Work = Join-Path $env:LOCALAPPDATA 'PartyBoard\quest'
New-Item -ItemType Directory -Force $Work | Out-Null

function Find-Adb {
    $candidates = @()
    if ($env:ANDROID_HOME) { $candidates += Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe' }
    $candidates += Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
    $onPath = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    $candidates += Join-Path $Work 'platform-tools\adb.exe'
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    Write-Host 'Téléchargement des outils Android (adb)...'
    $zip = Join-Path $Work 'platform-tools.zip'
    Invoke-WebRequest 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath $Work -Force
    Remove-Item $zip
    return Join-Path $Work 'platform-tools\adb.exe'
}

function Get-Headset($adb) {
    & $adb start-server | Out-Null
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $lines = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\S' }
        $ready = $lines | Where-Object { $_ -match '\tdevice$' }
        if ($ready) { return ($ready | Select-Object -First 1).Split("`t")[0] }
        if ($attempt -eq 0) {
            if ($lines | Where-Object { $_ -match 'unauthorized' }) {
                Write-Host 'Mets le casque : accepte « Autoriser le débogage USB » (coche « Toujours autoriser »).'
            } else {
                Write-Host 'Branche le Quest en USB (mode développeur activé)...'
            }
        }
        Start-Sleep -Seconds 2
    }
    throw 'Aucun casque trouvé.'
}

$adb = Find-Adb
$serial = Get-Headset $adb
$model = (& $adb -s $serial shell getprop ro.product.model).Trim()
Write-Host "Casque : $model"

if (-not $Apk) {
    $manifest = Invoke-RestMethod "$Release/quest-update.json"
    Write-Host "Dernière version : $($manifest.version)"
    if (-not $manifest.downloadUrl.StartsWith("$Release/")) { throw "Lien inattendu : $($manifest.downloadUrl)" }
    $Apk = Join-Path $Work 'PartyBoard-quest.apk'
    Write-Host 'Téléchargement...'
    Invoke-WebRequest $manifest.downloadUrl -OutFile $Apk
    $hash = (Get-FileHash $Apk -Algorithm SHA256).Hash
    if ($hash -ne $manifest.sha256.ToUpper()) { throw "Fichier corrompu (SHA-256 $hash)." }
}

Write-Host 'Installation (les sauvegardes sont conservées)...'
# adb reports failures on stderr, which Windows PowerShell turns into a
# terminating error under 'Stop': read its output instead.
$ErrorActionPreference = 'Continue'
$output = & $adb -s $serial install -r $Apk 2>&1 | Out-String
$ErrorActionPreference = 'Stop'
if ($output -notmatch 'Success') {
    if ($output -match 'UPDATE_INCOMPATIBLE') {
        Write-Host "La version installée est signée par une autre clé. Il faut la désinstaller d'abord"
        Write-Host "(cela efface les sauvegardes du casque) : `"$adb`" -s $serial uninstall $Package"
    }
    throw "Échec de l'installation : $output"
}
& $adb -s $serial shell am start -n "$Package/.PartyBoardActivity" | Out-Null
Write-Host 'Party Board est installé et lancé dans le casque (Bibliothèque > Sources inconnues).'
