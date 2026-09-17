@echo off
REM Lance le salon avec la porte D23 active.
REM
REM La porte n execute les hooks de dessin que sur un tick de simulation. Sans
REM elle, c est le RENDU qui decide combien de particules meurent, et la
REM simulation tire cinq nombres au hasard pour chaque emplacement libere : une
REM machine qui affiche une image de moins tire cinq nombres de moins, et les
REM deux PC cessent de calculer la meme chose. C est ce qui a arrete la session
REM du 13 septembre a 11h05, frame 12493, dans le mini-jeu m405.
REM
REM LES DEUX MACHINES DOIVENT UTILISER CE RACCOURCI, ou aucune.
REM Si une seule l utilise, le jeu refuse la connexion et affiche
REM "session mismatch ... hook-gate". Ce refus est voulu : mieux vaut ne pas
REM partir que de jouer une partie dont on ne pourra rien conclure.
REM
REM Ceci est une EXPERIENCE, pas un correctif demontre. Elle peut aussi changer
REM l affichage. Pour revenir au comportement normal, utilisez simplement
REM "Jouer en ligne.cmd".
pushd "%~dp0"
set PARTYBOARD_HOOK_TICK_GATE=1
echo.
echo   Porte D23 ACTIVE pour cette session.
echo   Verifiez que l autre PC lance le meme raccourci.
echo.
start "" "PartyBoardOnline.exe"
popd
