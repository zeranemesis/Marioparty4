# Prototype PAD Host/Client — protocole 3

Cette étape corrige l'intégration du prototype existant à deux joueurs. Elle ne
constitue pas encore le mode complet à quatre joueurs demandé. Aucun serveur,
compte, service web ou rollback n'est nécessaire à ce prototype.

## Chemin des inputs

SDL -> PADRead(status[4]) -> PartyBoard_NetplayPreparePads -> PADClamp ->
traitement original des boutons/répétitions -> HuPadRead -> HuPrcCall.

Le Host est le joueur 1 (port logique 0), le Client le joueur 2 (port logique 1).
Sur chaque PC, `--netplay-pad 1` lit la manette physique configurée dans le
premier emplacement du launcher. Il ne faut PAS passer 2 simplement parce
qu'on est le joueur 2. Les ports physiques 1 à 4 sont sélectionnables.

Le réseau transmet les valeurs brutes, avant PADClamp. Les détections d'appui,
les répétitions de boutons, les directions analogiques et les seuils des
gâchettes sont ensuite calculés par le même code original des deux côtés.
Les vibrations du jeu sont routées vers la manette locale de son propriétaire.
Les ports 3/4 sont déconnectés dans ce premier prototype synchronisé.

Si l'input attendu manque, le tick ne modifie pas les compteurs PAD, les
répétitions ni l'appel msmSysRegularProc. La boucle de présentation reste
active. Le traitement UDP est non bloquant et plafonné à 64 paquets par appel.

Input delay par défaut : 3 ticks de simulation (environ 50 ms à 60 Hz).
Le tick N capture un input pour N + délai ; les premiers ticks utilisent un
input neutre. Les deux joueurs subissent le même délai. Pas de prédiction.

Le Host attend maintenant un premier paquet compatible dans le contexte
courant avant de valider même les premières frames neutres. Cette barrière
ne remplace pas encore une vérification du disque et de l'état initial.

La surveillance de session utilise l'horloge monotone : 30 secondes pour un
premier pair compatible, puis 10 secondes sans progression de simulation.
Ces délais commencent dans le jeu, pas pendant le choix du disque du launcher.
Les paquets répétés ne repoussent pas indéfiniment le délai d'une frame bloquée.
Une expiration ou une configuration incompatible arrête la simulation,
ferme le socket et coupe la vibration locale. Le réseau reste marqué actif
pour empêcher une reprise silencieuse en solo. Une notification d'erreur est
prévue dans l'overlay existant, avec une raison détaillée dans la console.
Il faut fermer puis relancer la session pour reconnecter les joueurs.

## Transport et compatibilité

Le transport existant Winsock/POSIX est conservé, sans nouvelle dépendance.
Protocole 3 : 48 octets, champs sérialisés explicitement en big-endian, jamais
un memcpy de PADStatus. Il utilise encore le format INPUT du prototype v2,
avec une version différente parce que les valeurs sont maintenant brutes.
Les anciennes versions sont rejetées.

Une fois le pair fixé, les paquets d'autres adresses/ports sont rejetés.
Cela n'est PAS une authentification cryptographique. Le handshake complet,
l'identifiant de session aléatoire et les contrôles de compatibilité du disque
et du build restent à implémenter avant de recommander l'exposition Internet.

## Compilation et tests exécutables

Fichiers créés : `include/port/netplay_pad.hpp`, `tools/build_local.ps1`,
`tools/test_netplay_pad.ps1`, `tools/tests/netplay_pad_tick_test.c` et ce guide.
La surveillance temporelle est séparée dans `include/port/netplay_progress.hpp`.

Fichiers modifiés : `src/game/pad.c`, `include/game/pad.h`, `src/game/main.c`,
`src/port/netplay_runtime.cpp`, `include/port/netplay_runtime.h`,
`src/port/netplay_transport.cpp`, `include/port/netplay_transport.hpp`,
`src/port/entry.cpp` et `dol.def`. Les fichiers de build incluaient déjà les
sources réseau ; aucun ajout de dépendance à CMake n'était nécessaire.

Depuis la racine de `work/partyboard-audio-local`, avec PowerShell 7 et le
dossier CMake déjà configuré :

```powershell
./tools/build_local.ps1
./tools/test_netplay_pad.ps1
./build/aexp/RelWithDebInfo/partyboard.exe --netplay-self-test
```

Le script de build exécute :

```text
cmake --build build/aexp --config RelWithDebInfo --target partyboard --parallel 4 -- /verbosity:quiet /nologo
```

Il déduplique Path/PATH dans l'environnement enfant pour éviter une erreur
MSBuild du lanceur, sans changer l'environnement système de l'utilisateur.

Le test PAD compile le vrai src/game/pad.c avec des doubles matériel/réseau.
Il compare les ports local/distant pendant les appuis, maintiens et relâchements
et vérifie le gel des compteurs durant des attentes réseau : 1 161 assertions.

Le test interprocessus lance deux vrais partyboard.exe en mode sans fenêtre,
avec UDP localhost et inputs synthétiques. Il vérifie 600 ticks par processus
aux délais 0, 3 et 8, en lisant le port physique 4 du Client et en l'injectant
dans son port logique 2. Il vérifie boutons, deux sticks, deux gâchettes et les
ports inoccupés. Le test n'ouvre pas de disque ou de sauvegarde et ne lance
pas une partie graphique. Il ne prouve pas le déterminisme complet du jeu.

Le scénario de perte de pair coupe le socket du Client à la frame 60 sans
message de déconnexion. Le Host doit épuiser uniquement les inputs déjà reçus,
passer en erreur après 10 secondes sans progression et refuser 100 tentatives
supplémentaires de simulation. Le self-test runtime vérifie également les
seuils temporels avec une horloge simulée, les doublons et l'état terminal.

## Test graphique sur le même PC

Utiliser les mêmes exécutables/DLL et la même version du disque. Configurer
les manettes physiques dans le launcher ; activer les inputs en arrière-plan
et désactiver la pause sur perte de focus pour tester deux fenêtres.

Depuis le dossier `build/aexp/RelWithDebInfo` :

```powershell
# Première fenêtre (Host)
./partyboard.exe --netplay-host 34197 --netplay-pad 1 --netplay-delay 3 --netplay-full
# Deuxième fenêtre (Client)
./partyboard.exe --netplay-join 127.0.0.1:34197 --netplay-pad 2 --netplay-delay 3 --netplay-full
```

Ici le Client lit volontairement la DEUXIÈME manette physique du même PC.
Pour deux PC distincts, chacun utilise normalement `--netplay-pad 1`.
Vérifier que chaque manette pilote uniquement le bon joueur. Tester un appui
bref, un maintien dans un menu, un relâchement et un mini-jeu. Les profils de
configuration/sauvegarde peuvent encore être partagés entre deux instances
sur le même compte Windows : conserver une copie des sauvegardes pour les
tests graphiques. Le test automatique n'accède pas à ces fichiers.

## Test LAN sur deux PC

Host : même commande que ci-dessus. Client :

```powershell
./partyboard.exe --netplay-join 192.168.1.50:34197 --netplay-pad 1 --netplay-delay 3 --netplay-full
```

Remplacer l'adresse d'exemple par l'IPv4 locale du Host (affichée par ipconfig).
Autoriser l'application en UDP sur le réseau privé dans le pare-feu Windows
si nécessaire. Le LAN n'a pas besoin d'Internet ou de redirection sur la box.
Ce test sur deux machines physiques reste à effectuer.

## Limites et suite

- Deux joueurs seulement ; le modèle de lobby quatre places est encore séparé.
- Les transitions inter-overlays conservent l'ancien mécanisme de rattrapage
  local et de remise à zéro des frames/RNG ; elles ne sont pas validées par
  le test d'inputs et ne garantissent pas un lockstep strict de toute la partie.
- La perte de pair mène désormais à une erreur terminale. Un paquet DISCONNECT
  explicite, une sortie coordonnée et une reconnexion sans relancer le jeu
  restent à intégrer.
- Aucun hash d'état déterministe complet du jeu n'est encore comparé.
- Pas d'ENet, de miniUPnPc, de redirection automatique ou de découverte d'IP
  publique dans cette étape. Connexion directe IP/port uniquement.
- analogA/analogB et extButton ne sont pas transmis ; le traitement HuPad
  inspecté ne les consomme pas. Une autre utilisation directe de PADRead
  demanderait un élargissement du format.

Suite recommandée : session Host à trois pairs maximum, HELLO/attribution/
barrière de démarrage, frames continues, vérification build/disque et RNG,
déconnexion explicite. Ensuite miniUPnPc indépendant de la boucle du jeu,
avec repli IP/port lorsque la box ne permet pas de créer la redirection.

Les modifications de cette étape et les résultats sont locaux. Le ZIP
précédemment publié sur GitHub contient une autre version.
