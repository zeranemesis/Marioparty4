@echo off
REM Prints the same folder fingerprint the lobby compares between the two PCs,
REM and checks it against the one recorded in LISEZ-MOI.txt. Run it on both
REM machines: the two lines must be identical or the game will refuse to start.
REM
REM PartyBoardOnline.exe is a GUI-subsystem program, so its output only reaches
REM us through an explicit redirection. Hence Start-Process, not a plain call.
pushd "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$root=(Get-Location).Path; $out=Join-Path $env:TEMP ('pb-hash-'+[guid]::NewGuid().ToString('N')+'.txt'); $exe=Join-Path $root 'PartyBoardOnline.exe'; if(-not (Test-Path -LiteralPath $exe)){Write-Host 'PartyBoardOnline.exe est introuvable. Ce fichier doit rester dans le dossier du jeu.' -ForegroundColor Red; exit 2}; $p=Start-Process -FilePath $exe -ArgumentList '--build-hash' -Wait -PassThru -NoNewWindow -RedirectStandardOutput $out; $actual=(Get-Content $out -Raw); if($actual){$actual=$actual.Trim()}; Remove-Item $out -Force -ErrorAction SilentlyContinue; $expected=''; $readme=Join-Path $root 'LISEZ-MOI.txt'; if(Test-Path -LiteralPath $readme){foreach($l in Get-Content $readme){if($l -match 'Empreinte\s+([0-9a-f]{64})'){$expected=$Matches[1]}}}; Write-Host ''; Write-Host ('Empreinte de ce dossier : '+$actual); Write-Host ('Empreinte attendue      : '+$expected); Write-Host ''; if(-not $actual){Write-Host 'Le calcul a echoue. Le dossier est incomplet.' -ForegroundColor Red; exit 2}; if(-not $expected){Write-Host 'LISEZ-MOI.txt est absent ou modifie : comparaison impossible.' -ForegroundColor Yellow}elseif($actual -eq $expected){Write-Host 'OK. Ce dossier est intact.' -ForegroundColor Green}else{Write-Host 'DIFFERENT. Ce dossier ne correspond pas au paquet livre : re-extrayez le zip.' -ForegroundColor Red}; Write-Host ''; Write-Host 'Comparez la premiere ligne avec celle affichee sur l autre PC.'; Write-Host 'Si elles different, le salon refusera de lancer la partie.'"
echo.
popd
pause
