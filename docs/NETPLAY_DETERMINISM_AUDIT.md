# Audit de déterminisme online — branche `audio-local`

Date : 2026-09-10. Build de référence : `build/aexp` (MSVC, `PARTYBOARD_EXPERIMENTAL_MUSYX_AUDIO=ON`).

Ce document est le résultat de la mission A→D : cartographie du pipeline online,
inventaire des sources de non-déterminisme, classement, vérification prioritaire.
Chaque point est adossé à un `fichier:ligne` vérifié dans l'arbre courant.

---

## 1. Cartographie du pipeline online

### 1.1 Boucle principale (une itération = une image présentée)

`src/game/main.c:164` → boucle `while (PartyBoard_IsRunning)`

```
aurora_update()                       événements SDL/fenêtre
retrace = VIGetRetraceCount()         main.c:201  (STUB → toujours 0)
aurora_begin_frame()
simulationTicks = frame_pacer_simulation_tick()   main.c:218
HuSysBeforeRender() / Hu3DPreProc()
HuSysVWaitSet(1)                      main.c:236
for (i = 0; i < simulationTicks; ++i) {
    si i > 0 : msmMusFdoutEnd(); GlobalCounter++;      main.c:242-244
    simulationAllowed = HuPadPollSimulationTick();      main.c:246
    si autorisé :
        HuPadRead();                                    main.c:249
        PartyBoard_RunGameLogicTick();                  main.c:251
            pfClsScr(); HuPrcCall(1); MGSeqMain();      main.c:89-91
        PartyBoard_NetplayCommitTick();                 main.c:252
    frame_pacer_commit_simulation_tick();               main.c:266
}
HuSysVWaitSet(simulatedTicks)         main.c:272   ← minimumVcount = 0 ou 1
Hu3DExec()                            main.c:275   (animations HSF)
WipeExecAlways()                      main.c:285   (transitions d'écran)
HuSysVWaitSet(previousVCount)         main.c:290
HuSysDoneRender(retrace)              main.c:303
si tick simulé : GlobalCounter++      main.c:316
frame_limiter()                       main.c:335
```

Point clé : **en netplay, `frame_pacer_simulation_tick()` renvoie toujours 1**
(`src/port/imgui.cpp:284` force `target_frame_rate() == 60`). Donc
`simulatedTicks ∈ {0, 1}` : 1 si le réseau accepte le tick, 0 s'il attend.
`GlobalCounter`, `minimumVcount`, `wipe->time` et les animations HSF sont
alignés sur le nombre de ticks **acceptés**, pas sur le nombre d'images. C'est
correct et déjà robuste au « framerate d'affichage différent » (test 14).

### 1.2 Chemin PAD → réseau

```
HuPadPollSimulationTick()                 src/game/pad.c:411
└─ PadReadSimulationTick()                src/game/pad.c:364
   ├─ PADRead(status)
   ├─ PartyBoard_NetplayPreparePads(...)  src/game/pad.c:370   ← porte réseau
   │     └─ PartyBoard_NetplayTick()      src/port/netplay_runtime.cpp:1009
   ├─ PadApplySimulationStatus(...)       clamp/edges/repeat/rumble
   ├─ msmSysRegularProc()                 src/game/pad.c:376
   └─ VCounter++                          src/game/pad.c:377
```

Si `PartyBoard_NetplayPreparePads` renvoie `false`, la fonction sort **avant**
`PadApplySimulationStatus`, `msmSysRegularProc` et `VCounter++` : aucun effet de
bord partiel. Bon.

Les entrées réseau sont des `PADStatus` bruts (avant `PADClamp`), converties par
`inputFromPad`/`padFromInput` (`include/port/netplay_pad.hpp`). Le calcul des
fronts/répétitions se fait une seule fois, après acceptation. Bon.

### 1.3 Machine à états lockstep

`src/port/netplay_runtime.cpp` — un seul `Runtime` global.

- Délai d'entrée `kDefaultInputDelay = 3`, max 8 (l.77-78).
- Historique d'entrées : 256 frames, anneau `frame % 256` (l.76).
- Capture : `storeInput(localHistory, frame + inputDelay, pad)` (l.1161) puis
  envoi immédiat du sample futur (l.1166).
- Application : `findInput(localHistory, frame)` + `findInput(remoteHistory, frame)`
  (l.1182-1185) ; sans les deux, le tick est refusé (l.1186-1206) avec demande de
  retransmission bornée à 10 Hz (l.1192-1197).
- Contradiction d'entrée sur une frame déjà acceptée ⇒ `failSession` (l.609-618).
- Après acceptation du tick : `captureCommittedState()` (l.505) calcule le hash
  canonique de la frame N-1 et l'envoie.
- `StateHistory` (`include/port/netplay_state.hpp`) compare **uniquement des
  frames contiguës** et signale la première divergence (`nextEqual`).
- Avance non validée bornée à 128 frames (`kStateLeadLimit`, l.11 + l.1147).
- Réparation d'états à 10 Hz (`serviceStateRepair`, l.445).
- Perte de pair : `SessionProgress` (2 min) ⇒ `failSession`, **jamais** de
  reprise offline silencieuse (l.209-219). Bon.

### 1.4 Transport

`src/port/netplay_transport.cpp` — UDP, paquets fixes de 88 octets, protocole v6,
trois types (`Input`, `Retransmit`, `State`). Sérialisation explicite en
big-endian, pas de `memcpy` de structure. Bon.

### 1.5 Rollback (expérimental)

`src/port/rollback.cpp` + `rollback_scene.cpp` + `rollback_audio.cpp`.
Snapshot = heaps SYSTEM/DATA + globals de scène + piles de coroutines HuPrc +
`.data` de l'overlay Windows courant + PAD + horloge + séquence.
Armé seulement si `--netplay-rollback`, et désarmé dès qu'une transition de
contexte ou un wipe rend le replay sans dessin impossible
(`PartyBoard_RollbackRenderCanReplayWithoutDraw`, netplay_runtime.cpp:1092).

---

## 2. Inventaire des sources de non-déterminisme

### CRITICAL — divergence garantie, cause racine démontrée

#### C1. Attentes sur l'état audio piloté par un thread temps réel

**Le point le plus grave du port.**

`extern/musyx/src/musyx/runtime/hw_pc.c:1631` — `salAudioThreadFunc` : un thread
SDL dédié (`SDL_THREAD_PRIORITY_HIGH`) appelle `userCallback()` = `snd_handle_irq`
(`hardware.c:121`) à la cadence de consommation du périphérique audio
(`SDL_GetAudioStreamQueued` / `SDL_Delay(1)`, l.1642-1672).

`snd_handle_irq` (`hardware.c:44`) appelle `streamHandle()` (`hardware.c:102`),
qui invoque le callback de remplissage de flux `msmStreamUpdateFunc`
(`src/msm/msmstream.c:372`) → `msmStreamData` → `msmStreamShutdown` → `slot->status = 0`.

Donc **`slot->status` est muté par le thread audio**, à un rythme temps réel.

Et la logique de jeu s'y bloque :

| Fichier | Ligne | Motif |
|---|---|---|
| `src/game/board/star.c` | 638 | `while (msmStreamGetStatus(...) != 0)` |
| `src/game/board/lottery.c` | 1843, 1882 | `while (HuAudSStreamStatGet(...) != 0)` |
| `src/game/board/roll.c` | 923 | idem |
| `src/game/board/boo_house.c` | 735 | idem |
| `src/game/board/item.c` | 1864 | idem |
| `src/game/board/last5.c` | 1140 | idem |
| `src/REL/w01..w06Dll/mg_item.c` | ~366-545 | idem |
| `src/REL/mstoryDll/mg_clear.c` | 237 | idem |
| `src/REL/mstory2Dll/mg_clear.c` | 331 | idem |
| `src/REL/m450Dll/main.c` | 7236, 7575 | idem |
| `src/REL/w04Dll/big_boo.c` | 992 | idem |

Chaque boucle cède un tick de jeu par itération (`HuPrcVSleep()`), donc **le
nombre de ticks passés dans l'attente dépend du débit audio réel de chaque PC**.
Deux machines sortent de la boucle à des index de frame différents ⇒ désync dure
et systématique sur les événements de plateau les plus fréquents (achat d'étoile,
loterie, boutique, Boo, lancer de dé, fin de mini-jeu).

C'est aussi une *data race* : le thread audio écrit `slot->status` sans que le
thread de jeu ne prenne `globalMutex` (`hw_pc.c:1891-1897`) pour le lire.

**Classement : CRITICAL. Priorité P0 absolue.**

#### C2. Attentes en temps mur dans `bootDll`

`src/REL/bootDll/main.c:231-233` et `:275-277` :

```c
tick_prev = OSGetTick();
...
while (OSTicksToMilliseconds(OSGetTick() - tick_prev) < 3000) {
    HuPrcVSleep();      // ← cède un tick de jeu par itération
}
```

Le nombre de ticks consommés dépend du temps réel écoulé sur chaque machine. En
`--netplay-full`, la timeline réseau démarre au premier tick, donc **le premier
écran du jeu diverge déjà**. Chemin actif seulement quand `SystemInitF == 0`
(premier démarrage), c'est-à-dire exactement le cas d'une session online lancée
à froid.

**Classement : CRITICAL. P0.**

#### C3. `HuAudSndGrpWait` — boucle bornée en temps mur aux transitions d'overlay

`src/game/audio.c:559-568` :

```c
while ((msmMusGetNumPlay(TRUE) != 0 || msmSeGetNumPlay(TRUE) != 0)
    && OSTicksToMilliseconds(OSGetTick() - tickStart) < SNDGRP_TIMEOUT) {
    msmSysRegularProc();
}
```

Appelée depuis `HuAudSndGrpSetSet` (l.591), elle-même appelée par
`HuAudSndCharGrpSet` / `HuAudDllSndGrpSet` **au moment exact du changement
d'overlay** (`src/game/objmain.c:95-96`). Elle ne cède pas de tick de jeu (donc
pas de dérive de frame directe), mais elle appelle `msmSysRegularProc()` un
nombre de fois qui dépend du temps réel et de l'état du thread audio : l'état msm
(fades, compteurs de flux, voix) diverge à chaque transition. Ce n'est pas
directement visible dans le hash actuel, mais cela alimente C1.

La boucle de réessai l.612-618 dépend en plus du résultat d'allocation
`HuMemDirectMalloc(HEAP_DATA, sampSize)`.

**Classement : CRITICAL (contributeur direct de C1). P0.**

### HIGH — divergence possible ou détection défaillante

#### H1. Le hash canonique ne couvre pas l'état de jeu privé des overlays

`include/port/netplay_canonical.hpp` couvre : `GWSystem`, `GWPlayer`,
`GWPlayerCfg`, `GWGameStat`, drapeaux système, PAD logiques, tables de mini-jeux,
`mgTicTacToeGrid`, l'état de séquence (`minigame_seq.c:3653`) et l'état du dé
(`board/roll.c:84`).

Il **ne couvre pas** : objets `omObjData`, topologie/état des processus HuPrc,
état des modèles/motions HSF, sprites, wipe, heaps, état msm logique, position
des overlays, `omovlstat`/`omovlevtno`/`omovlhis`, la plupart des variables
statiques des DLL de plateau et de mini-jeu.

Conséquence : une divergence née dans un mini-jeu ou dans une DLL de plateau
n'est détectée que lorsqu'elle finit par toucher `GWPlayer`/`GWSystem` —
c'est-à-dire des centaines de frames plus tard, ou jamais (désync visible mais
non signalée). C'est exactement ce que la mission E demande de corriger.

Il n'y a par ailleurs **aucun découpage par sous-système** : un seul FNV-1a 64
bits global (`netplay_state.hpp:26-31`), donc le rapport ne peut pas dire
« BOARD DIFFERENT / RNG OK ».

**Classement : HIGH (diagnostic). P1 — mais préalable à tout le reste.**

#### H2. Aucun compteur de consommation RNG

`frand` (`src/game/frand.c:32`), `rand8` (`src/game/main.c:358`),
`BoardRand` (`src/game/board/main.c:1446`) : seules les **graines** sont
comparées (`StateDigest::frand/rand8` + `boardRandSeed` dans le hash). Une graine
différente prouve qu'un appel de trop a eu lieu, mais n'indique ni la fonction,
ni l'index d'appel, ni le contexte. La mission 3 demande explicitement
`frand_calls / rand8_calls / boardrand_calls`.

Note positive : la synchronisation initiale est correcte. L'hôte publie ses
graines (`netplay_runtime.cpp:315-317`), le client les applique
(l.578-586), et `BoardRandInit` dérive `boardRandSeed` de `frand_state_get()`
sans consommer d'échantillon en online (`board/main.c:1437-1441`). Le chemin
« graine nulle » de `frandom` évite `OSGetTime()` en online (`frand.c:18`).

**Classement : HIGH (diagnostic). P1.**

#### H3. `VIGetRetraceCount()` renvoie 0 en permanence

`src/port/stubs.c:312-316` — le commentaire `TODO this might be important` est
justifié. Appelants réels :

- `src/game/main.c:201` → `HuSysDoneRender(retrace)` → `src/game/init.c:222-228` :
  `while (VIGetRetraceCount() - retrace_count < minimumVcount - 1) VIWaitForRetrace();`
  Avec un compteur figé à 0, la boucle est **infinie dès que `minimumVcount > 1`**.
  Aujourd'hui `minimumVcount` vaut 1 à cet endroit (rétabli en `main.c:290`, et
  tous les appelants du jeu font `HuSysVWaitSet(1)`), donc le gel ne se produit
  pas — mais c'est une bombe amorcée, notamment si le rollback restaure
  `minimumVcount` (il est bien dans `PartyBoard_RollbackClockRegions`,
  `main.c:424`) à une valeur ≠ 1 hors de la fenêtre de rendu.
- `src/game/sreset.c:466-472` : boucle bornée à 1 349 800 itérations, chemin de
  soft-reset uniquement.

`VIGetRetraceCount` n'alimente **aucune logique de gameplay**. Le rendre
déterministe (compteur de ticks simulés) supprime le risque de gel sans changer
le comportement offline.

**Classement : HIGH (risque de gel). P0 pour la robustesse, P2 pour la désync.**

#### H4. `context` dans `StateDigest` : tolérance zéro vs 120 frames côté entrées

`netplay_runtime.cpp:1208-1222` accorde 120 frames de grâce à une différence de
`captureContext` entre pairs, alors que `StateDigest::operator==`
(`netplay_state.hpp:18`) compare `context` sans tolérance : la vérification de
hash échoue donc **avant** la grâce, et le message affiché est « DESYNC » au lieu
du message de contexte. Le code de grâce est de fait mort en `--netplay-full`.

Ce n'est pas un faux positif (une différence de contexte à la même frame *est*
une divergence, cf. §1.1 : les transitions sont pilotées par des ticks
déterministes), mais c'est incohérent et cela dégrade le diagnostic.

**Classement : HIGH (cohérence/diagnostic). P1.**

### MEDIUM

#### M1. `VCounter` diverge pendant l'initialisation

`src/game/pad.c:377` incrémente `VCounter` à chaque `PadReadSimulationTick`.
Avant le premier tick simulé, ces appels viennent du callback VI
(`PadReadVSync`, l.422) déclenché par chaque `VIWaitForRetrace()` de
`SwapBuffers` (`init.c:241`) — donc une fois par image rendue pendant le boot,
en nombre variable selon la machine. `VCounter` n'est lu qu'en
`src/game/sreset.c:481`, mais il fait partie du snapshot PAD (`pad.c:105`).

**Classement : MEDIUM.**

#### M2. `HuAudSndGrpSetSet` dépend du résultat d'allocation

`src/game/audio.c:571-588` : `HuMemDirectMalloc(HEAP_DATA, sampSize)`. Un échec
côté d'un seul pair change le chemin de code (l.612-618, deux tentatives). La
divergence d'allocation est possible si l'état du tas diverge, ce qui est
justement ce qu'on cherche à détecter.

**Classement : MEDIUM.**

#### M3. Écriture de sauvegarde pendant une session online

`src/game/saveload.c:384, 838` — `OSGetTime()` pour l'horodatage. Exclu du hash
(commentaire explicite `netplay_canonical.hpp:118`), donc non détectable comme
désync, mais les deux pairs écrivent des sauvegardes différentes.

**Classement : MEDIUM (hors désync).**

### LOW

- `src/port/dvd.c:126` — `DVDReadAsyncPrio` est **synchrone** et rappelle le
  callback en ligne. C'est un point *positif* : les chargements DVD ne
  consomment aucun tick variable. À ne pas « corriger ».
- `fadeStat` n'est jamais mis à une valeur non nulle dans le port
  (`src/game/audio.c:38,88`), donc la porte `omnextovl >= 0 && fadeStat == 0`
  (`src/game/objmain.c:84`) est toujours ouverte. Pas de dépendance au fondu
  audio pour les transitions d'overlay. Bon.
- Threads natifs : `src/port/ui/prelaunch.cpp:89` (validation ISO, UI seule),
  `src/port/rollback_io.cpp:75` (auto-test). Aucun ne touche l'état de jeu.
  Le seul thread problématique est celui de MusyX (C1).
- `EXI*`, `OSLink`, `OSGetFont*` : stubs jamais atteints en jeu normal.

---

## 3. Ce qui est déjà solide (à ne pas casser)

- Le pipeline de ticks est déjà découplé du framerate d'affichage (§1.1).
- `wipe->time`, animations HSF, `GlobalCounter` avancent au rythme des ticks
  acceptés, pas des images.
- Les entrées réseau sont brutes (avant clamp), horodatées par frame absolue,
  immuables, et toute contradiction fait échouer la session.
- La perte de pair ne fait jamais retomber en offline silencieusement.
- Les chargements DVD sont synchrones donc déterministes.
- La synchronisation initiale des RNG est correcte.

---

## 4. Plan de correction (ordre imposé par les dépendances)

| # | Action | Type | Priorité |
|---|---|---|---|
| 1 | Hash canonique par sous-système + première frame divergente + champ divergent | Diagnostic | P0 (préalable) |
| 2 | Compteurs de consommation RNG (`frand`/`rand8`/`BoardRand`) dans le hash | Diagnostic | P0 (préalable) |
| 3 | Découpler l'état **logique** des flux audio du thread audio (C1) | Cause racine | P0 |
| 4 | Remplacer les attentes en temps mur de `bootDll` par des attentes en ticks (C2) | Cause racine | P0 |
| 5 | Rendre `HuAudSndGrpWait` déterministe (C3) | Cause racine | P0 |
| 6 | `VIGetRetraceCount` déterministe (H3) | Robustesse | P0 |
| 7 | Fichier de rapport de désync par pair + outil `netplay_compare` | Diagnostic | P1 |
| 8 | Étendre la couverture du hash (objets, HuPrc, overlays, msm logique) | Diagnostic | P1 |
| 9 | Rollback : test SAVE/REPLAY/RESTORE/REPLAY par contexte | Rollback | P2 |

Les points 1-2 doivent précéder 3-5 : sans eux, on ne peut pas **prouver** qu'une
correction fonctionne, seulement qu'elle compile.

---

## 5. Statut des corrections

| # | Sujet | Statut | Correctif |
|---|---|---|---|
| C1 | Attente sur l'audio temps réel | **corrigé** | Horloge logique de flux dérivée des données du disque (`src/msm/msmstream.c`), avancée une fois par tick accepté depuis `PadReadSimulationTick`. Seule la transition de fin de flux — celle que pilote le thread audio — est corrigée ; pause, arrêt explicite et chargement restent inchangés. |
| C2 | Attentes en temps mur du boot | **corrigé** | `BootWaitMs` dans `src/REL/bootDll/main.c` : 180 ticks pour 3 s, 60 pour 1 s, exactement ce que comptent déjà à la main les branches voisines du même fichier. |
| C3 | Drain de banque audio | **corrigé** | `SNDGRP_DRAIN_STEPS` itérations fixes dans `HuAudSndGrpWait`, sans sortie anticipée sur des compteurs influencés par le thread audio. |
| H3 | `VIGetRetraceCount` figé | **corrigé** | Compteur incrémenté par `VIWaitForRetrace` (`src/port/stubs.c`), ce qui borne la boucle de `init.c:226`. Volontairement absent du hash canonique : il avance par image présentée, pas par tick simulé. |
| — | Sous-système AUDIO du hash | **corrigé** | `msmMusGetNumPlay` / `msmSeGetNumPlay` retirés : influencés par le thread audio, ils auraient produit de faux positifs sur toute paire de machines. |

Portée : les correctifs de timing sont gardés par `PartyBoard_NetplayEnabled()`.
Hors ligne, le code exécuté est littéralement celui d'avant.

### C4 (CRITICAL) — position des films THP dérivée du curseur audio — CORRIGÉ

Trouvé après l'audit initial, par un test. `ThpMovie::ended()` et `current_frame()`
(`src/port/thp_player.cpp`) calculaient la frame vidéo depuis `cursor`, un
`std::atomic` avancé par le mixeur sur le thread audio. Six sites de gameplay s'y
bloquent en cédant un tick par itération : `REL/bootDll/main.c:359`,
`REL/modeseldll/main.c:159`, `REL/modeseldll/modesel.c:207`,
`REL/mstory2Dll/main.c:154`, `REL/mstory2Dll/ending.c:359`,
`REL/staffDll/main.c:640`.

Observé en vrai : deux pairs quittant un film à des ticks différents ont demandé
des fondus différents (`duration` 10.0 contre 20.0, couleur blanche contre noire),
signalé comme `DESYNC frame=2148 category=SCENE`.

Correctif : horloge logique en ticks acceptés, `fps` et nombre de frames venant de
l'en-tête THP donc du disque.

**Leçon de méthode** : l'audit initial n'avait inspecté que `src/game/thpmain.c`,
où il n'y a qu'un `VIWaitForRetrace`. L'implémentation PC n'avait pas été lue avec
la même rigueur. C'est un test qui a rattrapé l'omission.

---

## 6. Non corrigé : fin de morceau et d'effet sonore séquencés

### C5 (CRITICAL, non corrigé) — `msmMusGetStatus`

`src/msm/msmmus.c`, dans `msmMusPeriodicProc` :

```c
case MSM_MUS_STOP:
case MSM_MUS_PLAY:
    if (sndSeqGetValid(player->seqId) == FALSE) {
        player->status = 0;   /* MSM_MUS_DONE */
    }
```

`sndSeqGetValid` (`extern/musyx/.../seq_api.c:105`) renvoie simplement si
l'instance de séquence existe encore ; elle est retirée quand le séquenceur
termine, ce que le thread audio fait avancer. La fin d'un morceau est donc
décidée par le thread audio.

Sites de gameplay concernés :

| Fichier | Ligne | Motif |
|---|---|---|
| `REL/w04Dll/big_boo.c` | 239, 325, 807 | `while (msmMusGetStatus(1) != 0)` |
| `REL/w02Dll/mg_coin.c` | 294 | `if (msmMusGetStatus(1) == 0)` |
| `REL/w04Dll/mg_coin.c` | 203, 207 | `if (msmMusGetStatus(...) == 0)` |
| `REL/ztardll/main.c` | 1122 | `if (msmMusGetNumPlay(1) ...)` |
| `game/board/audio.c` | 110 | renvoie `msmMusGetStatus(board[0])` |

### C6 (CRITICAL, non corrigé) — `HuAudFXStatusGet`

`REL/w06Dll/bowser.c:570` : `while (HuAudFXStatusGet(temp_r28) != 0)`. Même
mécanisme, sur l'état des voix d'effet sonore.

### Pourquoi ce n'est pas corrigé

La recette de C1 et C4 — une horloge logique dont la durée vient des données du
disque — **ne s'applique pas ici**. Un flux audio a une longueur en octets et un
film un nombre de frames dans son en-tête ; un morceau séquencé n'a ni l'un ni
l'autre. Sa fin émerge de l'exécution du séquenceur. Vérifié : `MSM_MUS`,
`MSMGrpInfo` et les structures de séquence MusyX n'exposent aucune durée
( `NOTE_DATA::length` est une longueur de note, pas de morceau ).

Trois options, toutes avec un coût réel :

1. **Faire avancer le séquenceur au tick de jeu.** Architecturalement juste et
   subsumerait C1, C4, C5 et C6. Mais dans MusyX, `snd_handle_irq` mêle
   l'avance du séquenceur et le mixage ; et en lockstep le thread de jeu
   s'arrête pour attendre son pair, ce qui ferait famine audio à chaque attente.
   Séparer les deux est possible mais c'est une modification profonde du moteur.
2. **Attente de durée fixe en ligne.** Centralisable dans `msmMusGetStatus`, sans
   retoucher les sites d'appel, donc sans hack par mini-jeu. Mais sans durée
   connue il faudrait un plafond générique, ce qui allonge les pauses ou coupe
   les jingles.
3. **Synchroniser explicitement cet état sur le réseau.** Autorisé par la
   consigne (« quelques états explicitement définis »), mais cela place une
   décision audio dans le protocole.

En attendant une décision, la divergence n'est **pas masquée** mais rendue
attribuable : le rapport de désync contient une ligne `audio_thread_context` avec
le statut musical par canal, non hachée. Elle n'est pas hachée parce qu'un écart
de statut que personne n'interroge n'est pas une divergence de gameplay, et le
hacher tuerait des sessions sans raison.

---

## 7. Couverture du hash canonique

Les seize sous-systemes sont desormais alimentes. ANIMATION a ete le dernier
ajoute : il couvre, par modele vivant de `Hu3DData`, les quatre horloges de
motion, les attributs, les temps et attributs de cluster, et l'occupation du slot
exportee comme booleen plutot que comme le pointeur `hsf`.

C'est le sous-systeme le plus observe du jeu. L'inventaire exhaustif des attentes
bloquantes (`while (f(...))`) du code de jeu et des overlays donne :

| Etat attendu | Occurrences | Statut |
|---|---|---|
| Temps de motion HSF | 252 | propriete du thread de jeu, **hache** |
| `WipeStatGet` | 183 | hache (SCENE) |
| `MGSeqStatGet` | 24 | hache (SEQUENCE) |
| `HuDataGetAsyncStat` / `HuARDMACheck` | 31 | deterministe : `ARQPostRequest` copie et rappelle son callback en ligne |
| `HuAudSStreamStatGet` | 11 | C1, corrige |
| `HuTHPEndCheck` | 5 | C4, corrige |
| `msmMusGetStatus` / `HuAudFXStatusGet` | 10 | **C5 / C6, non corriges** |

Cet inventaire est exhaustif sur ce motif : toute attente bloquante du jeu est
soit sous le hash, soit corrigee, soit documentee en C5/C6. Il n'y a pas d'autre
dependance au temps reel de cette famille.

Validation d'ANIMATION : deux instances reelles, 280 secondes, 13 transitions
d'overlay aux frames identiques, 140 checkpoints canoniques identiques, aucun faux
positif.

Reste non couvert : le temps logique des sprites (`sprman.c`), dont aucune attente
bloquante ne depend.
