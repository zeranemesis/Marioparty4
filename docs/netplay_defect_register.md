# Registre des défauts trouvés et non encore corrigés

Un défaut est inscrit ici dès qu'il est identifié, avec sa classification
technique exacte et ce qui reste à prouver. Un défaut n'est retiré du registre
que lorsqu'un test déterministe démontre sa correction.

La règle de classification : on ne relie jamais deux défauts sans preuve. Un
défaut trouvé sur le chemin d'un crash n'est pas pour autant la cause de ce
crash.

---

## D1 — `BoardSpaceCornerPosGet` : index de coin non borné

**Classification : confirmed unbounded array index, producing an out-of-bounds
read of an 8-byte stack array.**

**Statut : non corrigé. Détection instrumentée, correction différée.**

### Le code

`src/game/board/space.c:165` :

```c
void BoardSpaceCornerPosGet(s32 index, s32 corner, Vec *pos)
{
    Vec corner_ofs;
    Vec rot;
    s8 corner_pos[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    BoardSpaceRotGet(0, index, &rot);
    BoardSpacePosGet(0, index, pos);
    corner_ofs.x = corner_pos[corner][0] * 80.0f;
    corner_ofs.y = 0;
    corner_ofs.z = corner_pos[corner][1] * 80.0f;
    ...
}
```

`corner_pos` est un tableau local de 4 × 2 octets, soit **8 octets sur la pile**.
`corner` n'est borné **ni vérifié nulle part**.

### L'appelant

`src/REL/w04Dll/boo_event.c:1060`, dans `fn_1_52A0` :

```c
var_r27 = 0;
for (i = 0; i < 4; i++) {
    ...
    sp8[i] = GWPlayer[i].space_curr;
    if (lbl_1_bss_B0 == sp8[i]) {
        var_r27 += 1;            /* 0..4 */
    }
}
for (i = 0; i < 4; i++) {
    var_r30 = fn_1_2FBC(i);
    if (var_r30->unk08 != -1 && var_r30->unk08 != GWSystem.player_curr) {
        BoardSpaceCornerPosGet(lbl_1_bss_B0, var_r27++, &arg1[var_r30->unk08]);
    }
}
```

`var_r27` part du **nombre de joueurs déjà présents sur la case cible** (0 à 4),
puis s'incrémente une fois par itération qui passe le garde.

Borne atteignable :

| Condition | Valeur maximale de `corner` |
|---|---|
| 4 joueurs sur la case cible, 4 emplacements qualifiants | **7** |
| 4 joueurs sur la case, 3 joueurs distincts non courants | **6** |
| Hors-bornes dès que | **`corner >= 4`** |

Condition nécessaire : `joueurs_sur_la_case + emplacements_qualifiants > 4`.

### Ce que cela fait, et ce que cela ne fait pas

**Ce que cela fait.** Pour `corner = 7`, la fonction lit
`((s8*)corner_pos)[14]` et `[15]`, soit **6 et 7 octets au-delà** d'un tableau de
8 octets sur la pile. Les valeurs lues sont multipliées par `80.0f` et
deviennent un décalage de position : un modèle de joueur placé à une coordonnée
arbitraire.

**Ce que cela ne fait pas.**

- Ce n'est **pas** la corruption de tas de la session Big Boo. C'est une
  **lecture**, et de la pile, pas du tas. Une lecture ne peut pas lever
  `STATUS_HEAP_CORRUPTION` (`0xC0000374`).
- Ce n'est **pas** une cause de désynchronisation entre pairs. Les octets
  adjacents lus dépendent de la disposition de pile choisie par le compilateur,
  identique pour un même binaire, et de l'exécution antérieure, qui est
  déterministe. Les deux pairs liraient donc les mêmes octets.
- L'écriture `&arg1[var_r30->unk08]` est, elle, **dans les bornes** :
  `arg1` est `lbl_1_bss_80[4]` (`boo_event.c:87`) et `unk08` est un index de
  joueur 0..3 ou -1, le -1 étant exclu par le garde.

### Le seuil réel, mesuré

`tools/test_board_corner_index.ps1` transcrit l'arithmétique de `fn_1_52A0` et
énumère ses 40 000 états. Résultat :

| hypothèse sur les emplacements | index maximal | occupants nécessaires |
|---|---|---|
| un joueur peut détenir deux emplacements | **7** | **1** |
| un joueur ne détient qu'un emplacement | **6** | **2** |

**Le tableau de cas ci-dessus est donc trompeur, et je l'avais lu de travers.**
Il n'a jamais fallu quatre joueurs sur la case : la condition est
`occupants + emplacements_qualifiants > 4`, et avec quatre emplacements dont un
exclu pour le joueur courant, **un seul occupant suffit** si un joueur peut
détenir deux emplacements, deux sinon. La première version du test affirmait
quatre et a échoué ; c'était le test qui avait tort, pas le registre, dont la
condition nécessaire était correctement écrite dès le départ.

Que `unk08` puisse se répéter d'un emplacement à l'autre n'est pas tranché : le
défaut D2 décrit précisément une affectation qui écrase le quatrième emplacement
au lieu de refuser. Les deux seuils sont donc rapportés plutôt qu'un seul
supposé.

### Ce qui reste à prouver

Le chemin est-il réellement emprunté en jeu ? L'arithmétique dit que oui à partir
d'un ou deux occupants ; elle ne dit rien de la fréquence de ces états dans une
vraie partie. **Une détection est donc instrumentée** dans
`BoardSpaceCornerPosGet` : elle signale tout appel avec `corner >= 4` dans le fil
d'événements du rapport de crash, sans modifier l'arithmétique.

**Elle n'a jamais déclenché**, sur les **142 journaux de pair** accumulés à ce
jour. C'est un chiffre, pas une impression, et c'est la raison pour laquelle la
correction reste différée : corriger maintenant serait spéculatif au sens exact
du mot.

### Correction envisagée, volontairement différée

Borner `corner` dans `BoardSpaceCornerPosGet`, et corriger le comptage dans
`fn_1_52A0` pour qu'il ne puisse pas dépasser le nombre de coins. Ce correctif
appartient à un commit distinct du rapporteur de crash et de l'instrumentation
de pile, et ne doit pas être appliqué avant que la détection ait dit si le
chemin est vivant.

---

## D2 — `fn_1_52A0` voisin : emplacement écrasé quand les quatre sont occupés

**Classification : logic defect, no memory error.**

**Statut : non corrigé, observé en lisant D1, non instrumenté.**

`src/REL/w04Dll/boo_event.c:956-962` :

```c
for (i = 0; i < 4; i++) {
    var_r29 = fn_1_2FBC(i);
    if (var_r29->unk08 == -1) {
        break;
    }
}
var_r29->unk08 = temp_r30;
```

Si les quatre emplacements ont déjà `unk08 != -1`, la boucle se termine sans
`break` et `var_r29` conserve le dernier emplacement visité, `fn_1_2FBC(3)`, dont
l'affectation est alors **écrasée**. Il n'y a pas d'accès hors-bornes — `var_r29`
pointe toujours sur un emplacement valide — mais un joueur perd son association.

Aucune preuve que ce cas se produise. Inscrit pour ne pas le redécouvrir.

---

## D3 — Banque audio libérée sous une voix encore en lecture — **CORRIGÉ**

**Classification : use-after-free of a MusyX sample allocation, game thread frees
while the audio thread still reads. Deterministic; its crash is not.**

**Statut : corrigé, vérifié sur 21 replays. Preuve complète dans
[`docs/d3_audio_bank_lifetime.md`](d3_audio_bank_lifetime.md).**

### Vérification

Barrière en place dans `hwRemoveSample`, scénario `w04-results-unload`,
21 replays en trois campagnes parallèles :

| | avant | après |
|---|---|---|
| déchargements de l'écran de résultats produisant des références obsolètes | 4 sur 4 | 0 |
| références obsolètes par déchargement | 3 | 0 |
| lectures après libération sur le thread audio | 1 par déchargement | 0 |
| runs | 7 (2 crashes) | **21 (0 crash)** |

Et la preuve que le zéro a été mesuré : sur les 42 traces, **1260 libérations de
banque observées** et **509 voix vivantes détachées** par la barrière — chacune
aurait été une référence obsolète. Un détecteur éteint aurait rendu le même zéro
de violations mais zéro libération observée aussi.

Le test de non-régression est le scénario `w04-results-unload` lui-même : il
traverse les quatre déchargements, et la campagne classe toute violation en
`CRASH` même sans faute levée. **Ne pas le retirer de la campagne.**

### Ce qui est établi

Au déchargement de l'overlay 84 — `resultDll`, l'écran de résultats de mini-jeu,
d'après la table `include/ovl_table.h` — le jeu libère les banques audio du jeu
d'overlays sortant alors que des voix DSP en lisent encore les échantillons.

Mesuré à la frame **18141** et à la frame **27744**, sur les deux pairs, avec les
mêmes banques, les mêmes échantillons, les mêmes positions de lecture :

```
BANK_RELEASE_REQUEST            frame=18141 bank=19 group=112 samples=22
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1300 voice=37 voice_state=2 pos=2680
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1300 voice=39 voice_state=2 pos=4867
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1191 voice=12 voice_state=2 pos=1534
BANK_FREE                       frame=18141 bank=19
AUDIO_STALE_SAMPLE_READ         frame=18142 voice=12 sample=1191 freed_frame=18141
```

La frame 27744 est exactement celle des deux crashes historiques, et la lecture
invalide `ensureADPCMBlockDecoded+0xa4` (`hw_pc.c:711`) en est la conséquence
visible.

### Les deux causes, dans le code

1. **`hwBreak` ne fait qu'une demande.** `sndPopGroup` tue les voix par
   `synthKillVoicesByMacroReferences` → `voiceKill` → `hwBreak`, et `hwBreak`
   (`hardware.c:215`) se contente de lever le bit `0x20` que le thread audio
   lira à son prochain rendu. La voix continue de lire jusque-là.
2. **La libération se fait hors du verrou audio.** `sndPopGroup` rend
   `globalMutex` avant `RemoveSamples`, et le `free` réel — celui de
   `hwRemoveSample` sur la copie ARAM par échantillon — n'est protégé par rien.

S'y ajoute que `synthKillVoicesBySampleReferences` est **exclue à la
compilation** : `extern/musyx/CMakeLists.txt` fixe la version MusyX à 1.5.4 et le
garde dans `s_data.c` demande 2.0.1 ou plus. Une voix référençant un échantillon
du groupe sans macro correspondante n'est donc même pas prévenue.

### Ce que cela dit du correctif C3

`HuAudSndGrpWait` a été rendu déterministe par un nombre fixe d'itérations. La
trace montre `BANK_AUDIO_DRAIN_BEGIN irq=60725` et `BANK_AUDIO_DRAIN_END
irq_total=60725` : **le compteur de callbacks audio n'a pas avancé d'une seule
unité pendant le drain**. Le drain garantit le déterminisme et **rien** de la
durée de vie. Augmenter le nombre d'itérations ne changerait pas ce zéro.

### Correction retenue

Une barrière au point exact de la libération : dans `hwRemoveSample`, sous
`globalMutex`, détacher toute voix DSP pointant encore dans l'allocation, puis
libérer. Sous le verrou le thread audio n'est pas dans `salCtrlDsp`, et après le
détachement il ne peut plus y entrer sur cette adresse. Aucun délai, aucun
`Sleep`, aucun nombre de frames arbitraire.

---

## D4 — Débordement de pile de coroutine HuPrc — **CORRIGÉ**

**Classification : coroutine stack exhaustion, confirmed at instruction level.**

**Statut : corrigé par `875a1ef7`, vérifié deux fois.**

Conservé ici parce que le chemin qui a mené à sa découverte est réutilisable.

`fn_1_30A4` (`src/REL/w04Dll/boo_event.c:343`) demandait la constante `0x1000`,
doublée à 8192 octets sur PC, pour un besoin mesuré de **8200 octets**. Il
débordait de **huit octets**, exactement l'adresse de retour d'un `call`.

Deux manifestations selon la présence d'une page de garde :

| Build | Pile de coroutine | Symptôme |
|---|---|---|
| `bc63c93c` | bloc `malloc` nu | écrase l'en-tête de tas voisin → `0xC0000374` |
| avec page de garde | réservation gardée | faute sur la page de garde → `0xC0000005` |

**Pourquoi il a résisté si longtemps.** Ce mode de défaillance détruit ses
propres preuves. Le `call` qui déborde ne peut pas empiler son adresse de
retour ; le noyau ne peut pas davantage empiler un cadre d'exception ; aucun
gestionnaire en mode utilisateur ne s'exécute. De plus, le compteur de marque
d'eau ne mesure que les piles retirées ou vivantes au moment d'un rapport : la
pile qui déborde tue le processus à cet instant et n'est **jamais** mesurée.
Tous les pics jamais imprimés étaient des survivants, ce qui a fait passer
l'hypothèse pour réfutée avec un maximum rassurant de 27 %.

La sortie a été la sémantique `PAGE_GUARD` de Windows, qui lève une exception
*et* débloque la page dans le même geste, laissant assez de pile pour rapporter.

---

## D5 — L'horloge de motion HSF n'avance pas dans un tick rejoué

**Classification : state advanced outside the replayed tick, making SAVE /
RESTORE / REPLAY non-reproducible.**

**Statut : trouvé par le harnais rollback local, non corrigé.**

Premier test du probe `PARTYBOARD_FORCE_ROLLBACK`, sur la toute première frame
qu'il a examinée : un retour arrière d'**une seule frame** ne se reproduit pas.

```
save_frame=300  target_frame=301  replay_length=1
first_divergent_subsystem=ANIMATION
first_divergent_field=(model->motWork).time
expected_value=0x43860000 (268.0)   actual_value=0x43858000 (267.0)
```

Quinze des seize sous-systèmes canoniques sont identiques au bit près. Seul
`ANIMATION` diverge, et sur un seul champ, pour trois modèles : le temps de
motion est **exactement une frame en retard** après le rejeu.

### Pourquoi

`src/game/hsfman.c:348` : `PartyBoard_AnimationAdvance()` est appelée à la fin de
`Hu3DExec()`, c'est-à-dire dans la **passe de présentation**, pas dans la logique
de jeu. Or un tick rejoué est
`PartyBoard_RollbackRunGameLogicTick` → `PartyBoard_RunGameLogicTick`
(`src/game/main.c:98`), qui ne fait pas de passe de présentation. L'horloge de
motion n'avance donc jamais pendant un rejeu.

C'est le même genre de trou que `GlobalCounter`, que le chemin de rollback réel
compense déjà explicitement par un `++GlobalCounter` dans `simulateFrame`. Rien
ne compense l'animation.

### Ce que cela implique

Le chemin de rollback réseau réel a exactement le même trou : son `simulateFrame`
n'appelle que `PartyBoard_RollbackRunGameLogicTick` et `++GlobalCounter`. Donc
**tout rollback réseau complet construit sur cette base ferait dériver les
animations d'une frame par frame rejouée.** C'est précisément la raison pour
laquelle le harnais local devait exister avant le rollback réseau.

### Ce qu'il faudra décider avant de corriger

L'ordre. Dans une frame normale la séquence est `logique(F)` puis `dessin(F)`,
et c'est `dessin(F)` qui fait avancer l'horloge. L'instantané est pris après
`logique(F)`. Pour que le rejeu de `logique(F+1)` reproduise l'état, il doit donc
d'abord rejouer l'avance d'animation qu'avait faite `dessin(F)`, puis la logique.
Et `PartyBoard_AnimationAdvance` appelle aussi `HuSprFinish()` et
`Hu3DAnimExec()` : il faudra vérifier ce que ces deux-là touchent avant de les
rejouer hors de la passe de dessin.

### Coût du snapshot, mesuré au passage

`snapshot_bytes=53949214`, soit **51,5 Mo par point de sauvegarde**. Le probe en
garde deux à la fois. C'est une mesure, pas une estimation, et elle appartient au
dossier du rollback réseau.

---

## D6 — L'horloge d'animation avance par image rendue, pas par tick simulé

**Classification : logical state driven by the render cadence, which is
wall-clock dependent. Latent, hors ligne uniquement — mécanisme établi par
lecture, inatteignable en ligne, occurrence non observée.**

**Statut : non corrigé, non observé, et neutralisé en ligne par le bridage de
`target_frame_rate()`. Trouvé en lisant le code autour de D5.**

### Correction de cette entrée, 2026-09-11

La version précédente de cette entrée disait que le silence du détecteur venait
du choix de la campagne — « tous les runs tournent à 60 » — et que deux pairs
pouvaient se connecter à des cadences différentes sans que rien ne le signale.
**C'était faux, et la conclusion pratique était dangereuse.**

`src/port/imgui.cpp` :

```cpp
int target_frame_rate()
{
    return PartyBoard_TargetFrameRateFor(PartyBoard_NetplayEnabled(),
        partyboard::getSettings().video.targetFrameRate.getValue());
}
```

et la fonction pure qu'elle appelle retourne `kOriginalSimulationRate` — 60 —
dès que le netplay est actif, **quel que soit le réglage vidéo**. En ligne, les
deux pairs sont donc bridés à 60 par le moteur, pas par la campagne. Il n'existe
aucune configuration en ligne où `simulatedTicks >= 2`.

Conséquence sur le plan de mesure : l'expérience prévue — « un pair à 60,
l'autre à 240, voir si `ANIMATION` diverge » — **aurait produit un résultat nul
ressemblant à une preuve.** Les deux pairs auraient tourné à 60, le hash aurait
concordé, et on aurait écrit « D6 non reproductible en ligne » en croyant l'avoir
testé. C'est exactement le genre de faux vert que ce registre existe pour
empêcher.

Le bridage est donc une **propriété de sûreté porteuse**, et non une préférence :
c'est lui qui rend inoffensive l'absence de la fréquence d'images dans
`runtimeConfigSignature`. Il est désormais épinglé par le sous-test
`frame-rate-clamp` de `--netplay-self-test`, qui vérifie que huit réglages
distincts donnent 60 en ligne et que le réglage hors ligne reste honoré. Retirer
le bridage pour laisser le netplay tourner à 144 fait rougir ce test
immédiatement, au lieu de produire un an plus tard une désynchronisation que
personne ne saura relier à un réglage vidéo.

### Le mécanisme

`src/game/hsfman.c:23` :

```c
#define PARTYBOARD_ADVANCE_FRAME PartyBoard_IsSimulationTick
```

et `src/game/main.c:276` :

```c
PartyBoard_IsSimulationTick = simulatedTicks != 0;
```

C'est un **booléen**, pas un compte. Or la boucle de `main.c:246` peut exécuter
plusieurs ticks de simulation dans une seule image rendue, et `Hu3DExec()` —
donc `PartyBoard_AnimationAdvance()` et le `data->tick++` de `hsfman.c:330` —
n'est appelée **qu'une fois par image rendue**.

Donc : deux ticks de simulation dans la même image rendue font avancer l'horloge
d'animation **une seule fois**.

### Quand cela peut arriver

`frame_pacer_simulation_tick()` (`src/port/imgui.cpp:301`) retourne
**exactement 1** tant que `video.targetFrameRate <= 60`. Au-dessus, il rattrape
le temps réel et peut rendre 0, 1 ou plusieurs ticks, borné par
`kMaxSimulationTicksPerFrame`.

Mais en ligne cette porte est fermée en amont : `target_frame_rate()` ignore le
réglage vidéo dès que `PartyBoard_NetplayEnabled()` est vrai et retourne 60. La
campagne écrit bien `video.targetFrameRate = 60` dans le profil, mais ce n'est
pas ce qui garantit le rapport 1:1 — le moteur le garantissait déjà. Si le
sous-système `ANIMATION` n'a jamais divergé entre pairs, ce n'est pas parce que
la campagne a bien choisi ses réglages, c'est parce qu'**aucun réglage ne peut
ouvrir ce chemin en ligne**.

Hors ligne, au-dessus de 60, le mécanisme reste entier. C'est pourquoi D6 est
classé latent et hors ligne uniquement, et non fermé.

### Pourquoi cela compte

`ANIMATION` **fait partie du hash canonique**. Un à-coup chez un seul pair, qui
regrouperait deux ticks dans une image, décalerait son horloge d'animation d'une
frame **définitivement**, et la partie serait déclarée désynchronisée.

`runtimeConfigSignature` (`src/port/netplay_runtime.cpp:800`) ne contient que le
délai d'entrée, le contexte, le drapeau partie complète et le drapeau rollback.
**La fréquence d'images n'y est pas.** Deux pairs peuvent donc se connecter avec
des réglages vidéo différents sans que rien ne le signale — et c'est sans
conséquence **uniquement grâce au bridage** : les deux simuleront à 60 quoi qu'ils
aient réglé. Cette omission et ce bridage sont liés. Si l'un disparaît, l'autre
doit disparaître aussi, ce que le sous-test `frame-rate-clamp` rend visible.

### Ce qu'il reste à mesurer, avant toute correction

Rien de tout cela n'a été observé, et **la mesure en ligne qui avait été prévue
n'a plus de sens** : voir la correction datée en tête d'entrée. Il reste :

1. hors ligne, instrumenter `simulatedTicks` au-dessus de 60 images par seconde
   et vérifier qu'une valeur `>= 2` se produit réellement. C'est la seule mesure
   qui puisse encore dire quelque chose sur ce mécanisme ;
2. en ligne, la seule vérification utile n'est plus de chercher D6 mais de
   prouver que le bridage tient dans le binaire livré : lancer deux minutes avec
   un pair réglé à 240 et constater que le détecteur reste muet **parce que le
   pacer a rendu 60**, ce que le sous-test `frame-rate-clamp` affirme et qu'une
   exécution réelle confirme.

Tant que le point 1 n'existe pas, ceci est un mécanisme lu, pas un défaut
constaté, et il est inscrit comme tel.

### Lien avec D5

Les deux défauts ont la même racine : **l'horloge d'animation est avancée par la
passe de présentation.** D5 en est la conséquence pour le rejeu, qui n'a pas de
passe de présentation ; D6 en est la conséquence pour deux machines dont les
cadences d'affichage diffèrent. Une correction qui déplace l'avance d'animation
dans le tick de simulation les fermerait toutes les deux.

---

## D7 — La préférence de vibration sauvegardée atteint un champ par joueur

**Classification : per-machine saved preference reaching per-player minigame
state, outside the canonical hash. Candidat — chemin établi par lecture,
occurrence non observée.**

**Statut : non corrigé, non observé. Trouvé en auditant les exclusions du hash
(`docs/canonical_hash_exclusions.md`).**

### Distinguer deux choses qui portent le même nom

**`RumbleBit`, l'état matériel, est neutralisé.** `src/port/netplay_runtime.cpp`
force, dans `PartyBoard_NetplayPreparePads` :

```cpp
*rumble = PAD_CHAN0_BIT | PAD_CHAN1_BIT;
```

une constante identique sur les deux pairs quelles que soient les manettes
branchées. `RumbleBit` fait partie de l'instantané de rollback
(`padSnapshotRegions`, `src/game/pad.c:106`) sans être haché, et **c'est ce
bridage qui rend cette absence inoffensive** — même structure que D6. Il est
désormais épinglé par la sonde PAD, qui vérifie la valeur produite par le chemin
en ligne réel, avec le netplay actif.

**`GWGameStat.rumble`, la préférence sauvegardée, ne l'est pas.** Elle est
locale à chaque machine, n'est pas hachée, et six modules la lisent :
`m428Dll:122`, `m442Dll:2282`, `m455Dll:817`, `m456Dll:471`, `m459dll:282`,
`option/rumble.c:51`.

### Ce qui est établi

Dans `m442Dll`, `fn_1_90FC()` retourne `GWGameStat.rumble` après avoir consulté
`HuPadRumbleGet()`, et `main.c:517` écrit ce retour dans un champ **par
joueur** :

```c
var_r30->unk_0C = fn_1_90FC();
```

Ce n'est donc pas seulement « faut-il appeler le moteur ». La valeur entre dans
une structure de joueur.

### Ce qui n'est pas établi

Si ce champ change le résultat du mini-jeu. Le dire exigerait de lire beaucoup
plus de ce module décompilé, ou de le mesurer. Tant que ce n'est pas fait, c'est
un chemin lu, pas un défaut constaté.

### Comment le mesurer, sans supposition

Aucun de ces six mini-jeux n'est dans la matrice, donc aucun n'a jamais tourné à
deux pairs. Le balayage des 58 mini-jeux de W2 les atteindra. **La mesure utile
n'est pas de les lancer, c'est de les lancer avec les deux pairs réglés
différemment** : un `GWGameStat.rumble` à 0 d'un côté, à 1 de l'autre. Si le
hash du sous-système du mini-jeu diverge, D7 est confirmé ; s'il ne diverge pas,
D7 est fermé sur ces six modules et sur eux seuls.

Lancer les deux pairs avec le même réglage produirait un vert qui ne prouve
rien — le piège exact de l'expérience D6 abandonnée.

---

## D8 — Les réglages de pause repartent d'un champ non haché vers des champs hachés

**Classification : unhashed field feeding hashed fields through a documented
code path. Candidat — chemin établi par lecture dans les deux sens, contexte
d'exécution non établi.**

**Statut : non corrigé, non observé. Trouvé au même endroit que D7.**

### Le chemin, dans les deux sens

`src/game/board/pause.c:154-165` recopie cinq réglages depuis `GWSystem` vers
`GWGameStat` :

```c
GWGameStat.story_pause.explain_mg = GWMGExplainGet();
GWGameStat.story_pause.show_com_mg = GWMGShowComGet();
GWGameStat.story_pause.mg_list = GWMGListGet();
GWGameStat.story_pause.mess_speed = GWMessSpeedGet();
GWGameStat.story_pause.save_mode = GWSaveModeGet();
```

et `src/REL/modeseldll/main.c:212-221` les réinjecte :

```c
GWMGExplainSet(GWGameStat.party_pause.explain_mg);
```

Or `GWSystem.explain_mg`, `show_com_mg`, `mg_list`, `mess_speed` et `save_mode`
**sont hachés** (`include/port/netplay_canonical.hpp`). `GWGameStat.story_pause`
et `party_pause` ne le sont pas.

Le chemin complet est donc : réglage local → champ non haché → retour dans un
champ haché.

### Pourquoi cela compte plus que les autres candidats

C'est le seul champ exclu du hash dont un chemin **vers le hash** soit établi par
lecture. Les autres candidats de `docs/canonical_hash_exclusions.md` divergent
dans leur coin ; celui-ci peut faire diverger le hash lui-même.

Et il le ferait de la pire manière : deux joueurs dont les réglages de pause
diffèrent produiraient un `mismatch` sur le sous-système `Gamework` **sans
qu'aucune action de jeu ne l'explique**. Le rapport de divergence nommerait le
bon champ et la cause serait dans un menu visité avant la partie.

### Ce qui n'est pas établi

Si `modeseldll` s'exécute dans un contexte en ligne. C'est la seule question, et
elle se tranche par lecture ou par une trace, sans temps machine.

### À faire avant W3

Deux humains sur deux machines auront des réglages de pause différents, parce
que rien ne les harmonise. C'est donc un candidat que la première vraie session
peut déclencher, et il vaut mieux savoir avant qu'après.
