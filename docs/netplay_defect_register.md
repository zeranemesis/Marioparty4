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

---

## D9 — Un plantage réel n'a produit aucun rapport — **CAUSE ÉTABLIE ET PROUVÉE**

**Classification : crash reporter silent because its guard pages were never
armed. Cause établie par lecture, puis démontrée par l'expérience.**

**Statut : l'instrument est désormais armé par défaut. Aucune ligne de code du
rapporteur n'a été modifiée — il n'était pas cassé, il était éteint.**

### La cause

`PARTYBOARD_STACK_WATCHDOG` commande les pages de garde des piles de coroutine,
et **aucun script ne le posait**. `src/port/coroutine_stack.cpp` dit exactement
ce que cela coûte, dans un commentaire écrit le jour où le mécanisme a été
construit :

> A reserved page gives a plain access violation, and the faulting instruction
> is the push of a return address: the kernel then cannot push an exception
> frame either, so no handler ever runs and the process dies silently.

Non armé, le garde bas est **une page RÉSERVÉE** et c'est la première branche
qui s'applique. Armé, ce sont **quatre pages ENGAGÉES** avec `PAGE_GUARD`, qui
lève `STATUS_GUARD_PAGE_VIOLATION` *et* efface son propre bit de garde dans le
même geste, laissant au fil assez de pile pour parler.

### La preuve, le même scénario dans les deux états

Correctif D4 annulé dans les deux cas, `d4-big-boo`, un run complet chacun.

| | garde **éteinte** | garde **armée** |
|---|---|---|
| résultat | CRASH frame 48671 | CRASH frame 48685 |
| rapport de plantage | **aucun** | **58 042 octets** |
| relevé de piles | **aucun** | **7 211 octets** |
| empreinte | *(aucune)* | `STACK_OVERFLOW:overlay92:fn_1_30A4` |
| piles gardées | 0 | 16 |

Et ce que le rapport dit, là où il n'y avait rien :

```
reason=COROUTINE_STACK_OVERFLOW
COROUTINE STACK OVERFLOW at frame 48685, faulting address 0x264ea433ff8:
GUARD PAGE BELOW the coroutine stack of fn_1_30A4:
stack 0x264ea434000-0x264ea436000, size 8192 (game constant 4096 doubled by process.c),
faulting address is 8 bytes past the bottom of the stack, peak observed 7936
```

**« 8 octets au-delà du bas de la pile »** — exactement les huit octets que
l'entrée D4 décrit, l'adresse de retour d'un `call`. Le rapporteur ne produit
donc pas seulement *un* rapport : il produit **le** rapport, celui qui aurait
identifié D4 en quelques minutes au lieu de l'enquête que D4 raconte.

### Ce que cela apprend au-delà de D9

Trois instruments ont été trouvés dans le même état le même jour — détecteur
audio, intégrité du tas, pages de garde — existants, fonctionnels, et armés par
aucun script. **Un interrupteur de diagnostic par défaut éteint sera éteint le
jour où il aurait servi.** La règle appliquée aux trois est désormais la même :
la campagne les arme, et un détecteur demandé sans preuve qu'il a tourné vaut
`HARNESS_FAILURE`, jamais `PASS`.

### Ce qui reste ouvert

Le coût. Quatre pages engagées par coroutine au lieu d'une page réservée, sur
seize piles observées ici. Ce n'est pas mesuré sur une partie longue, et c'est
la seule raison qui pourrait justifier de ne pas l'armer partout.

L'ancienne recette de reproduction reste valable et vaut d'être conservée : elle
est le premier plantage moteur de ce projet reproductible **à la frame près et à
volonté**, et c'est un banc d'essai, pas seulement un souvenir.

---

## D9 — annexe : la recette de reproduction

### Ce qui a été observé

Le 2026-09-11, en annulant délibérément le correctif D4 pour prouver que son
scénario de non-régression rougit bien (`docs/regression_proofs.md`), les deux
pairs sont morts à la frame 48671 :

```
exit_peer_0 = 0xC0000005 (-1073741819)   EXCEPTION_ACCESS_VIOLATION
exit_peer_1 = 0xC0000005 (-1073741819)   EXCEPTION_ACCESS_VIOLATION
classification = PROCESS_CRASH / PROCESS_CRASH
```

C'est exactement la signature D4 attendue avec page de garde. Mais le dossier du
run ne contient **ni rapport de plantage, ni minidump, ni fichier
`stack-usage-*`** — seulement les journaux, l'état vivant et la trace audio. Le
répertoire de travail du binaire n'en contient pas davantage.

### Pourquoi cela compte, indépendamment de D4

D4 est corrigé : ce plantage précis ne peut pas se produire dans un binaire livré.
Ce qui reste, et qui n'est pas corrigé, c'est que **le rapporteur a été muet sur
un plantage réel**. Tout le circuit de télémétrie — empreintes stables,
déduplication, file d'incidents, consentement, export — suppose qu'un plantage
produit un rapport. Ici il n'en a pas produit, et personne ne l'aurait su : le run
était classé `CRASH`, ce qui est correct, mais **sans empreinte**, donc
impossible à regrouper avec une récidive. Chaque occurrence aurait ressemblé à un
défaut neuf.

L'entrée D4 dit elle-même que ce mode de défaillance détruit ses propres preuves,
et que la sortie fut la sémantique `PAGE_GUARD`, qui lève une exception *et*
débloque la page en un seul geste. Le code de sortie `0xC0000005` indique que la
page de garde **a bien fauté**. Le rapporteur aurait donc dû disposer de pile.
Pourquoi il n'a rien écrit reste à établir.

### Ce qui n'est pas établi

Trois hypothèses, aucune vérifiée :

1. le gestionnaire n'était pas installé sur le fil qui a fauté ;
2. il s'est exécuté et a échoué à écrire (chemin, droits, fil d'écriture non
   démarré) ;
3. la faute s'est produite dans un état où le dispatch vers le fil d'écriture ne
   pouvait pas aboutir.

**Ne pas corriger sur l'une de ces hypothèses.** Instrumenter d'abord.

### Recette de reproduction

Elle existe, elle est déterministe, et elle coûte quatorze minutes :

1. `src/game/process.c` : remplacer `stack_size *= 4;` et le plancher 32768 par
   `stack_size *= 2;`
2. reconstruire
3. `tools\netplay_campaign.ps1 -DiscPath <iso> -Manifest tests/scenarios/regression-proofs.json -Scenario d4-big-boo -Repeat 1`
4. le plantage tombe à la frame 48671, sur les deux pairs

C'est la première fois du projet qu'un plantage du moteur est reproductible **à
la frame près et à volonté**. C'est le banc d'essai dont le rapporteur a besoin,
et il faut s'en servir avant d'y toucher.

### Atténuation déjà en place

La campagne attribue désormais une empreinte à un plantage muet :
`NO_REPORT:<nom d'exception>:overlay<N>`, construite uniquement à partir du nom
de l'exception et du contexte de jeu — jamais d'un PID, d'une adresse ASLR, d'un
horodatage ni d'un handle. L'**absence** de rapport fait partie de la signature :
deux plantages qui rendent le rapporteur muet dans le même overlay sont bien plus
probablement un seul défaut que deux. Ce n'est pas un correctif, c'est ce qui
permet de compter les occurrences en attendant.

---

## D10 — Plantage intermittent dans la fusion de blocs de `HuMemMemoryFree`

**Classification : access violation reading a free-list neighbour during block
coalescing, at startup. Intermittent, observé une fois sur 27 exécutions du
même fichier d'entrée.**

**Statut : non corrigé, non expliqué. Un rapport complet existe.**

### Ce qui a été observé

```
EXCEPTION_ACCESS_VIOLATION
HuMemMemoryFree+0x115  [src/game/memory.c:154]
simulation_frame=2  game_context=-1  overlay=1  board=0 turn=0
```

`src/game/memory.c:154` est dans le chemin de fusion des blocs libres :

```c
if (block->next > block && !block->next->flag) {
    PartyBoard_MemDiagOnRetire(block->next);
    block->next->next->prev = block;      /* <- ici */
```

La faute est une **lecture** : `block->next` pointait sur une zone illisible, donc
`block->next->next` a fauté. Le voisin dans la liste libre est invalide.

C'est à la **frame 2**, pendant le chargement, sur un seul des deux pairs.

### Comment il a été trouvé, et ce que cela dit

Par accident, pendant un balayage du menu sans rapport. L'impulsion injectée
était à la frame 4160 et n'a jamais été appliquée — le processus était mort 4158
frames plus tôt. **L'entrée n'y est pour rien.**

C'est aussi le premier plantage neuf sur lequel le rapporteur produit un rapport
complet, parce que la garde de pile a été armée le même soir (voir D9). Sans
elle, ce défaut aurait été un `0xC0000005` muet de plus.

### Ce que le détecteur de tas n'a pas vu, et pourquoi cela compte

`PARTYBOARD_MEM_DIAGNOSTICS` était **armé** : dix tas enregistrés, balayage
propre à la frame 0, `mem_corruption_detected = false`. Le détecteur n'a donc
rien vu du bloc qui a fauté deux frames plus tard.

Deux explications possibles, et rien ne permet encore de choisir :

1. la corruption survient **entre deux balayages** — le balayage tourne une fois
   par tick accepté, et tout peut arriver à l'intérieur d'un tick ;
2. le bloc libéré **n'appartient à aucun tas surveillé** — `HuMemDirectMalloc`
   et les allocations précoces ne passent pas forcément par un tas enregistré au
   moment où elles sont faites.

La seconde serait la plus utile à savoir : elle voudrait dire que le détecteur a
un angle mort au démarrage, précisément là où ce défaut vit.

### Le taux, et pourquoi cinq verts ne referment rien

| | |
|---|---|
| occurrences | **1** |
| exécutions du même fichier | **27** (22 pendant le balayage, 5 en répétition dédiée) |

Cinq répétitions du fichier exact ont toutes réussi. **Cela ne referme pas le
défaut.** Un défaut qui se manifeste une fois sur vingt-sept n'est pas écarté par
cinq exécutions vertes ; c'est exactement ce que la règle du projet sur les
défauts probabilistes interdit de conclure.

### Ce qu'il faut faire, dans l'ordre

1. **Ne pas corriger.** Rien n'explique encore la cause, et `memory.c` est le
   chemin le plus fréquenté du jeu.
2. Établir le taux réel : le démarrage est traversé par **chaque** run, donc
   toute campagne l'échantillonne gratuitement. Compter les occurrences plutôt
   que lancer un stress dédié.
3. Trancher l'angle mort du détecteur : vérifier si le bloc fauté appartenait à
   un tas enregistré au moment de la faute. Si non, le détecteur doit couvrir le
   démarrage, et c'est un correctif de l'instrument, pas du jeu.
4. Le plantage est sur **un seul** pair. Les deux exécutent la même entrée, donc
   ce qui diffère est l'ordonnancement, pas la logique — un indice de course
   plutôt que de logique déterministe.

### Pourquoi ce défaut compte plus que son taux

Il est au **démarrage**. Chaque session le traverse, et une session humaine de
deux heures qui meurt à la frame 2 coûte la soirée de deux personnes. Un défaut
rare sur un chemin emprunté une seule fois par run reste rare ; celui-ci est rare
sur un chemin emprunté par tout le monde, à chaque fois.

---

## D11 — Divergence `ANIMATION` à la deuxième entrée sur un plateau

**Classification : single-subsystem canonical divergence in ANIMATION, one frame,
under netplay at 60 Hz. Observé une fois. D6 écarté par la mesure.**

**Statut : non corrigé. Premier défaut trouvé par entrée générée.**

### L'isolation

| frame | état |
|---|---|
| 14421 | **identique sur les seize sous-systèmes** |
| 14422 | **seul `ANIMATION` diffère** — local `0a9ed93d`, distant `74807821` |

Une frame, un sous-système. Tout le reste — `RNG`, `GAMEWORK`, `PLAYERS`,
`BOARD`, `OBJECTS`, `PROCESSES`, `INPUT`, `TIMERS` — concorde des deux côtés à la
frame de la divergence. Les entrées sont donc identiques et c'est bien l'horloge
d'animation qui diverge, pas ce qui la pilote.

### D6 est écarté, et par la mesure et non par l'argument

| | |
|---|---|
| `d6_batched_frames` | **0** |
| `d6_worst_batch` | **0** |
| cadence des deux pairs | **60 / 60** |
| lignes `D6>` imprimées | **0** |

Aucun regroupement de ticks de simulation dans une image rendue. Le bridage de
`target_frame_rate()` a tenu exactement comme l'entrée D6 l'affirme. **Ce défaut
est autre chose.**

### Ce qui rend ce run différent de tous les précédents

Son chemin d'overlays :

```
1@2  74@840  70@2939  95@3869  70@4206  89@7586
                       ^^^^^^          ^^^^^^
                    Tutorial          Toad's
```

Le run **entre sur un plateau, en ressort vers le menu, puis entre sur un
second**. Aucune session de ce projet n'avait jamais fait cela — tous les
enregistrements existants entrent sur un plateau et y restent.

C'est aussi exactement le déplacement que Valentin avait suggéré (quitter par le
menu pour aller sur un autre plateau), obtenu ici par un singe.

### L'hypothèse, et elle n'est pas vérifiée

L'état d'animation ne serait pas entièrement réinitialisé entre deux entrées de
plateau, laissant à la seconde un reliquat qui peut différer entre les deux
pairs. Le défaut n'apparaîtrait donc **que** sur une deuxième entrée, ce qui
expliquerait qu'aucun enregistrement ne l'ait jamais montré.

**Rien ne l'établit encore.** C'est une hypothèse tirée d'une seule occurrence et
d'une particularité du chemin ; elle peut être fausse.

### Ce qu'il faut faire

1. ~~Rejouer la même graine.~~ **Fait, et la réponse est : intermittent.**

   | | |
   |---|---|
   | occurrences | **1** |
   | exécutions du fichier exact | **7** |

   Les six répétitions ont toutes réussi, et elles sont allées **plus loin** que
   le run divergent — environ 19 100 frames contre 14 426. Même fichier d'entrée,
   mêmes frames, six fois sur sept sans divergence.

   Cela réoriente le diagnostic : **une course, pas une faute de logique**. Une
   divergence logique sur une entrée identique se reproduirait à la frame près,
   comme D5 le faisait à sa première occasion. Le trafic observé au moment de
   l'incident va dans le même sens — `rejected=12474` sur `received=16376`, soit
   **76 % de paquets rejetés**, et `remote_ready=0` à l'arrêt.

   Ce qui reste à expliquer est alors : *qu'est-ce qui, dans l'ordonnancement,
   peut faire avancer l'horloge d'animation d'un pair et pas de l'autre ?* Et
   l'hypothèse de la deuxième entrée de plateau reste compatible avec une
   course : un reliquat non réinitialisé peut n'être lu que dans une fenêtre
   étroite.

   **Six verts ne referment pas ce défaut**, pas plus que les cinq de D10.
2. **Construire un scénario minimal** : entrer sur un plateau, ressortir,
   entrer sur un second. S'il diverge de façon répétée, le défaut est cerné sans
   dépendre d'un singe.
3. **Ne pas corriger avant.** L'entrée D5 a déjà déplacé l'avance d'animation ;
   y toucher de nouveau sur une seule observation risquerait de casser ce qui
   marche.

### Pourquoi cette occurrence compte au-delà d'elle-même

C'est le **premier défaut que la campagne automatique trouve seule**. Les
enregistrements humains existants ne pouvaient pas le produire : ils n'entrent
jamais deux fois sur un plateau. Cela valide l'ensemble de la démarche — une
entrée générée atteint des chemins qu'aucun enregistrement ne contient.

---

## D12 — Divergence `OBJECTS` à l'instant du chargement d'un mini-jeu

**Classification : single-subsystem canonical divergence in OBJECTS, one frame,
exactly at the frame a minigame overlay loads. Observé une fois.**

**Statut : non corrigé. Distinct de D11 : autre sous-système, autre chemin.**

### L'isolation

| frame | état |
|---|---|
| 17778 | **identique sur les seize sous-systèmes** |
| 17779 | **seul `OBJECTS` diffère** — local `cfeb71a2`, distant `83b6f114` |

### Le chemin, et pourquoi il compte

```
70@2904  →  89@11305  →  3@17492  →  15@17780
menu        Toad's       instructions   BATTANDOMINO
```

La divergence tombe à la frame **17779**, soit **une frame avant** que l'overlay
du mini-jeu n'apparaisse. C'est donc l'instant de la transition elle-même :
l'écran d'instructions se retire et le mini-jeu se charge.

`D6` est écarté ici aussi : `d6_batched_frames = 0`.

### Ce qui le distingue de D11

| | D11 | D12 |
|---|---|---|
| sous-système | `ANIMATION` | `OBJECTS` |
| moment | en cours de jeu sur un plateau | à la transition vers un mini-jeu |
| chemin particulier | deuxième entrée de plateau | entrée de mini-jeu |

Rien n'interdit qu'ils aient une cause commune — tous deux sont des divergences
d'un seul sous-système sur une transition — mais les traiter comme un seul défaut
sans preuve serait la faute que ce registre existe pour empêcher.

### Le taux

**Deux occurrences**, et elles ont été trouvées par deux lots différents :

| run | empreinte | contexte |
|---|---|---|
| `night-6022` | `DESYNC:OBJECTS:unknown` | frame 17779, chargement de BATTANDOMINO |
| `survey-3101` | `DESYNC:OBJECTS:unknown` | lot de sondage, même signature |

Deux occurrences indépendantes de la même signature en une nuit, alors que D11
n'en a qu'une : **D12 est probablement le plus fréquent des trois défauts
trouvés cette nuit.** Comme les autres il reste intermittent, et un run vert ne
dit rien.

### Ce qu'il faut faire

1. Accumuler des occurrences. Chaque run qui entre dans un mini-jeu échantillonne
   ce chemin gratuitement ; c'est le balayage des mini-jeux qui fournira les
   statistiques, sans campagne dédiée.
2. Quand il y en aura assez, regarder **quel objet** diverge. `OBJECTS` est un
   agrégat ; le rapport de divergence ne nomme pas encore le champ fautif, et
   c'est la première chose à instrumenter avant toute correction.
3. **Ne pas corriger.** Une seule occurrence, un agrégat, aucune cause établie.

---

## D14 — la fin de la vidéo d'intro n'arrive pas sur la même frame

**Classification : non-determinism in a gameplay-blocking wait, observed between
two physical machines, surfacing as a `SCENE` state divergence.**

**Statut : CAUSE TROUVEE ET CORRIGEE le 2026-09-12 a 23h10. Vu rouge puis vert
sur le meme scenario, avec la chaine mesuree de bout en bout.**

**Niveau de preuve : REAL-NETWORK.** C'est la première divergence du projet
observée entre deux machines physiques, et elle n'était pas reproductible en
boucle locale.

### Ce qui a été observé

Session du 2026-09-12, 10:07:31 → 10:08:01. Hôte et client sur deux PC, même
paquet (`ff2a8040…6b1651c`), même disque, ping 15–17 ms, **0 paquet perdu, 0
erreur de socket**.

```
DESYNC frame=1804 category=SCENE context=74/74 counter=1804/1804
```

Quinze sous-systèmes sur seize sont identiques au bit près à cette frame, RNG et
entrées compris. Les deux rapports sont exactement symétriques :

| frame | SCENE hôte | SCENE client |
|---|---|---|
| 1801 | `4fde85d4` | `4fde85d4` |
| 1803 | `4fde85d4` | `4fde85d4` |
| **1804** | **`2feec2b4`** | `4fde85d4` |

Seul l'hôte a bougé. Son vidage de champs au moment de la détection :

```
wipeData.mode = 2 (WIPE_MODE_OUT)   stat = 1
wipeData.time = 0.0                 duration = 10.0
couleur = 255/255/255
```

Un fondu blanc de dix frames qui **vient d'être créé**. Le client ne l'avait pas
encore créé.

Aucune touche n'était pressée sur aucun des deux PC des frames 1797 à 1808 : le
déclencheur n'est pas une entrée.

### Le chemin, établi par lecture

Les deux pairs entrent dans l'overlay 74 (`modeseldll`) à la **frame 872
exactement**, avec le même `mode_select online=1 menu_event=0 skip_file=1`. La
divergence est donc entièrement contenue dans les 932 ticks qui suivent.

`src/REL/modeseldll/modesel.c:207` :

```c
while (!HuTHPEndCheck()) {
    ...
    HuPrcVSleep();
}
_ClearFlag(FLAG_ID_MAKE(1, 11));
WipeColorSet(255, 255, 255);
WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 10);
```

Le fondu est déclenché par la **fin de la vidéo THP d'intro**, et par rien
d'autre. Couleur et durée concordent avec ce site d'appel et avec aucun autre :
`modeseldll/main.c` ne crée que des fondus noirs de durée 20.

### Ce qui rend ce défaut instructif

**Le problème était déjà connu et déjà corrigé.** `src/port/thp_player.cpp:203`
porte ce commentaire :

> *les modules bloquent sur la fin du film, donc deux machines quittaient un
> film sur des frames différentes et demandaient ensuite des fondus d'écran
> différents*

Le correctif fait dériver la position du film d'un compteur de ticks simulés
(`logical_frame()`) au lieu du curseur audio, dès que le netplay est actif, et
il a son propre test. Vérifié par lecture :

- `PartyBoard_ThpLogicalTick()` est appelé depuis `PadReadSimulationTick`
  (`src/game/pad.c:385`), **après** la porte netplay, donc une seule fois par
  tick accepté ;
- l'index des frames du film est construit intégralement à l'ouverture depuis
  l'en-tête du fichier, donc `frames.size()` et `fps` sont identiques des deux
  côtés (même disque, SHA-256 vérifié) ;
- `HuTHPSprCreateVol` est appelé depuis la logique de jeu, donc sur le même tick.

**Le taux est donc corrigé, et pourtant les deux pairs ne sont pas sortis du
film ensemble.** Il reste une différence dans le *nombre de ticks effectivement
comptés* — `PartyBoard_ThpLogicalTick()` ne compte que si `g_movie` existe — ou
dans un chemin qui contourne la porte. Le client rapporte `repaired=7` là où
l'hôte rapporte `repaired=0` ; ce n'est pas une explication, c'est la seule
asymétrie relevée entre les deux journaux.

### Pourquoi il a fallu 932 frames pour le voir

**La position du film n'est pas hachée.** Aucun des 2 211 champs du hachage
canonique ne la contient. Les deux pairs pouvaient donc s'écarter dès la frame
872 sans que rien ne le dise, jusqu'à ce que l'écart produise un effet dans un
champ haché — le fondu, 932 frames plus tard.

C'est exactement ce que `docs/canonical_hash_exclusions.md` existe pour prévoir :
ce qui est exclu du hachage est ce dont le hachage ne peut rien dire.

### Ce qu'il faut faire, dans l'ordre

1. **Mesurer avant de corriger.** Publier `logicalTicks`, `playback_frame()` et
   la frame de simulation à laquelle `g_movie` a été créé dans le diagnostic
   natif des deux pairs, puis rejouer la même approche. La comparaison dit
   immédiatement si l'écart est un décalage constant pris au démarrage ou une
   dérive accumulée.
2. **Hacher la position logique du film**, pour que la prochaine divergence de
   cette famille soit signalée à la frame où elle naît et nommée, au lieu de
   remonter 932 frames plus tard sous une autre catégorie.
3. Seulement ensuite, corriger. La piste à évaluer est de dériver la position du
   film de la frame réseau elle-même — identique aux deux pairs par
   construction — plutôt que d'un compteur d'appels.

### Contournement immédiat

Passer la vidéo d'intro avec une touche. Un saut est une entrée, donc en
lockstep, donc les deux pairs quittent le film sur la même frame. Non vérifié.

---

## D15 — `omAddMember` laisse `object->group` non initialisé quand le groupe est plein

**Classification : use of an uninitialised struct field, producing a
non-deterministic canonical hash and an out-of-bounds array index on
destruction.**

**Statut : non corrigé. Cause racine établie par lecture du code et mesurée sur
les deux pairs.**

**Niveau de preuve : REAL-NETWORK.** Trouvé en comparant les vidages de champs
des deux machines de la session du 2026-09-12 10:32.

### Le code

`src/game/objmain.c:365` :

```c
void omAddMember(Process *objman_process, u16 group, omObjData *object)
{
    omObjMan *objman = objman_process->user_data;
    omObjGroup *group_ptr = &objman->group[group];
    if (group_ptr->num_objs != group_ptr->max_objs) {
        object->group = group;          /* seule ecriture du champ */
        object->group_idx = group_ptr->next_idx;
        ...
    }
    /* pas de else */
}
```

Et `omAddObjEx` (`src/game/objmain.c:303`) n'ecrit `object->group` que dans
l'autre branche :

```c
if (group >= 0) {
    omAddMember(objman_process, group, object);
} else {
    object->group = group;
    object->group_idx = 0;
}
```

**Quand `group >= 0` et que le groupe est plein, personne n'ecrit
`object->group`.** L'objet vient d'etre pris dans le pool
(`object = &obj_base[next_idx]`), donc le champ garde le contenu de
l'emplacement recycle.

### La mesure

Desync du 2026-09-12, frame 16382, overlay 30 (`m422Dll`, BELCON COIN),
`category=OBJECTS`. La comparaison des deux rapports donne **un seul champ
different sur 2007** :

```
681  OBJECTS  object->group   000001e0 (480)  contre  00000185 (389)
```

`OM_MAX_GROUPS` vaut **10** : les valeurs valides sont 0 a 9, ou -1. Ni 480 ni
389 n'en font partie. Les **sept autres objets** du meme vidage portent tous
`ffffffff`, le sentinelle « aucun groupe ».

Et la table des groupes de ce gestionnaire, des deux cotes :

```
groupe 0..9 : max_objs=0  num_objs=0  next_idx=0
```

**Les dix groupes ont une capacite nulle.** `num_objs != max_objs` est donc faux
pour tous, et `omAddMember` ne fait jamais rien sur ce gestionnaire. Tout objet
cree avec un `group >= 0` en ressort avec un champ non initialise.

Les deux pairs ont des historiques d'allocation differents, donc des restes
differents : 480 d'un cote, 389 de l'autre. Le hachage canonique les voit et
arrete la partie.

### La consequence qui depasse le determinisme

`src/game/objmain.c:424` :

```c
void omDelMember(Process *objman_process, omObjData *object)
{
    if (object->group != -1) {
        omObjGroup *group = &objman->group[object->group];
        group->obj[object->group_idx] = NULL;
        group->next[object->group_idx] = group->next_idx;
        ...
    }
}
```

Avec `object->group == 480` et `objman->group` alloue a **dix** elements
(`HuMemDirectMallocNum(HEAP_SYSTEM, OM_MAX_GROUPS * sizeof(omObjGroup))`),
c'est une **ecriture hors bornes** loin apres la fin du tableau, dans le tas
systeme.

Ce n'est donc pas seulement un defaut de determinisme : c'est une corruption de
tas latente, de la meme famille que D1.

**Ce qui n'est PAS etabli** : que ce chemin ait ete emprunte lors du
`STATUS_HEAP_CORRUPTION` de S1, ou qu'il explique D10. Les deux sont plausibles
et aucun n'est demontre. On ne relie pas deux defauts sans preuve.

### Le correctif envisage, et pourquoi il est sur

Initialiser le champ **avant** la tentative d'insertion, dans `omAddObjEx` :

```c
object->group = -1;
object->group_idx = 0;
if (group >= 0) {
    omAddMember(objman_process, group, object);
}
```

Un objet dont l'insertion echoue devient alors proprement « sans groupe » au
lieu de porter un reste. Le cas ou l'insertion reussit est inchange, puisque
`omAddMember` ecrit les deux champs lui-meme. Aucun comportement existant n'est
modifie ; seul un comportement indefini le devient.

### Ce qu'il faut faire, dans l'ordre

1. Un test deterministe qui cree un gestionnaire avec un groupe de capacite
   nulle, appelle `omAddObjEx` avec `group = 0`, et exige `object->group == -1`.
   Il doit **rougir** sur le code actuel.
2. Le correctif, dans un commit separe.
3. Rejouer BELCON COIN et exiger que la divergence ait disparu.
4. Chercher **pourquoi** ce gestionnaire a dix groupes de capacite nulle : soit
   les groupes ne sont jamais dimensionnes sur ce chemin, soit l'appelant
   demande un groupe qui n'a jamais ete prevu. Le correctif rend le symptome
   inoffensif ; il ne repond pas a cette question.

---

## D16 — la comptabilite du tas diverge pendant un mini-jeu

**Classification : divergent heap allocation bookkeeping between two peers,
detected by the `HEAPS` subsystem of the canonical hash.**

**Statut : non corrige. Deux occurrences, sur deux tas differents. On ne sait
pas encore s'il s'agit d'un seul defaut ou de deux.**

**Niveau de preuve : SCRIPTED.** Trouve par le balayage automatique des
mini-jeux du 2026-09-12, en boucle locale. Aucune session a deux machines n'a
encore rencontre ce chemin.

### Les deux occurrences

| | occurrence 1 | occurrence 2 |
|---|---|---|
| mini-jeu | overlay 15, `m407Dll` BATTANDOMINO | overlay 17, `m409Dll` CRAY SHOT |
| frame | 20464 | 38125 |
| tas | **2, `HEAP_DATA`** (44 Mo) | **0, `HEAP_SYSTEM`** (9 Mo) |
| blocs | 7155 contre 7151, ecart **4** | 204 contre 212, ecart **8** |
| octets | 12 735 360 contre 12 734 720, ecart **640** | 380 320 contre 385 344, ecart **5024** |
| par bloc | 160 octets, exactement | 628 octets en moyenne |
| champs differents | 2 sur 26 376 | 2 sur 3 709 |

Dans les deux cas **rien d'autre ne differe**. Ni le RNG, ni les entrees, ni les
objets, ni les compteurs : un pair a simplement alloue plus de blocs que
l'autre, et le hachage canonique le voit.

### Une troisieme occurrence, et un rapport qui se repete

Le 2026-09-12 a 20h10, `partie-complete-0` a diverge en `HEAPS` a la frame
32170, dans **overlay 23, `m415Dll` PYONPYON STAMP**, sur le **tas 0,
`HEAP_SYSTEM`** :

| | valeur |
|---|---|
| blocs | 67 contre 74, ecart **7** |
| octets | 596 448 contre 611 808, ecart **15 360** |
| champs differents | **2 sur 2300** — les deux compteurs du meme tas, rien d'autre |

Le meme mini-jeu avait deja ete releve le meme jour avec un ecart de **28 blocs
et 61 440 octets**. Or 28 = 4 x 7 et 61 440 = 4 x 15 360 : **le meme groupe
d'allocations, repete quatre fois dans un cas et une fois dans l'autre.**

C'est le premier element qui donne une forme a ce defaut. Un ecart de blocs qui
se reproduit a l'identique, par multiples entiers d'une meme unite de 7 blocs et
15 360 octets, ne ressemble pas a du bruit d'allocateur : il ressemble a une
sequence d'allocations executee un nombre de fois different sur chaque pair.

Cette occurrence partage le tas de l'occurrence 2 (`HEAP_SYSTEM`) mais pas le
mini-jeu. Elle ne tranche donc toujours pas la question de savoir s'il s'agit
d'un ou de plusieurs defauts, et elle ne doit pas etre lue comme si elle le
faisait.

**L'hypothese du detecteur, examinee puis ecartee par lecture.** La campagne
arme `PARTYBOARD_MEM_DIAGNOSTICS=1`, et son balayage a coute jusqu'a **202 ms**
sur ce run, contre un budget d'image de 16,67 ms. Un detecteur qui allouerait
dans `HEAP_SYSTEM` fabriquerait lui-meme la divergence qu'il sert a trouver.
Verification faite : `PartyBoard_MemDiagSweep` (`src/port/mem_diagnostics.cpp:485`)
ne fait que parcourir les tas, son `SweepContext` est sur la pile, et les seules
allocations du module sont dans son auto-test, sur une arene obtenue par
`std::malloc`. **Il n'alloue rien dans les tas du jeu.**

Ce qui reste, et qui n'est pas elimine : une image de 202 ms peut deplacer tout
ce qui depend de l'horloge murale plutot que du tick simule. Un run de controle
avec `-MemDiagnostics ''` reste donc utile, mais comme second chemin de
verification et non comme prealable.

### Le quantum de 160 octets, vu une deuxieme fois et ailleurs

`video-00`, 2026-09-12 a 23h28, sur le binaire portant le correctif D14 :
divergence `HEAPS` a la frame 6649, **overlay 89, `w01Dll` — le plateau**, sur le
**tas 2, `HEAP_DATA`** :

| | valeur |
|---|---|
| blocs | 3448 contre 3450, ecart **2** |
| octets | 13 670 496 contre 13 670 816, ecart **320** |
| par bloc | **160 octets, exactement** |

L'occurrence 1 de ce defaut donnait **4 blocs de 160 octets** dans BATTANDOMINO.
Celle-ci en donne **2**, sur un plateau, dans un module sans rapport. Le meme
quantum dans deux contextes qui n'ont rien en commun : ce n'est pas la meme
fonction appelee deux fois, c'est **la meme taille d'objet**.

`MEM_ALLOC_SIZE` arrondit a 32 octets pres apres avoir ajoute l'en-tete, donc un
bloc de 160 octets correspond a une demande d'environ 97 a 160 octets — un petit
descripteur. `HEAP_DATA` recoit ce qui est lu du disque : modeles, animations,
sprites. Un descripteur d'animation ou de sprite cree une ou deux fois de plus
d'un cote correspond exactement a cette signature.

C'est la piste a suivre pour D16, et elle est plus etroite que « la comptabilite
du tas diverge » : il s'agit de trouver **quel descripteur de 160 octets** est
alloue un nombre de fois different.

### Ce qui les distingue, et pourquoi ils restent separes

Ce ne sont pas les memes tas. `HEAP_DATA` recoit les donnees lues du disque —
modeles, animations, sprites — et `HEAP_SYSTEM` recoit les structures du
moteur. Une cause qui expliquerait l'un n'expliquerait pas forcement l'autre, et
les reunir sous un seul defaut sur la seule foi de leur categorie commune serait
exactement le raccourci que ce registre interdit.

L'ecart de 160 octets **exactement** quatre fois de suite, dans la premiere,
n'est pas du bruit d'allocateur : ce sont quatre allocations identiques faites
d'un cote et pas de l'autre.

### Une observation a ne pas confondre avec une preuve

Avant le correctif D15, BATTANDOMINO divergeait a la frame 20464. Apres, le meme
marcheur l'a traverse entierement (3397 frames) et n'a diverge que deux
mini-jeux plus loin.

Un mecanisme relierait les deux : `omDelMember` indexait
`objman->group[object->group]` avec une valeur non initialisee sur un tableau de
dix elements, donc ecrivait **dans le tas**, et pouvait corrompre la
comptabilite de l'allocateur differemment sur chaque pair. C'est plausible et
ce n'est pas demontre. Une occurrence disparue apres un correctif est une
observation, pas une causalite.

### Ce qu'il faut faire, dans l'ordre

1. **Instrumenter l'allocateur**, comme la boucle de curseur l'a ete cet
   apres-midi : tracer chaque allocation du tas concerne avec sa taille et son
   appelant, sur les deux pairs, et comparer. Quatre blocs de 160 octets se
   retrouvent dans une trace.
2. Rejouer BATTANDOMINO sur le binaire corrige de D15, plusieurs fois, pour
   savoir si l'occurrence 1 a vraiment disparu ou si elle est intermittente.
3. Seulement ensuite decider s'il s'agit d'un ou de deux defauts.


---

## D17 — le marcheur choisit le mode de jeu et le plateau par accident

**Etat : CORRIGE ET VERIFIE le 2026-09-12 a 20h53.**
**Portee : outillage de test uniquement. Aucun effet sur une partie jouee par un
humain.** Il est inscrit ici parce qu'il a fausse la couverture rapportee, ce qui
est exactement le genre de defaut que ce registre existe pour empecher.

### Ce qui a ete mesure

Le rythme du marcheur (`netplay_runtime.cpp`) envoie trois frames de stick
horizontal toutes les 240 frames, en aveugle. Le menu des modes (`modeseldll`,
contexte 74) dispatche sur un curseur : 0 = Fete, 1 = Histoire, 2 = Mini-jeux
(`src/REL/modeseldll/main.c:210-243`).

Le nombre de crans qui tombe dans ce menu ne depend donc que de sa duree
d'ouverture, et cette duree bouge avec l'entree qui precede. Deux campagnes
lancees le meme soir, sur le meme binaire, a trois minutes d'intervalle :

| run | frames dans `modeseldll` | curseur final | mode | `max_turn` |
|---|---|---|---|---|
| `partie-complete-0` | 1102 (1059 -> 2161) | 0 | Fete | 20 |
| `nuit-b0` | 817 (841 -> 1658) | 1 | Histoire | 15 |

Les traces `modesel_loop` relevees dans les deux journaux donnent `cursor=0`
quatre fois pour le premier, et `0, 0, 1` pour le second. La valeur de
`max_turn` confirme le mode par un second chemin : 20 vient de
`mentDll/main.c:840`, 15 de `BoardStoryConfigSet` (`src/game/board/main.c:361`).

### Pourquoi c'est un defaut et pas un reglage

Six runs de nuit avaient ete prepares pour couvrir six plateaux differents, avec
un nombre croissant de crans place entre les frames 2500 et 5200. Cette fenetre
tombe dans `mentDll`, mais le curseur du **mode** avait deja derive avant, dans
`modeseldll`. Les six runs jouaient donc le mode Histoire sur le meme plateau :
dix heures de machine pour une seule ligne de couverture, et une couverture
annoncee qui n'aurait pas correspondu a ce qui tournait.

### Le correctif

`--netplay-walk-plan=<crans>` fait reagir le marcheur au **contexte** au lieu du
numero de frame :

- dans `modeseldll` : aucun horizontal, A seulement. Le curseur ne peut plus
  deriver.
- dans `mentDll` : exactement N crans a droite espaces de 60 frames, depuis le
  siege 0 seulement, puis A. Le verrou de repetition de `PadADConv` dure 20
  frames, donc une impulsion donne un cran.

Le contexte est le seul repere stable : il fait partie de l'etat canonique et le
protocole verifie a chaque tick que les deux pairs sont dans le meme
(`context_skew=0`). Le numero de frame ne l'est pas, puisqu'un ecran charge de
1000 a 1567 frames avant de lire la manette et que cette duree bouge de
plusieurs centaines de frames selon ce qui precede.

### La verification

Deux runs de sept minutes, binaire `build/hashv3`, machine partagee avec deux
autres campagnes :

| run | drapeau | plateau atteint | attendu | `max_turn` | verdict |
|---|---|---|---|---|---|
| `verif+1` | `--netplay-walk-plan=0:1` | **`w04Dll`** a la frame 5895 | `w04Dll` | 20 | PASS |
| `verif-1` | `--netplay-walk-plan=0:-1` | **`w03Dll`** a la frame 5895 | `w03Dll` | 20 | PASS |

Les deux plateaux sont ceux que la lecture de `sp8[6] = {1, 2, 0, 3, 4, 5}` avec
un curseur partant de l'index 2 avait predits, et ce sont deux plateaux
differents l'un de l'autre et de `w01`. `max_turn = 20` confirme le mode Fete
dans les deux cas, et `extra_arguments` est desormais inscrit dans `run.json`.

**C'est la premiere fois que ce projet choisit un plateau.** Les vingt-cinq
campagnes precedentes jouaient toutes `w01`, non par choix mais parce que c'est
la position de repos du curseur.

### Ce qui reste a prouver

1. Que les crans -2 et +2 donnent bien `w02` et `w05`. La lecture le dit, la
   mesure ne l'a pas encore dit.
2. La table crans -> plateau, **relevee** sur les runs et non deduite.
3. Qu'un run planifie soit reconnaissable **dans son propre enregistrement**.
   C'est fait, par deux chemins independants. La ligne
   `[NET TEST] walk_plan armed notches=0` est bien conservee : la campagne ecrit
   la sortie standard de chaque pair dans `peer-N-stdout.log` a la fin du run, et
   elle y figure pour `carte-0`. Et `run.json` porte **`board_max_turn`** — 20
   vient du menu Fete (`mentDll/main.c:840`), 15 du mode Histoire
   (`BoardStoryConfigSet`) : `carte-0` affiche 20 la ou les runs de la nuit
   affichaient 15. `extra_arguments` a ete ajoute au dossier de run pour qu'un
   resultat dise aussi avec quels drapeaux il a tourne.

---

## D18 — `HU3D_ATTR_DISPOFF` diverge sur treize modeles dans GURUGURU BOX

**Classification : divergent model visibility between two peers, detected by the
`ANIMATION` subsystem of the canonical hash.**

**Statut : non corrige, champ nomme, cause inconnue.**

**Niveau de preuve : SCRIPTED.** Trouve le 2026-09-12 a 20h14 par `nuit-b0`, en
boucle locale, mode Histoire.

### Ce qui a ete mesure

Divergence a la frame 38665, **overlay 39, `m431Dll` GURUGURU BOX**, categorie
`ANIMATION`. **13 champs different sur 5030**, et ce sont tous le meme champ sur
des modeles differents :

```
FIELD 1488  ANIMATION  model->attr  00100000  contre  00100001
FIELD 1644  ANIMATION  model->attr  00100000  contre  00100001
...
FIELD 2463  ANIMATION  model->attr  00000000  contre  00000001
```

Le bit qui differe est le bit 0, `HU3D_ATTR_DISPOFF` (`include/game/hu3d.h:52`).
**Treize modeles sont caches sur un pair et visibles sur l'autre.**

Rien d'autre ne differe : `mismatch=0`, `rng_sync=1`, `context_skew=0`, les
entrees et le RNG sont identiques des deux cotes, et les quinze autres
sous-systemes sont d'accord.

### Pourquoi ce defaut compte plus que sa categorie ne le suggere

`HU3D_ATTR_DISPOFF` n'est pas un detail de presentation, et c'est pourquoi il
est hache : la visibilite d'un modele est decidee par la logique du jeu. Treize
modeles qui apparaissent chez un joueur et pas chez l'autre, dans un mini-jeu ou
il faut suivre des boites, c'est une partie que les deux joueurs ne jouent pas.

Treize d'un coup, et un quatorzieme (`FIELD 2463`) dont l'attribut complet vaut
0 contre 1, suggerent une operation de groupe — un « cacher tout » ou un
« montrer tout » execute une fois de plus d'un cote — plutot que treize decisions
independantes.

### Une deuxieme occurrence, dans un autre mini-jeu, trois fois plus large

`partie-complete-3`, 2026-09-12 a 21h14, **au tour 6 sur 20 apres 53 minutes** —
le run le plus profond du projet. Divergence a la frame 146086, **overlay 48,
`m440Dll` NEO KOOPA BAKUDAN**, categorie `ANIMATION`. **35 champs sur 3478**, et
c'est toujours le meme champ sur des modeles differents :

```
FIELD 1886  ANIMATION  model->attr  00000011  contre  00000010
FIELD 1925  ANIMATION  model->attr  00000011  contre  00000010
...  (35 fois, a 39 champs d'intervalle, donc 35 modeles consecutifs)
```

**Le bit qui differe est le meme, `HU3D_ATTR_DISPOFF`.** Et il y a une chose de
plus que dans la premiere occurrence : les 35 modeles portent tous
`0x10 = HU3D_ATTR_HOOKFUNC`. Ce ne sont pas des modeles quelconques, ce sont des
modeles dessines par une fonction de hook.

Deux mini-jeux sans rapport, le meme bit, sur un groupe entier de modeles a
chaque fois — 13 d'un cote, 35 de l'autre. Ce n'est plus une curiosite d'un
module : c'est une forme.

### Ce qu'il faut faire

1. Enumerer les appels a `Hu3DModelAttrSet`/`Hu3DModelAttrReset` avec
   `HU3D_ATTR_DISPOFF` dans `src/REL/m431Dll/` **et `src/REL/m440Dll/`**, et
   chercher celui qui s'applique a un groupe. Dans `m440Dll` les boucles
   evidentes (`main.c:490`, `main.c:501`) ne portent que sur trois modeles : le
   groupe de 35 est ailleurs, probablement la boucle de `main.c:661` sur
   `lbl_1_bss_64[]`.
2. Instrumenter cet appel sur les deux pairs et comparer la frame a laquelle il
   se produit.
3. Rejouer GURUGURU BOX plusieurs fois : on ne sait pas encore si la divergence
   est deterministe ou intermittente.

---

## D19 — les quatre identifiants de motion ne sont pas haches, et l'un d'eux decide une allocation

**Classification : control-flow field excluded from the canonical hash.**

**Statut : trou etabli par lecture. Chaine causale complete sauf un maillon, qui
demande une mesure.**

**Niveau de preuve : SCRIPTED**, trois occurrences le 2026-09-12.

### Le trou

`PartyBoard_NetplayAnimationState` (`src/game/hsfman.c:2414`) exporte, pour
chaque modele : `attr`, `motAttr`, les quatre structures de travail
`motWork`, `motOvlWork`, `motShiftWork`, `motShapeWork`, les tableaux de
cluster, puis `motIdSrc`, `linkMdlId` et `cameraBit`.

Il n'exporte **aucun** des quatre identifiants qui selectionnent ces structures
de travail (`include/game/hu3d.h:275-278`) :

```c
HU3DMOTID motId;
HU3DMOTID motIdOvl;
HU3DMOTID motIdShift;
HU3DMOTID motIdShape;
```

Les horloges sont hachees, les identifiants qui disent **quelle** animation ces
horloges font avancer ne le sont pas. Deux pairs peuvent donc jouer deux
animations differentes avec des temps identiques, et le hachage canonique
declarera qu'ils sont d'accord.

### Pourquoi ce n'est pas une remarque theorique

`src/REL/m415Dll/main.c:1014` (PYONPYON STAMP) :

```c
case 6:
    if ((Hu3DMotionEndCheck(object->model[2]) == 0)
        && (Hu3DMotionShiftIDGet(object->model[2]) >= 0)) {
        temp_r31->unkC = 1;
    }
    else {
        ...
        if (temp_r31->unkC != 0) {
            ...
            var_r22 = fn_1_A2D0(0x28, 1);
```

`Hu3DMotionShiftIDGet` retourne exactement `model->motIdShift`
(`src/game/hsfmotion.c:321`) — le champ non hache. Et `fn_1_A2D0`
(`src/REL/m415Dll/map.c:562`) fait **sept** `HuMemDirectMallocNum(HEAP_SYSTEM, ...)`
consecutifs, lignes 585 a 593.

### Ce que les trois occurrences mesurent

| run | frame | blocs | octets |
|---|---|---|---|
| balayage du matin | ~20000 | 28 d'ecart | 61 440 |
| `partie-complete-0`, 20h10 | 32170 | 67 contre 74, **7** | 596 448 contre 611 808, **15 360** |
| `partie-complete-1`, 20h21 | 33545 | 74 contre 81, **7** | 611 808 contre 626 656, **15 360** |

**Le quantum est identique a l'octet pres, trois fois**, et 28 = 4 x 7,
61 440 = 4 x 15 360. La valeur locale du second run est exactement la valeur
distante du premier. Sept blocs et 15 360 octets, c'est un appel de `fn_1_A2D0`
et rien d'autre.

Les deux runs ont par ailleurs **le meme chemin d'overlays a la frame pres**
(`1@2 ... 23@30890`) : la route est deterministe, le mini-jeu est deterministe,
la categorie est deterministe, **seule la frame de la divergence ne l'est pas**.
C'est la signature d'une bascule de branche pres d'une limite, pas d'un chemin
different.

### Une quatrieme occurrence, dans un autre mini-jeu, avec le meme compte

`nuit-b1`, 2026-09-12 a 20h38, apres **24 minutes et cinq tours** — le run le
plus profond de la journee. Divergence `HEAPS` a la frame 65398, **overlay 46,
`m438Dll` SYAKUNETSU WANWAN ATTACK**, sur le **tas 0, `HEAP_SYSTEM`** :

| | valeur |
|---|---|
| blocs | 56 contre 63, ecart **7** |
| octets | 153 632 contre 175 968, ecart **22 336** |
| champs differents | **2 sur 4388** |

Sept blocs a nouveau, mais **22 336 octets et non 15 360**. Ce n'est donc pas le
meme groupe d'allocations — c'est le meme *genre* de groupe.

`src/REL/m438Dll/fire.c:603-611` fait sept `HuMemDirectMallocNum(HEAP_SYSTEM, ...)`
consecutifs, et la suite des champs est celle de `m415Dll/map.c:585-593` a
l'identique :

```
unk_34  arg1 * sizeof(s16)             unk34  arg1 * 2
unk_4C  arg0 << 8                      unk4C  arg0 * sizeof(unkType)
unk_50  arg0 * (4 * sizeof(Vec))       unk50  arg0 * sizeof(Vec[4])
unk_54  arg0 * (4 * sizeof(GXColor))   unk54  arg0 * sizeof(unkType2)
unk_58  arg0 * sizeof(Vec)             unk58  arg0 * sizeof(Vec)
unk_5C  arg1 * (arg0 * sizeof(...))    unk5C  arg1 * (arg0 * sizeof(...))
```

**C'est le meme utilitaire, recopie dans chaque DLL de mini-jeu.** Quinze
modules du depot ont un `map.c` de cette famille, et leurs contenus different
— ce sont des variantes, pas un fichier partage. Le defaut n'est donc pas a un
endroit : il a autant de copies que de mini-jeux qui l'utilisent.

**Ce que cela etablit** : quatre divergences `HEAPS` de la journee, dans trois
mini-jeux differents, valent toutes un nombre entier d'appels a cet utilitaire.
Un pair l'appelle une fois de plus que l'autre.

**Ce que cela n'etablit pas** : pourquoi. Dans PYONPYON STAMP la branche est
gardee par `Hu3DMotionShiftIDGet`, champ non hache — d'ou ce defaut. Le garde de
la branche correspondante dans `m438Dll` n'a pas encore ete lu, et rien ne dit
qu'il s'agit du meme.

### La chaine, fermee par lecture

Le 2026-09-12 au soir, le dernier maillon lisible a ete lu. `Hu3DMotionEndCheck`
ne compare pas deux valeurs hachees :

```c
s32 Hu3DMotionEndCheck(s16 arg0)
{
    if (!(Hu3DData[arg0].motAttr & HU3D_MOTATTR_REV)) {
        return (Hu3DMotionMaxTimeGet(arg0) <= Hu3DMotionTimeGet(arg0));
    }
    ...
}

float Hu3DMotionMaxTimeGet(s16 arg0)
{
    HU3DMODEL *temp_r31 = &Hu3DData[arg0];
    if (temp_r31->motId == -1) {
        return 0.0f;
    }
    temp_r30 = &Hu3DMotion[temp_r31->motId];
    ...
}
```

Le temps est hache. **La duree maximale contre laquelle il est compare est lue
dans `Hu3DMotion[motId]`, par un index qui ne l'est pas.** Deux pairs dont le
`motId` differe comparent donc le meme temps a deux durees differentes, et
repondent differemment a « cette animation est-elle finie ».

Le cas `motId == -1` est le plus brutal : il rend `0.0f`, donc
`0.0f <= temps` est vrai des la premiere frame, donc l'animation est declaree
**finie immediatement**. Un pair dont le `motId` est encore -1 pendant que
l'autre a un identifiant valide prend l'autre branche tout de suite — et c'est
cette branche qui alloue les sept blocs.

Chaine complete : `motId` non hache -> `Hu3DMotionMaxTimeGet` -> `Hu3DMotionEndCheck`
-> la branche de `m415Dll/main.c:1014` -> `fn_1_A2D0` -> sept blocs de
`HEAP_SYSTEM` -> divergence `HEAPS`.

### Le maillon qui manque encore

Il n'est **pas** demontre que les deux pairs avaient reellement des `motId`
differents a cet instant : la lecture etablit que ce serait suffisant, pas que
c'est arrive. C'est precisement ce que le binaire `build/hashv3` va dire, et la
reponse n'a que deux formes possibles — soit une divergence `ANIMATION` sur un
identifiant de motion apparait avant la divergence `HEAPS`, soit elle
n'apparait pas.

### Le correctif, qui est aussi l'experience

Ajouter les quatre identifiants a `PartyBoard_NetplayAnimationState` et passer
`kStateHashVersion` de 2 a 3.

- S'ils sont la cause, le prochain run divergera en `ANIMATION` sur
  `model->motIdShift`, **a une frame anterieure**, et nommera le modele.
- S'ils ne le sont pas, la divergence `HEAPS` restera identique, et un trou reel
  du hachage aura quand meme ete ferme.

Dans les deux cas la mesure tranche. C'est pour cela que le changement se fait
avant d'avoir la reponse, et non apres.

---

## D20 — divergence `SCENE` dans le menu des modes, sous charge

**Classification : divergent wipe (fade) state between two peers in a menu.**

**Statut : mesure faite, champs nommes, cause NON etablie. A ne pas confondre
avec une cause comprise.**

**Niveau de preuve : SCRIPTED**, une occurrence, le 2026-09-12 a 22h00.

### Ce qui a ete mesure

`partie-complete-2`, meme binaire et meme sonde que les runs 0 et 1 qui venaient
de traverser ce meme ecran sans rien signaler. Divergence a la **frame 2146**,
**overlay 74** (`modeseldll`), apres 41 secondes. **6 champs sur 2211**, tous
dans `wipeData` :

| champ | pair 0 | pair 1 |
|---|---|---|
| `wipeData.mode` | 2 (`WIPE_MODE_OUT`) | 3 (`WIPE_MODE_BLANK`) |
| `wipeData.stat` | 1 (en cours) | 0 (termine) |
| `wipeData.duration` | 10,0 | 20,0 |
| `wipeData.color` | 255,255,255 | 0,0,0 |

Ni le RNG (`frand_calls=0` des deux cotes), ni les entrees, ni les quinze autres
sous-systemes ne different.

### Ce que cela dit, et ce que cela ne dit pas

Les deux durees ne sont pas deux etats d'un meme fondu : une duree de 10 et une
duree de 20 viennent de **deux appels differents** a `WipeCreate`, et les
couleurs le confirment. Les deux pairs n'ont donc pas joue la meme suite
d'appels dans ce menu.

Et `WipeCreate` (`src/game/wipe.c:161`) **abandonne en silence** si un fondu est
deja en cours :

```c
if(wipe->stat) {
    return;
}
```

Une seule frame d'ecart sur l'horloge du fondu suffit donc a ce qu'un pair
accepte une demande que l'autre jette. L'ecart ne se rattrape pas : il se fige.

**Ce qui n'est pas etabli** : pourquoi les deux pairs ont diverge en amont. La
mecanique de D6 est ecartee par la mesure — `d6_batched_frames = 0` sur les
quatre runs de la soiree, donc le pacer n'a jamais produit deux ticks dans une
image. `WipeExecAlways` est par ailleurs deja bride par `PartyBoard_IsSimulationTick`
(`src/game/wipe.c:12`), et n'avance donc pas sur une image rendue sans tick.

### La correlation avec la charge, a prendre pour ce qu'elle est

Cette occurrence est la seule des quatre a s'etre produite avec **trois campagnes
et une compilation** sur la machine, contre deux campagnes pour les precedentes.
Les compteurs de blocage sont tres asymetriques : `stalled=50 longest_stall=7`
d'un cote, `stalled=3 longest_stall=0` de l'autre.

C'est une correlation sur une seule occurrence. Elle ne prouve rien, et elle ne
disqualifie pas le defaut non plus — **au contraire** : sur deux vraies machines
les blocages de lockstep sont la regle, pas l'exception, et c'est precisement ce
que le verdict `PLAYABILITY` a ete cree pour mesurer. Un defaut qui n'apparait
que sous blocage est un defaut qui apparaitra en ligne.

### Ce qu'il faut faire

1. Rejouer le meme scenario **machine au repos**, puis **machine chargee**, et
   compter. Une occurrence ne distingue pas encore les deux.
2. Le binaire de D19 (qui hache `motId`, `motIdOvl`, `motIdShift`, `motIdShape`)
   est aussi la sonde de ce defaut-ci : si la divergence amont est un identifiant
   d'animation, elle sera desormais signalee **avant** le fondu, et nommee.
3. Ne pas corriger `WipeCreate` pour qu'il accepte une demande pendant un fondu.
   L'abandon silencieux amplifie l'ecart, il ne le cree pas, et le masquer
   detruirait le signal.

---

## D21 — quinze tirages aleatoires de plus sur un pair, dans SYAKUNETSU WANWAN ATTACK

**Classification : divergent control flow, detected by the `RNG` subsystem.**

**Statut : mesure faite, champ nomme, cause inconnue.**

**Niveau de preuve : SCRIPTED**, une occurrence, `nuit-b2`, 2026-09-12 a 21h01,
apres 22 minutes et cinq tours.

### Ce qui a ete mesure

Divergence a la frame 68552, **overlay 46, `m438Dll`**, categorie `RNG`.
**2 champs sur 4415** :

| champ | pair 0 | pair 1 | ecart |
|---|---|---|---|
| `PartyBoard_NetplayFrandCalls()` | 441 453 | 441 468 | **15 appels** |
| `stamp.frand` | `fc1051eb` | `a045c4db` | l'etat qui en resulte |

La frame precedente etait entierement d'accord. **Un pair a donc fait quinze
tirages de plus que l'autre en une seule frame**, ce qui n'est pas un ecart de
valeur mais un ecart de chemin : une boucle a tourne un nombre de fois different,
ou une branche a ete prise d'un cote seulement.

### Pourquoi elle compte autant que les divergences de tas

C'est le **meme mini-jeu** que l'occurrence 4 de D19, qui y avait montre sept
blocs de `HEAP_SYSTEM` d'ecart. Deux categories differentes, un seul module. Cela
oriente vers une divergence de branche dans `m438Dll` dont les consequences se
voient tantot dans le tas, tantot dans le compteur de tirages, selon ce que la
branche fait en premier.

`REFRESH_RATE` a ete verifie et ecarte : c'est une constante de compilation
(`include/version.h:24`), identique sur les deux pairs puisqu'ils partagent le
binaire. Les appels `frandmod(REFRESH_RATE * 3)` de `map.c` ne sont donc pas la
source.

### Ce qu'il faut faire

1. Attendre ce que dit le binaire `build/hashv3`, qui hache desormais les quatre
   identifiants de motion. Si la branche de `m438Dll` depend d'un de ces
   identifiants comme celle de `m415Dll`, la divergence sera signalee **avant**
   ce compteur, et nommee.
2. Sinon, instrumenter les fonctions a forte densite de tirages de
   `m438Dll/map.c` — `fn_1_CAB0` en fait 18, `fn_1_D57C` 10, `fn_1_DA64` 9 — et
   comparer le nombre d'entrees sur les deux pairs.

---

## D22 — une position d'objet qui diverge de deux millièmes, sur le plateau

**Classification : divergent object translation, detected by the `OBJECTS`
subsystem.**

**Statut : mesure faite, champs nommes, cause inconnue.**

**Niveau de preuve : SCRIPTED**, une occurrence, `nuit-b3`, 2026-09-12 a 21h07.

### Ce qui a ete mesure

Divergence a la frame 19125, **overlay 89, `w01Dll` — le plateau lui-meme, pas
un mini-jeu**. **2 champs sur 3712** :

| champ | pair 0 | pair 1 | ecart |
|---|---|---|---|
| `object->trans.x` | -10,8421383 | -10,8413868 | 0,0007515 |
| `object->trans.z` | 3,3762352 | 3,3782520 | 0,0020168 |

Deplacement total 0,00215 unite. L'objet n'a **ni modele ni motion**
(`mdlcnt = 0`, `mtncnt = 0`), il n'est donc pas anime : sa position est calculee
par sa propre fonction. Son echelle est 751 x 1 x 1351 et son `trans.y` est nul.

### Pourquoi elle ne ressemble a aucune des autres

Les six autres divergences de la journee sont des ecarts de **chemin** : un bloc
alloue ou non, un tirage fait ou non, un modele cache ou non. Celle-ci est un
ecart de **valeur**, et minuscule. Sur deux processus issus du meme binaire, sur
la meme machine, une difference de virgule flottante n'a pas d'explication
ordinaire.

Et surtout : **elle se produit sur le plateau.** La classe de defaut n'est donc
pas confinee aux mini-jeux, ce que les six occurrences precedentes laissaient
croire.

### L'hypothese de la charge, mesuree puis ecartee

Toutes les campagnes de la soiree ont tourne a deux ou trois en parallele, avec
une compilation par-dessus. Il fallait donc verifier que les blocages de lockstep
ne fabriquaient pas ces divergences. Les compteurs de blocage de chaque run, en
face de la frame ou il est mort :

| run | blocages | plus long | frame de divergence |
|---|---|---|---|
| `partie-complete-0` | 38 | 2 | 32 170 |
| `partie-complete-1` | 82 | 1 | 33 545 |
| `partie-complete-2` | 50 | 7 | **2 146** |
| `nuit-b0` | 354 | 4 | 38 665 |
| `nuit-b1` | **565** | **37** | **65 398** |
| `nuit-b2` | 171 | 3 | 68 552 |
| `nuit-b3` | 111 | 5 | 19 125 |

Le run le plus bloque est celui qui est alle **le plus loin**, et le moins bloque
est mort le plus tot. **Il n'y a pas de correlation.** Les divergences ne sont
donc pas un artefact de ma propre parallelisation, et les sept comptent.

---

## Experience du remplissage memoire — protocole ecrit AVANT le resultat

Ce protocole est inscrit ici avant que la mesure existe, pour qu'aucune lecture
apres coup ne puisse l'ajuster a ce qui sera sorti.

### La question

Sept divergences ont ete mesurees le 2026-09-12, de **cinq formes** :

| forme | ou | defaut |
|---|---|---|
| compte de blocs de tas | `m415Dll` x2, `m438Dll` | D19 |
| visibilite de modeles | `m431Dll` | D18 |
| phase de fondu | `modeseldll` | D20 |
| compte de tirages aleatoires | `m438Dll` | D21 |
| position d'objet, 0,002 unite | `w01Dll` | D22 |

Une seule cause pourrait produire les cinq : **la lecture d'une memoire jamais
ecrite.** `HuMemMemoryAlloc2` (`src/game/memory.c`) rend son bloc exactement tel
qu'il l'a trouve. D15, corrige le matin meme, etait precisement de cette classe.

### Le dispositif

`PARTYBOARD_MEM_FILL` fait ecrire a l'allocateur un octet fixe sur chaque bloc
distribue. Une lecture de memoire non initialisee devient alors **identique sur
les deux pairs**, donc inoffensive pour le determinisme — sans cesser d'etre un
bug.

Une seule variable change : le meme binaire, `build/walkplan`, le meme scenario
(`--netplay-walk-plan=0:0`, mode Fete, `w01`), le meme budget. Seule la variable
d'environnement differe, et elle est inscrite dans chaque `run.json` sous
`mem_fill`.

### Les deux issues, et ce que chacune autorise a conclure

- **Le taux de divergence s'effondre** : la classe est nommee. Cela n'identifie
  aucun champ — il faudra ensuite les chercher un a un, comme `object->group`
  l'a ete. Et cela ne corrige rien : le remplissage devra etre retire.
- **Le taux ne bouge pas** : la classe est ecartee, et les sept defauts restent
  sept chemins de code a instruire separement. L'hypothese de l'identifiant de
  motion (D19) redevient la piste principale.

### Ce que le remplissage ne sera jamais

Un correctif. Il **masque** exactement le defaut qu'il sert a detecter, et ce
depot ne masque pas les echecs. Il reste eteint par defaut, il est refuse comme
reglage de campagne ordinaire, et tout resultat obtenu avec lui porte
`mem_fill` dans son enregistrement pour qu'il ne puisse jamais etre relu comme un
run normal.

### Le temoin

Le bras « sans remplissage » n'est pas a refaire : c'est la soiree entiere.
**Huit runs termines qui jouent des mini-jeux, huit divergences.** Un bras de six
runs remplis qui n'en produirait aucune serait deja un ecart que le hasard
n'explique pas.

### Resultats, au fur et a mesure

| run | `mem_fill` | resultat | tour | ou | champs |
|---|---|---|---|---|---|
| `remplissage-0` | **165** | **DESYNC** `OBJECTS` | 2 | overlay 40, `m432Dll` PAIR DE RACE | `trans.x/y/z` d'un objet |
| `remplissage-1` | **165** | **DESYNC** `OBJECTS` | 3 | overlay 35, `m427Dll` BOAT RACE | `trans.y` de **six** objets |

| `remplissage-2` | **165** | **DESYNC** `SCENE` | 0 | overlay 74, `modeseldll`, frame 1910 |  |

### Verdict : l'hypothese est ecartee

**Trois runs remplis, trois divergences.** Le temoin est de dix runs non
remplis pour dix divergences. Remplir chaque bloc distribue d'un octet fixe ne
change ni le fait qu'une divergence arrive, ni sa rapidite.

**La lecture d'une memoire jamais ecrite n'est donc pas la cause commune de la
soiree du 2026-09-12.** Le remplissage reste dans le depot, eteint, comme
instrument : il a repondu a sa question et il repondra a la suivante du meme
genre. Il ne doit jamais etre arme dans une campagne ordinaire.

Ce que l'experience a donne en plus de son verdict : `remplissage-2` a reproduit
**D20** — la meme divergence de fondu dans le menu des modes, a la frame 1910
contre 2146 la premiere fois. Ce defaut-la est donc reproductible, ce qu'une
seule occurrence ne permettait pas de dire.

**Deux sur trois, et deux fois la meme categorie.** Sans remplissage, les dix
divergences de la soiree se repartissaient sur cinq categories ; avec, les deux
sont `OBJECTS`. L'echantillon est trop petit pour en tirer autre chose qu'une
question, mais la question est nette.

Le second est instructif au-dela du verdict. Six objets, **et seulement leur `y`**,
en deux groupes de valeurs identiques :

```
FIELD 818, 840   trans.y  16,0355  contre  16,1387
FIELD 862 a 928  trans.y  -8,9645  contre  -8,8604
```

Des objets qui partagent exactement la meme hauteur et qui derivent ensemble, ce
n'est pas du bruit : c'est **une surface**. BOAT RACE est un mini-jeu d'eau, et
son `map.c` cree **dix hooks de dessin** — le plus grand nombre releve dans un
seul module. Une houle dont la phase avance depuis le chemin de dessin donnerait
exactement cette signature, et c'est D23.

**Premier point contre l'hypothese.** Le remplissage etait bien arme — le
`run.json` porte `mem_fill: 165` — et la divergence s'est quand meme produite, de
la meme classe que D22 : trois flottants de `object->trans`, avec un ecart de
**0,18 unite sur 834,9** en y, soit 2 x 10^-4 en relatif. Ce n'est pas un arrondi.

Un run ne tranche rien : 1 divergence sur 1 run rempli contre 8 sur 8 non
remplis n'est pas encore une difference. Mais si les cinq suivants divergent
aussi, la classe « lecture de memoire jamais ecrite » sera ecartee, et il faudra
prendre les sept defauts un par un.

---

## D23 — les hooks de dessin avancent l'etat du jeu une fois par IMAGE, pas par tick

**Classification : game state advanced from the render path, which is not
lockstepped.**

**Statut : chaine causale complete, lue de bout en bout. Mesure en attente.**

**C'est le defaut central de la soiree du 2026-09-12** : il explique a lui seul
la forme de la plupart des huit divergences mesurees ce jour-la.

### La chaine, lue ligne a ligne

**1. Un pair qui attend son voisin rend quand meme une image.**
`src/game/main.c:270` :

```c
simulationAllowed = HuPadPollSimulationTick();
if (simulationAllowed) {
    HuPadRead();
    PartyBoard_RunGameLogicTick();
    PartyBoard_NetplayCommitTick();
    simulatedTicks++;
}
```

Quand le lockstep attend, `simulationAllowed` est faux, `simulatedTicks` reste a
zero — **et la boucle continue jusqu'au dessin.**

**2. Le dessin appelle les hooks, sans garde.** `src/game/hsfdraw.c:2308`, dans
`Hu3DDrawPost` :

```c
if (drawObj->model->attr & HU3D_ATTR_HOOKFUNC) {
    hookFunc = (void *)drawObj->model->hsf;
    hookFunc(drawObj->model, drawObj->matrix);
```

`Hu3DExec` est appele a chaque tour de boucle, sans condition. Juste a cote,
`data->tick++` **est** garde par `PARTYBOARD_ADVANCE_FRAME`. L'appel du hook ne
l'est pas.

**3. Les hooks ne dessinent pas : ils font avancer l'etat.**
`src/REL/m415Dll/map.c`, le hook de PYONPYON STAMP :

```c
for (var_r30 = 0; var_r30 < lbl_1_bss_36C.unk30; var_r30++, var_r31++) {
    if ((var_r31->unk8 != 0) && ...) {
        if ((u8)omPauseChk() == 0) {
            if (var_r31->unk30) {
                var_r31->unk30(var_r31);   /* rappel de mise a jour */
            }
            if (var_r31->unk8 == 0) continue;
            fn_1_9DC8(var_r31);            /* fait vieillir le ruban */
            if (var_r31->unk8 == 0) continue;
        }
        fn_1_88B8(var_r31);                /* et seulement la, dessine */
    }
}
```

`unk8` est le **drapeau d'occupation de l'emplacement**, et il peut tomber a zero
a l'interieur du hook. Sa seule garde est `omPauseChk()`, qui parle du menu
pause, pas du tick de simulation.

**4. Et c'est ce drapeau que l'allocateur des sept blocs consulte.**
`fn_1_A2D0` (`map.c:562`) cherche le premier emplacement libre en testant
`unk8 == 0`, puis fait ses sept `HuMemDirectMallocNum(HEAP_SYSTEM, ...)`.

### Ce que cela explique

| divergence | explication |
|---|---|
| **sept blocs de tas**, 4 fois (D19) | un ruban de plus ou de moins vit chez un pair : un appel de `fn_1_A2D0` d'ecart |
| **`HU3D_ATTR_DISPOFF`** sur 13 puis 35 modeles (D18) | les 35 portaient tous `HU3D_ATTR_HOOKFUNC`. Ce sont des modeles de hook, montres ou caches depuis le chemin de dessin |
| **positions d'objets** a 0,002 et 0,18 pres (D22) | un objet anime par un hook a recu un pas de plus |
| **quinze tirages aleatoires** (D21) | une boucle de hook a tourne une fois de plus |

Quatre formes, une cause. Ce n'est pas une preuve que les quatre en viennent —
c'est un mecanisme qui les produit toutes et qui est etabli dans le code.

### Ce que le depot savait deja

La porte de sûrete du rollback **refuse** de rejouer un tick sans dessiner des
qu'un modele porte `HU3D_ATTR_HOOKFUNC`, et nomme son refus `"model-draw-hook"`
(`src/game/hsfman.c:2354`). Le projet sait donc depuis longtemps que ces hooks
portent de l'etat de jeu. Personne n'avait tire la conclusion symetrique : **si
un tick ne peut pas etre rejoue sans dessiner, alors une image ne peut pas etre
dessinee sans tick.**

`d6_batched_frames` ne pouvait pas le voir : il compte les images portant **plus
d'un** tick, et le cas qui se produit en ligne est une image n'en portant
**aucun**.

### La premiere mesure, et ce qu'elle refuse de confirmer

`rendered_frames` est desormais rapporte a cote de `simulation_frame`. Premier
run, `fin-w01`, 2026-09-12 a 21h56, trois releves :

| frame de simulation | images rendues, pair 0 | images rendues, pair 1 | ecart |
|---|---|---|---|
| 1086 | 1086 | 1088 | **2** |
| 2915 | 2915 | 2917 | **2** |
| 4434 | 4434 | 4436 | **2** |

**Les deux pairs ne rendent pas le meme nombre d'images** : la condition
prealable existe. Mais l'ecart est **constant**, pas croissant. Le pair qui
rejoint a rendu deux images pendant l'etablissement de la connexion, avant que
la simulation ne commence, et depuis **chaque image porte exactement un tick des
deux cotes**.

C'est un resultat qui va contre la forme forte de l'hypothese. Deux images
supplementaires au demarrage, avant qu'aucun modele n'existe, n'appellent aucun
hook utile. Pour que D23 explique les divergences mesurees, il faut que l'ecart
**croisse** pendant la partie — c'est-a-dire qu'un pair en attente rende des
images sans tick. Sur 4434 frames, il n'a pas bouge.

Deux lectures restaient ouvertes. **La mesure a tranche une demi-heure plus
tard, sur le meme run.**

| frame de simulation | images rendues, pair 0 | images rendues, pair 1 |
|---|---|---|
| 4 434 | 4 434 (+0) | 4 436 (**+2**) |
| 90 893 | 90 922 (+29) | 92 345 (**+1 452**) |
| 92 722 | 92 720 (+29) | 94 174 (**+1 452**) |
| 94 520 | 94 549 (+29) | 95 974 (**+1 454**) |

**L'ecart croit, et il croit d'un seul cote.** Le pair 1 a rendu **1 452 images
de plus qu'il n'a simule de ticks** la ou le pair 0 n'en a rendu que 29 de plus.
Entre les deux pairs, a la meme frame de simulation, **1 423 images d'ecart**,
contre 2 au debut du run.

Ce n'est donc pas un decalage fixe de demarrage : c'est bien un pair qui tourne
en rendant pendant qu'il attend l'entree de son voisin. La condition prealable de
D23 est **prouvee par la mesure** : sans garde, ces 1 423 images auraient appele
chaque hook de dessin 1 423 fois de plus sur un pair que sur l'autre.

### Et la correspondance exacte qui nomme le mecanisme

Au meme instant, les compteurs de blocage des deux pairs :

| pair | `stalled_ticks` | images rendues en trop |
|---|---|---|
| 0 | 30 | **29** |
| 1 | 1 496 | **1 452** |

**Un tick bloque = une image rendue sans tick.** Les deux compteurs se suivent a
quelques unites pres, et ces quelques unites sont le retard entre deux ecritures
du fichier d'etat. Il n'y a plus rien a supposer sur le mecanisme : la boucle
principale rend une image a chaque tour, le lockstep lui refuse le tick, et
`Hu3DDrawPost` appelle quand meme tous les hooks.

`stalledTicks` etait deja compte et deja rapporte. Ce qui manquait n'etait pas la
mesure, c'etait de voir que ce compteur mesurait aussi **le nombre de fois ou le
jeu a avance son etat sans y avoir droit**.

Ce qui reste a etablir est le lien, pas la condition : que ces appels
supplementaires soient bien ce qui a produit les divergences mesurees. Seule la
comparaison rouge/vert de la garde peut le dire.

### Ce qui reste a mesurer, et le correctif propose

1. L'ecart `rendered_frames` sur un run charge, ou `stalled` se compte par
   centaines. C'est la mesure qui tranche.
2. Correctif candidat : **sous netplay, ne pas presenter d'image pour une frame
   qui ne porte aucun tick.** Une image par tick, des deux cotes, et tous les
   hooks redeviennent deterministes d'un coup. Le prix est que la fenetre se fige
   pendant une attente — ce qui est exactement ce qu'un jeu en lockstep doit
   faire.
3. Il doit etre **vu rouge puis vert** : le meme scenario, meme binaire, avec et
   sans le correctif, et la difference des compteurs `rendered_frames` comme
   temoin.

---

## D24 — un hook de dessin detruit continue de s'annoncer, et le jeu saute a l'adresse 0

**Classification : call through a null function pointer left advertised by a
stale attribute bit.**

**Statut : CAUSE ETABLIE PAR LECTURE, CORRECTIF ECRIT. Pas encore vu rouge puis
vert — les deux arbres de construction sont occupes par la comparaison de D23.**

**Niveau de preuve : SCRIPTED**, trois occurrences sur trois graines le
2026-09-12, dans `m445Dll` KINOPIO HAMMER, overlay 53.

### Ce que disait le rapport, et ce qu'il ne disait pas

```
exception_name=EXCEPTION_ACCESS_VIOLATION
exception_address=0x0
faulting_module=<none: address is not inside a loaded module>
game_context=53 overlay=53 minigame=44
[STACK TRACE]
```

Adresse zero, aucun module, **et une trace de pile vide** — parce qu'il n'y a pas
de cadre a l'adresse 0. Ce n'est pas un dereferencement de pointeur nul : c'est un
**appel** a travers un pointeur de fonction nul.

### La cause

`hsf` et `hookFunc` partagent la meme memoire (`include/game/hu3d.h:300`) :

```c
union {
    HSFDATA *hsf;
    HU3DMODELHOOK hookFunc;
};
```

`Hu3DModelKill`, sur un modele porteur d'un hook, libere ses donnees puis :

```c
temp_r31->hsf = NULL;
if (modelKillAllF == 0) {
    HuMemDCFlush(HEAP_DATA);
}
return;
```

Il met donc la fonction a zero **et sort sans retirer `HU3D_ATTR_HOOKFUNC`.**
L'attribut continue d'annoncer un hook qui n'existe plus.

`Hu3DDrawPost` lit exactement cette paire, sans verification :

```c
if (drawObj->model->attr & HU3D_ATTR_HOOKFUNC) {
    hookFunc = (void *)drawObj->model->hsf;
    hookFunc(drawObj->model, drawObj->matrix);
```

`Hu3DDraw` met le modele dans la file de dessin tot dans l'image ; tout ce qui le
tue avant que `Hu3DDrawPost` ne la vide — y compris un hook plus ancien dans la
meme passe — laisse une entree dont la fonction est nulle. L'appel part a zero.

Le depot avait deja la trace de cette ambiguite sans l'avoir lue comme telle :
`PartyBoard_RollbackRenderCanReplayWithoutDraw` teste
`Hu3DData[i].hsf && (attr & HOOKFUNC)` — il utilise `hsf` comme test de vie
**parce que l'attribut seul ne suffit pas**.

### Le correctif, en deux endroits qui ne font pas double emploi

1. **A la source** : `Hu3DModelKill` retire `HU3D_ATTR_HOOKFUNC` en meme temps
   qu'il annule la fonction. Un hook detruit cesse de s'annoncer. C'est le
   correctif.
2. **Au site d'appel** : `Hu3DDrawPost` refuse un pointeur nul au lieu d'y
   sauter, et l'inscrit comme evenement nomme. Ne pas dessiner un hook mort est
   correct ; sauter a zero est un plantage sans pile. Si le cas revient par un
   autre chemin, il sera nomme au lieu d'etre devine.

### Ce qui reste a faire

Le rejouer sur le scenario qui l'a produit trois fois, sans le correctif puis
avec. Tant que ce n'est pas fait, il est inscrit comme **non demontre rouge**,
au meme rang que le correctif du harnais et pas au rang de D3, D4 et D5.

---

## D23 — la comparaison rouge/vert, telle qu'elle se presente a 23h00

Protocole ecrit avant les resultats, comme celui du remplissage. **Meme code
source des deux cotes**, meme scenario (`--netplay-walk-plan=0:0`, mode Fete,
`w01`), meme machine, en parallele. Une seule chose differe :
`PARTYBOARD_HOOK_TICK_GATE`.

| bras | garde | run | tour atteint | frames | verdict |
|---|---|---|---|---|---|
| **vert** | armee | `fin-w01` | **5** | **172 454** | **toujours en vie a 1 h 23** |
| **rouge** | eteinte | `rouge-0` | 3 | 72 726 | `DESYNC:OBJECTS` a 26 min |

**Un run contre un run.** Ce n'est pas un resultat, c'est un premier point. Le
run vert a plus du double des frames du rouge et continue ; le rouge est mort
dans la categorie qui, depuis ce soir, resiste a tout — `OBJECTS`, des positions
flottantes.

Trois runs rouges supplementaires sont prevus. Le bras vert, lui, n'aura son
verdict qu'a la fin de sa partie : `minTurns=20`, donc il echouera s'il
s'arrete avant le classement, meme sans diverger.

### Ce que la comparaison ne pourra pas dire

Elle ne dira pas si la garde est **le** correctif. Elle dira si elle change le
taux de divergence sur ce scenario. Sept defauts distincts ont ete mesures ce
soir, de cinq formes ; la garde ne s'adresse qu'a ceux qui passent par un hook
de dessin, et rien ne dit qu'ils soient tous de ce genre.

### Un trou dans le correctif lui-meme — trouve par lecture, et un raisonnement a retirer

**Correction d'une inference fausse, ecrite ici puis retiree le meme soir.**
J'avais presente les morts du bras rouge dans les menus comme la preuve d'un trou
dans la garde. Elles ne prouvent rien de tel : **dans le bras rouge la garde est
eteinte**, donc qu'un ecran y meure ne dit rien de ce que la garde couvre. Le
raisonnement etait invalide et il est retire.

Ce qui reste, et qui tient tout seul, est une lecture. En cherchant pourquoi les
menus sont fragiles, j'ai regarde le chemin de la video du menu des modes — et
**la premiere version de la garde ne couvrait que les hooks de modeles.** Le menu des modes joue sa video comme un **sprite**
(`HuTHPSprCreateVol`), et `HuSprDisp` (`src/game/sprput.c:77`) appelle
`sprite->func(sprite)` depuis la passe de dessin :

```c
if(sprite->attr & HUSPR_ATTR_FUNC) {
    if(sprite->func) {
        func = sprite->func;
        func(sprite);
```

`HuSprExec` tourne une fois par image rendue, et — contrairement a
`PartyBoard_AnimationAdvance`, deux lignes plus bas dans `Hu3DExec` — cet appel
n'est derriere aucune garde de tick.

La porte de sûrete du rollback nommait deja `"sprite-draw-hook"` comme raison de
refuser un rejeu sans dessin (`src/game/hsfman.c:2361`). Le meme aveuglement
symetrique que pour les modeles, au meme endroit du code.

**Ce que ce correctif est, et ce qu'il n'est pas.** Il ferme un chemin qui fait
avancer de l'etat depuis le dessin, etabli par lecture, exactement comme celui
des modeles. Il n'est **pas** demontre que ce chemin cause D20 : cela reste a
mesurer, et le bras vert refait avec la garde complete est ce qui le dira.

La garde couvre desormais les deux, et elle a ete deplacee dans le port
(`PartyBoard_HookTickGateEnabled`) pour que les deux sites de dessin lisent une
seule decision au lieu d'en garder chacun une copie.

### Une precaution sur les autres binaires

Le meme jour, `kinopio-vert` a franchi l'endroit ou `kinopio-rouge` etait mort
(1 891 frames contre 7 603). **Cela ne compte pas comme une preuve de D24** :
trois choses differaient entre ces deux binaires — le correctif du hook nul, la
trace de la video, et la navigation de la liste des mini-jeux. Un run qui va
plus loin avec trois changements ne dit lequel a compte. C'est inscrit ici pour
qu'on ne le relise pas plus tard comme un resultat.

---

## D14 — annexe du 2026-09-12 : la cause, et la demonstration

### Ce qui etait deja fait, et pourquoi ca ne suffisait pas

Le port derive la **position** de la video des ticks de simulation acceptes et
non du peripherique audio, avec un auto-test qui l'epingle. Cette moitie-la
fonctionne : mesuree pendant toute l'attente, la position des deux pairs est
identique a l'unite pres (iter 60 -> 122, 120 -> 152, 180 -> 182, 240 -> 212).

Mais le test de fin ne lit pas que la position :

```cpp
bool ended() const {
    if (looped) return false;
    return stopped.load(...) || playback_frame() >= frames.size();
}
```

Et `stopped` est pose par le **mixeur, sur le thread audio**, quand sa file se
vide (`src/port/thp_player.cpp:179`) :

```cpp
} else {
    stopped.store(true, std::memory_order_relaxed);
    break;
}
```

**La video se terminait donc quand la carte son avait fini de jouer.** Le
commentaire ecrit a cote de `stopped` — « set by HuTHPStop from game logic, so it
is already the same on both peers » — etait vrai de `HuTHPStop` et faux du
drapeau, que le mixeur ecrivait aussi.

### La mesure, avant

Deux pairs, meme binaire, meme scenario :

| | position video | frame de simulation |
|---|---|---|
| pair 0 | 214 | **1906** |
| pair 1 | 213 | **1904** |

Deux frames d'ecart a la sortie de l'attente. Juste apres, `WipeColorSet` puis
`WipeCreate(WIPE_MODE_OUT, ..., 10)` — et `WipeCreate` abandonne en silence si un
fondu tourne deja. Un pair creait le sien, l'autre non, et l'ecart se figeait :
c'est **D20** en entier, qui n'etait donc pas un defaut a part mais la
consequence visible de celui-ci.

### Le correctif

Deux sens partageaient un drapeau ; ils sont separes :

- **`stopped`** — le **jeu** a demande l'arret, par `HuTHPStop`. De la logique de
  jeu, donc deja identique des deux cotes.
- **`audioDrained`** — le **peripherique** s'est vide. Une propriete de cette
  machine, et jamais une raison de terminer une video que deux pairs attendent
  ensemble.

Hors ligne les deux comptent toujours : rien ne change pour un joueur seul.

### La mesure, apres

```
peer 0 : thp_end ticks=433 logical=216 playback=216 frames=216 stopped=0 drained=1 looped=0
peer 1 : thp_end ticks=433 logical=216 playback=216 frames=216 stopped=0 drained=1 looped=0

peer 0 : modesel_movie_end thp_frame=215 thp_total=216 wipe_stat=0 frame=1909
peer 1 : modesel_movie_end thp_frame=215 thp_total=216 wipe_stat=0 frame=1909
```

Meme nombre de ticks, meme position, **meme frame de simulation a la sortie**. Et
`drained=1` des deux cotes : le peripherique s'etait bien vide, il ne decide
simplement plus. Le run a poursuivi au-dela de la frame 4868, la ou quatre runs
consecutifs mouraient entre 1888 et 1908.

### Ce que cette demonstration vaut, et ce qu'elle ne vaut pas

C'est une chaine causale complete : le drapeau fautif est identifie, sa valeur
est relevee des deux cotes avant et apres, et la grandeur qu'il faussait — la
frame de sortie — est passee de 1904/1906 a 1909/1909.

Ce n'est **pas** une statistique. Un seul run apres correctif. La regle du depot
demande vingt rejeux pour un defaut intermittent, et D14 en est un : il ne tuait
pas tous les runs — `fin-w01` avait franchi ce meme ecran sans encombre. Le
correctif reste donc **a confirmer en volume** avant d'etre compte comme clos.

---

## D25 — la vitesse d'animation des quatre joueurs : 1,0 contre 0,9

**Classification : divergent animation speed on the four player models.**

**Statut : champs nommes, valeurs exactes connues, setter non encore identifie.**

**Niveau de preuve : SCRIPTED**, une occurrence, `video-01`, 2026-09-12 a 23h33,
sur le binaire portant le correctif D14.

### Ce qui a ete mesure

Divergence a la frame 12235, **overlay 89, `w01Dll` — le plateau**. **8 champs
sur 4496**, et ce sont deux champs sur quatre modeles :

```
FIELD 2896  (model->motWork).speed        3f800000  contre  3f666666
FIELD 2904  (model->motShiftWork).speed   3f800000  contre  3f666666
... quatre fois, a 43 champs d'intervalle
```

`3f800000` est **1,0** et `3f666666` est **0,9**. Quatre modeles espaces
regulierement : ce sont les quatre joueurs.

### Pourquoi cette occurrence vaut plus que les autres

Ce n'est ni une derive ni un arrondi : **ce sont deux valeurs discretes**. Un
pair a pose 0,9, l'autre a laisse 1,0. Il y a donc un appel, quelque part, qui a
eu lieu d'un cote et pas de l'autre — ou avec un argument different.

Les deux champs changent ensemble sur chaque modele, donc un seul appel les pose
tous les deux.

### Ce qui a ete ecarte

`src/game/board/player.c:1999` calcule une vitesse de marche par
`var_f27 = 1.0f / (arg2 / 59.0f)`, ou `arg2` est une duree en frames. Cette
formule donne exactement 1,0 quand `arg2` vaut 59, mais **elle ne peut pas donner
exactement `0x3f666666`** : il faudrait que `arg2` soit non entier. La valeur
0,9 vient donc d'un litteral, pas de ce calcul.

Le seul `MotionSpeedSet(..., 0.9f)` litteral du depot est dans `m460Dll`, qui
n'a rien a voir avec un plateau.

### La suite

Trouver l'appel qui pose 0,9 sur les quatre joueurs d'un plateau. C'est une
recherche bornee : deux valeurs connues, quatre modeles, un seul overlay, et le
champ est desormais hache donc la prochaine occurrence sera signalee a la frame
ou elle se produit.

---

## D23 — verdict de la comparaison rouge/vert, 2026-09-12 a 23h40

| bras | garde | runs | divergences |
|---|---|---|---|
| rouge | eteinte | 4 | **4** |
| vert, hooks de modeles seuls | armee | 1 (`fin-w01`) | 0, **toujours en cours** au tour 8 |
| vert, modeles **et** sprites | armee | 2 | **2** |

**La garde n'est pas demontree utile.** Le bras vert complet a diverge deux fois
sur deux, dont une exactement au meme endroit et a la meme frame que les rouges.
Le seul run vert qui dure est `fin-w01`, et c'est **un** run : il ne porte pas de
conclusion a lui seul, d'autant qu'il tourne sur le binaire a garde partielle.

Il faut donc le dire dans ce sens et pas dans l'autre : **la garde des hooks de
dessin reste justifiee par la lecture** — ces appels font bien avancer de l'etat
depuis le chemin de rendu, `stalled_ticks` compte exactement les images
supplementaires concernees, et le depot lui-meme refuse deja de rejouer un tick
sans dessiner a cause d'eux — **mais aucune mesure ne montre pour l'instant
qu'elle empeche une divergence.**

Ce qui a demontrablement aide ce soir, c'est D14 : cinq runs mouraient entre les
frames 1888 et 1908 ; apres le correctif, trois runs sur trois franchissent ce
point, dont un `PASS` complet.

### Ce que je ne ferai pas

Presenter `fin-w01` comme la preuve de la garde. Ce run a commence avant que les
deux autres correctifs de la soiree n'existent, il tourne sur un binaire
different des deux bras, et il n'a pas fini. Il prouve une chose et une seule,
qui suffit deja : **une partie peut tenir plus de deux heures et huit tours sans
interruption**, ce qu'aucun run du projet n'avait fait.

---

## D26 — un mini-jeu qui ne se termine jamais, et le marcheur qui l'y aide

**Statut : blocage mesure, cause probable identifiee, non demontree.**

**Niveau de preuve : SCRIPTED.** Une occurrence, `fin-w01`, 2026-09-12 a 23h55.

### Ce qui a ete mesure

Le run le plus long du projet — 276 000 frames, deux heures et demie de jeu sans
interruption, tour 8 sur 20 — s'est arrete d'avancer sans mourir. La simulation
tourne, les frames defilent, mais :

```
game_context=52 overlay=52 minigame=43 board=0 turn=8 max_turn=20
overlay_transition_frame=216088 frames_since_transition=60247
```

**60 247 frames dans le meme mini-jeu**, soit seize minutes et quarante-quatre
secondes de temps de jeu. Overlay 52 = `m444dll` = **MIRACLE PINBALL**. Aucun
mini-jeu de ce jeu ne dure plus d'une minute.

Ce n'est ni un plantage ni une divergence : c'est un run qui tourne a vide. Sans
le compteur de tours, il aurait consomme ses quatre heures de budget et rapporte
276 000 frames — un chiffre qui aurait eu l'air excellent.

### La cause probable, et pourquoi elle est genante

Le rythme du marcheur appuie sur START toutes les 240 frames. Dans un mini-jeu,
START appelle `MGSeqPauseKill()` (`src/game/objsysobj.c:86`), **qui tue la
sequence du mini-jeu**. Si c'est cette sequence qui decide de la fin, la tuer
toutes les quatre secondes empeche le mini-jeu de se terminer.

Valentin l'avait signale deux fois — « sur les mini-jeu tu appuie tout le temps
sur pause, pourquoi ? » — et la consequence etait pire que gênante a l'oeil : le
marcheur empechait peut-etre les mini-jeux de finir.

### Ce qui a ete fait

START n'est plus envoye que sur l'ecran de demarrage quand un plan est arme
(`--netplay-walk-plan`). Partout ailleurs, A.

### Ce qui reste a demontrer

Que MIRACLE PINBALL se termine normalement sans ces appuis. Tant que ce n'est pas
mesure, deux lectures restent possibles : soit le marcheur empechait la fin, soit
ce mini-jeu ne se termine pas tout seul et c'est un defaut du jeu porte. La
difference compte, et un run suffira a trancher.

### Une lecon de harnais

Un run qui n'avance plus doit etre detecte **pendant** qu'il tourne, pas
reconstitue apres coup. `minTurns` attrape le cas a la fin ; il faudrait un
verrou qui arrete un run reste plus de N frames dans le meme overlay, avec N
choisi d'apres la duree reelle du plus long mini-jeu. Sans cela, une nuit entiere
peut se passer dans une table de flipper.

---

## 2026-09-13, 02h21 — la premiere partie menee jusqu'a son dernier tour

Ce n'est pas un defaut, c'est un fait, et il n'avait jamais eu lieu.

`fin05-0`, campagne `finp2`, binaire `build/d24` :

```
result = PASS
board_turn = 5   board_max_turn = 5   min_turns = 5
duration = 2702 s      last_frame = 160203
extra_arguments = --netplay-walk-plan=0:0 --netplay-max-turns=5
hook_tick_gate = 1     coverage_source = SCRIPTED   coverage_ceiling = PARTIAL
```

**Douze mecaniques de plateau dans une seule partie** : `BATTLE`, `BLOCK`, `CPU`,
`DICE`, `ITEM`, **`LAST5`**, `LOTTERY`, `MUSHROOM`, `SHOP`, `STAR`, `TUTORIAL`,
`WARP`. Six mini-jeux joues. Et le chemin se termine par
`w01Dll -> mstory3Dll` : **le module de fin de partie a tourne.**

### Ce que cela etablit

- Une partie **atteint son dernier tour**. Aucun run du projet ne l'avait fait.
- Le code des **cinq derniers tours** (`LAST5`) s'execute.
- Le module de **fin de partie** est atteint.

### Ce que cela n'etablit pas, et qu'il ne faut pas laisser glisser

- **Ce n'est pas une partie de vingt tours.** Le nombre de tours a ete force a
  cinq par `--netplay-max-turns`, ce qui est inscrit dans le resultat. Une partie
  de cinq tours terminee prouve que le chemin de fin fonctionne ; elle ne prouve
  pas qu'une partie de vingt tours va au bout.
- **Le niveau de preuve reste `SCRIPTED`, plafond `PARTIAL`.** Aucun humain n'a
  joue, et les deux pairs sont sur la meme machine. Cela ne monte d'un cran ni la
  ligne `HUMAN` ni la ligne `REAL-NETWORK`, qui restent a zero.
- **La fin de partie n'est pas demontree complete.** Le run est entre dans
  `mstory3Dll` a la frame 75 715 et n'a plus change d'overlay jusqu'a la fin de
  son budget, 80 000 frames plus tard. Le module a donc tourne, mais rien ne dit
  qu'il s'est termine : c'est la meme forme que l'enlisement de MIRACLE PINBALL,
  et c'est la prochaine chose a regarder.

### Ce qui l'a rendue possible

Quatre correctifs de la nuit, dans l'ordre ou ils ont compte : D17 (le marcheur
choisissait le mode au hasard), D14 (la carte son decidait de la fin de la
video), la suppression de START hors des ecrans qui en ont besoin, et
`--netplay-max-turns`, qui rend la fin de partie atteignable en 45 minutes au
lieu de plus de deux heures.

---

## D27 — la sequence de fin de partie ne se termine pas, et c'est G6

**Statut : REPRODUIT DE FACON DETERMINISTE. C'est la premiere fois qu'un defaut
rapporte par un humain est reproduit par une campagne automatique.**

**Niveau de preuve : SCRIPTED**, deux runs sur deux, 2026-09-13 a 02h21.

### Ce qui a ete mesure

`fin05-0` et `fin05-1`, memes entrees, meme binaire. Leurs chemins d'overlays
sont **identiques a la frame pres** — ce qui confirme au passage que le marcheur
planifie est deterministe :

```
w01Dll@75711  ->  mstory3Dll@75715  ->  mstory3Dll@80623
```

puis plus aucune transition jusqu'a la fin du budget, **frame 160 203 et
160 568**. Quatre-vingt mille frames, vingt-deux minutes de temps de jeu, dans le
module de fin de partie, sans en sortir.

La partie a bien atteint son dernier tour : `board_turn = 5` sur `max_turn = 5`,
et les douze marqueurs de plateau ont ete touches, `LAST5` compris.

### Pourquoi c'est G6

`docs/defauts_graphiques.md` porte, releve par Valentin en jouant le 2026-09-12 :

> **G6** — Explosion de la tete de Bowser en accelere, et fin buggee : **le jeu
> continue malgre la victoire**.

« Le jeu continue malgre la victoire » et « le module de fin tourne quatre-vingt
mille frames sans se terminer » decrivent le meme evenement, vu d'un cote par un
joueur et de l'autre par un chemin d'overlays.

J'avais ecrit dans cette page que G6 « ne decrit pas un probleme de rendu : cela
decrit une condition de fin qui ne se declenche pas ». C'etait la bonne lecture,
et elle est maintenant mesuree.

### Ce que cela change

Un defaut rapporte a l'oeil, sans recette, dans une liste de neuf defauts
graphiques dont j'avais ecrit qu'aucun ne serait reproductible en campagne
automatique — vient d'etre reproduit **deux fois sur deux, a la frame pres**.
Il a donc desormais une recette :

```
--netplay-walk-plan=0:0 --netplay-max-turns=5
```

quarante-cinq minutes, et le module de fin est atteint a la frame 75 715.

### Ce qu'il reste a faire

1. Lire `src/REL/mstory3Dll/` pour trouver ce que la sequence attend. La piste la
   plus proche est celle d'`instDll` : un ecran qui ne se ferme que sur une
   entree precise que le marcheur n'envoie pas. `mstory3Dll` a ete atteint deux
   fois (75 715 puis 80 623), donc quelque chose y avance avant de se figer.
2. Verifier si l'acceleration de l'explosion decrite dans G6 se produit aussi
   ici. Elle est de la famille de D6 et D14 — une animation pilotee par autre
   chose que le tick — et le correctif de D14 pourrait l'avoir deja changee.

## D23 — la demonstration, enfin, et elle vient d'un vrai reseau

Le 2026-09-13 a 11h05, la premiere session a deux machines physiques de
l'histoire du projet s'est arretee sur une desynchronisation. Elle porte la
preuve que quatre comparaisons rouge/vert en boucle locale n'avaient pas su
produire.

Preuve conservee : `docs/preuves/netplay_desync_2026-09-13_110521_player1.log`
(sha256 `c60e8dbf81d0a19278395f9ef4fe929aa75b00091e8348a3eebd4ec7d42d5df9`).

### Ce que le rapport dit

Session `4d503452`, hote joueur 0, `input_delay=3`, `full_game=1`, protocole 7,
hachage version 4. Contexte `13` — l'overlay `m405Dll`, le mini-jeu
`405:MEDREY RACE` — et `minigame=4`.

    first_desync_frame=12493  last_good_frame=12492
    category=RNG

Sur les seize sous-systemes du hachage canonique, **un seul differe** :

| sous-systeme | local | distant | |
|---|---|---|---|
| RNG | `e85f506c` | `f9cec1a1` | **DIFFERENT** |
| META, GAMEWORK, PLAYERS, BOARD, OBJECTS, PROCESSES, OVERLAY, ANIMATION, SEQUENCE, HEAPS, SCENE, INPUT, TIMERS, AUDIO, MINIGAME | | | identiques |

Les entrees des deux machines sont identiques aux frames 12492, 12493 et 12494.
`HEAPS` identique dit qu'aucune allocation ne differe. `rand8` est identique des
deux cotes ; seul `frand` diverge.

### La mesure, et non l'hypothese

`frand` est un generateur de Lehmer : depuis une graine donnee, la suite est
entierement determinee, donc le nombre de tirages entre deux valeurs se compte
en avancant le generateur. Le rapport porte la valeur de `frand` a chaque frame
pour les deux pairs, de 12373 a 12493 — cent vingt frames consecutives.

`docs/preuves/compte_tirages_frand.py` fait ce comptage :

    frames ou le nombre de tirages DIFFERE : 1 sur 120
      frame 12493 : local=63  remote=68  ecart=5

    profil des dernieres frames (local / distant) :
      12488: 43/43   12489: 48/48   12490: 53/53
      12491: 53/53   12492: 58/58   12493: 63/68

Cent dix-neuf frames au tirage pres identiques, puis **exactement cinq tirages
de plus sur la machine distante**, une seule fois. Le profil monte par paliers
de cinq : cinq tirages, c'est une unite.

### La chaine causale, lue dans le code

Cinq tirages, c'est le cout d'**une particule emise**. `ParManFunc`
(`src/game/hsfanim.c:1090`), le processus emetteur, tire pour chaque particule
qu'il cree : l'echelle, puis les trois composantes de la vitesse, puis l'index
de couleur.

Et la question de savoir **combien de particules il peut creer** ne se decide
pas dans la simulation. Elle se decide dans le dessin :

- `ParManHook` (`src/game/hsfanim.c:1262`) est un hook appele depuis
  `particleFunc`, c'est-a-dire depuis `Hu3DDrawPost` — le chemin de rendu. Il
  fait vieillir chaque particule (`particleDataP->time++`) et il la **tue** :

      if (particleDataP->scale < 0.01 || particleDataP->time >= param->maxTime) {
          particleDataP->scale = 0.0f;
      }

- `ParManFunc`, cote simulation, cherche les emplacements libres — ceux dont le
  dessin vient de mettre `scale` a zero — et tire cinq nombres pour chacun :

      while (particleDataP < particleDataEnd) {
          if (!particleDataP->scale) {
              ... frandmod(...) x5 ...

Le meme chemin de dessin avance aussi `particleP->count += minimumVcount`
(`hsfanim.c:855`), sous la garde `shadowModelDrawF == FALSE`.

**Donc : le rendu decide combien de particules meurent, et la simulation tire du
hasard partage pour chaque emplacement ainsi libere.** Une machine qui affiche
une image de moins fait vieillir ses particules une fois de moins, libere un
emplacement de moins, emet une particule de moins, et tire cinq nombres de
moins. La graine partagee diverge, definitivement.

L'etat des particules est volontairement hors du hachage canonique — c'est de la
presentation. C'est pourquoi la divergence n'apparait qu'au moment ou elle
atteint le generateur, et qu'elle apparait seule, `category=RNG`, tout le reste
identique. Ce rapport est la signature exacte de ce mecanisme.

### Ce que cela change pour le verdict de D23

Le 2026-09-12 a 23h40, la comparaison rouge/vert concluait a une absence de
demonstration : quatre runs rouges, deux verts, rien de concluant. Cette entree
ne contredit pas ce verdict — elle explique pourquoi il ne pouvait pas
conclure. **Les deux bras tournaient sur la meme machine.** Deux processus sur
un seul PC affichent le meme nombre d'images ; le defaut que la porte corrige ne
peut par construction pas s'y manifester. Il fallait deux machines, et deux
machines viennent de le produire en trois minutes et demie de jeu.

La porte `PARTYBOARD_HOOK_TICK_GATE` (`netplay_runtime.cpp:1833`, par defaut
**inactive**) fait exactement ce que la chaine ci-dessus demande : elle
n'execute les hooks de dessin que sur un tick de simulation. Elle reste a
demontrer — mais elle a desormais un defaut reel, reproductible par nature et
observable, contre lequel se mesurer.

### Ce qu'il ne faut pas en conclure

Que la session « a plante ». Elle ne l'a pas fait : le processus etait toujours
vivant et repondait apres l'arret. Le moteur a detecte la divergence et a arrete
la partie, comme la regle l'exige — on ne resynchronise pas, on ne recopie pas
l'etat de l'hote. Vu de l'ecran, un arret ressemble a un plantage ; c'est une
question d'affichage, pas de moteur.

### La porte entre dans la poignee de main — 2026-09-13, 11h13

Avant de tester la porte, il fallait rendre l'experience incapable de mentir.

`PARTYBOARD_HOOK_TICK_GATE` change ce que fait la **simulation**, et il etait
invisible de la signature echangee au demarrage. Deux machines pouvaient donc ne
pas etre d'accord dessus et etre quand meme admises dans la meme session — l'une
executant les hooks par image, l'autre par tick — et le seul symptome aurait ete
une desynchronisation quelques minutes plus tard, qui se lit exactement comme le
defaut que la porte est censee corriger. On aurait mesure le contraire de ce
qu'on croyait mesurer.

`runtimeConfigSignature` porte desormais un bit de plus,
`kRuntimeHookGateFlag = 0x00040000`, et le masque magique se resserre de
`0xfffc0000` a `0xfff80000` pour lui laisser la place. Un pair qui n'est pas
d'accord est **refuse a la poignee de main**, avec un message qui nomme la
cause :

    Netplay: session mismatch (local mode full/delay 3/hook-gate 1, remote
    signature 0x...). PARTYBOARD_HOOK_TICK_GATE must be set the same way on
    both machines.

Consequence voulue : un binaire plus ancien rejette purement et simplement un
pair dont la porte est active, parce que le bit 18 tombe encore dans SON masque.
Porte inactive, les deux restent interoperables.

L'auto-test du runtime verifie maintenant que **chaque** champ porte par la
signature la modifie — delai, contexte, mode complet, rollback et porte — parce
qu'un champ qui ne la modifie pas est un champ sur lequel deux pairs peuvent
diverger sans etre inquietes. `partyboard.exe --netplay-self-test` : 8/8, 0 echec.

Livraison : `Jouer en ligne (test D23).cmd` dans le paquet, qui pose la variable
et rappelle que les deux machines doivent l'utiliser. Empreinte du paquet
`1e541cac980cac59eb440b572e2eaa5f2a46b465d9a8ea21690a692aeacf77a7`.

**Ce qui reste non demontre :** que la variable traverse bien le salon jusqu'au
jeu. Le code dit que oui — `ChildProcess.Start` (`tools/online/Lobby.cs:12`)
pose les variables sur le processus courant puis appelle `Process.Start` avec
`UseShellExecute=false` sans toucher a `info.Environment`, donc l'enfant herite
de tout. Mais ce n'est pas une mesure. Le test qui le prouve tient en dix
secondes : une machine lance le raccourci de test, l'autre le raccourci normal.
Si le jeu refuse en disant `hook-gate`, la variable atteint bien le jeu. Ensuite
seulement, les deux passent au raccourci de test.

## D29 — une divergence d'ANIMATION en session humaine, et un instrument qui ne sait pas la nommer

2026-09-13, 11h23. Deuxième session à deux machines de la journée : six minutes,
**21 663 frames**, cinq mini-jeux enchaînés (`m406Dll` SKI RACE, `m407Dll`
BATTANDOMINO, `m408Dll` SKY DIVE, `m409Dll` CRAY SHOT, `m410Dll` JANJAN FREE
THROW). Arrêt sur désynchronisation dans `m410Dll`.

Preuve : `docs/preuves/netplay_desync_2026-09-13_112318_player1.log`.

    first_desync_frame=21663 last_good_frame=21662
    context=18 overlay=18 minigame=9
    category=ANIMATION
    SUBSYSTEM ANIMATION local=a25c53fb remote=13e85275 DIFFERENT

**Un seul sous-système diffère, et ce n'est pas le même qu'à 11h05.** Le RNG est
identique des deux côtés (`061b60ea`), ainsi que META, GAMEWORK, PLAYERS, BOARD,
OBJECTS, PROCESSES, OVERLAY, SEQUENCE, HEAPS, SCENE, INPUT, TIMERS, AUDIO et
MINIGAME. Les empreintes ANIMATION sont identiques à chaque frame de 21650 à
21662, puis divergent à 21663. C'est donc un défaut **distinct** de D23.

### Les deux machines racontent exactement la même histoire

Valentin a fourni les diagnostics des deux PC. Ils se répondent au miroir près :

    hôte   : ANIMATION local=a25c53fb remote=13e85275 DIFFERENT
    client : ANIMATION local=13e85275 remote=a25c53fb DIFFERENT

et les quinze autres sous-systèmes sont déclarés identiques des deux côtés. La
détection elle-même est donc saine : deux machines indépendantes arrivent au
même verdict. C'est la première fois que cela est vérifié.

### Pourquoi le champ reste inconnu

Le sous-système ANIMATION compte **7704 champs** dans ce rapport. Pour nommer
celui qui diffère, il faut les valeurs des deux côtés — et chaque machine ne
consigne que les siennes. Il suffirait de disposer des deux fichiers
`netplay_desync_*.log` pour les comparer champ par champ en une commande.

**Le paquet de diagnostic ne les collectait pas.** Il ramassait `session.txt`,
`native.txt` et `native.txt.desync`, c'est-à-dire tout sauf le seul fichier qui
porte les valeurs. D29 reste donc **localisé mais non nommé**, et c'est
entièrement la faute de l'outil.

### Une fausse piste, écartée en la vérifiant

En comparant les traces `mot_speed` des deux bundles, le client en portait
**quinze de plus** que l'hôte — ce qui ressemblait beaucoup à la cause. C'était
faux : `Report.Read()` ne lisait qu'un mégaoctet de `native.txt`, et les deux
bundles s'arrêtaient simplement à des endroits légèrement différents. La trace
réelle de l'hôte, conservée en entier, compte **2744** événements `mot_speed`
contre 1494 dans son bundle. La « divergence » était une troncature.

Notée ici parce qu'elle a été crue une minute : deux fichiers tronqués
différemment se comparent comme deux comportements différents.

### Trois corrections d'outillage, faites le jour même

1. **Le rapport de désynchronisation entre dans le paquet de diagnostic.** Deux
   paquets se diffèrent désormais au champ près. Vérifié sans donnée
   personnelle : ni chemin utilisateur, ni nom de compte, ni pseudo, ni adresse.
2. **`native.txt` est lu par la fin, plus par le début.** Une session qui meurt
   à la frame 21663 gardait les frames 0 à 12000 et jetait les minutes qui
   mènent à l'arrêt. La troncature est annoncée dans le fichier.
3. **Chaque section commence sur sa propre ligne.** L'en-tête était concaténé à
   la ligne précédente quand celle-ci ne se terminait pas par un saut : la
   section `--- native.txt.desync ---` était donc invisible à toute recherche
   ancrée en début de ligne, et j'ai cru pendant plusieurs minutes qu'elle
   n'existait pas.

### Et une quatrième, trouvée en chemin, qui vaut les trois autres

**`PartyBoardOnline.exe` ne se construit pas avec `cmake --build`.** C'est du C#
bâti par `tools/build_online.ps1`. J'ai modifié `tools/online/Report.cs`, lancé
la construction CMake, vu « 0 erreur, 0 avertissement », et failli empaqueter un
salon où la correction n'était pas — sans le moindre signal.

`package_release.ps1` surveillait `src/` et `include/` et pas `tools/online/`.
Il surveille maintenant les sources du salon contre **son propre binaire** —
paire séparée, puisque comparer du C# à `dol.dll` refuserait toute construction
C++ normale.

**Démontré dans les deux sens** : source rendue plus récente que le binaire →
`REFUS ... Le salon ne se construit PAS avec cmake`, code de sortie 2 ; salon
reconstruit → empaquetage accepté, code de sortie 0.

## D30 — sept blocs de trop dans HEAP_SYSTEM, et deux champs sur 2551

2026-09-13, 11h48. Troisième session à deux machines : **54 952 frames**, plus de
quinze minutes, sept mini-jeux. Arrêt dans `m415Dll` (`415:PYONPYON STAMP`).

    first_desync_frame=54952  last_good_frame=54951
    context=23 overlay=23 minigame=14
    category=HEAPS

**Troisième catégorie de la journée**, après RNG (11h05, `m405Dll`) et ANIMATION
(11h23, `m410Dll`). Trois sous-systèmes, trois mini-jeux : ce ne sont pas trois
symptômes d'un même défaut.

### Le champ, enfin nommé

C'est la première divergence de ce projet dont on connaisse le champ exact. Les
deux paquets de diagnostic — produits par le salon corrigé une heure plus tôt,
qui embarque désormais le rapport — se comparent par `compare_desync.py` :

    PREMIER CHAMP DIVERGENT : index 1017, sous-systeme HEAPS

    index  champ                             hote      client
    1016   HuMemHeapSizeGet(heap 0)          00900000  00900000
    1017   HuMemUsedMallocSizeGet(heap 0)    00075400  00079000   <<<
    1018   HuMemUsedMallocBlockGet(heap 0)   0000004f  00000056   <<<
    1022   HuMemUsedMallocSizeGet(heap 2)    007a3de0  007a3de0
    1023   HuMemUsedMallocBlockGet(heap 2)   00000691  00000691

    Total : 2 champs divergents sur 2551.

**Le client avait 7 blocs et 15 360 octets de plus que l'hôte dans le tas 0**
(`HEAP_SYSTEM`, 9 Mo). Tout le reste de l'état du jeu — RNG, objets, processus,
animations, tas 2 et 3, entrées, minuteurs — était identique au bit près.

Une piste, et pas une conclusion : `HEAP_SYSTEM` est notamment là où `chrman.c`
alloue les effets de personnage — poussière (`HOOKDUSTWORK` l.1332,
`NPCDUSTWORK` l.1710) et particules (l.708). Sept allocations de plus sans le
moindre autre effet observable, c'est la signature d'une création d'effet
déclenchée par autre chose que la simulation. À démontrer.

### Le champ a été retrouvé par calcul AVANT d'avoir le fichier du client

`docs/preuves/retrouve_heaps.py`. Le rapport ne porte que le hachage de
l'autre pair — mais HEAPS n'a que **quinze champs**, et le hachage par
sous-système est un FNV-1a/32 sur les octets gros-boutistes des valeurs, graine
2166136261 (`netplay_state.hpp:52,96`). L'espace est donc énumérable.

L'outil vérifie d'abord qu'il **reproduit l'empreinte locale annoncée**, et
refuse de chercher s'il n'y arrive pas — sans quoi tout résultat serait du bruit.
Puis il balaie les écarts d'occupation, tas par tas :

    empreinte locale recalculee : 43f7530a
    empreinte locale annoncee   : 43f7530a
    empreinte distante visee    : a5784131

    SOLUTION :
      heap 0 : +7 bloc(s), +15360 octet(s)

Le fichier du client, reçu ensuite, donne **exactement** ces valeurs : 86 blocs
contre 79, 495 616 octets contre 480 256. La méthode est donc vérifiée sur un
cas réel — pour une divergence HEAPS, une seule machine suffit désormais.

Elle ne se généralise pas : ANIMATION compte 7704 champs, et là il faudra
toujours les deux rapports.

## D30 — l'instrument, et les deux défauts qu'il a révélés en étant essayé

Le recensement des blocs demandé par D30 : quand le sous-système HEAPS diverge,
chaque machine liste ses blocs vivants avec leur taille et l'appelant que
l'allocateur consigne déjà. Deux listes se comparent, et l'écart nomme la
fonction.

### Comment on le démontre, puisqu'une vraie divergence ne se convoque pas

`--netplay-inject-heap=<frame>[:<blocs>]`, indicateur de test que le salon ne
passe jamais : à la frame indiquée, et **sur le seul pair qui porte
l'indicateur**, N blocs sont alloués dans `HEAP_SYSTEM` et volontairement
conservés. Les deux pairs cessent donc d'être d'accord sur leurs tas, exprès.

`tools/demo_heap_census.ps1` lance la paire locale et récolte les rapports.

### Le résultat

    failure=1 first_desync_frame=400 last_good_frame=399
    category=HEAPS
    SUBSYSTEM HEAPS local=a4891a24 remote=26781ce8 DIFFERENT

La divergence tombe **exactement à la frame injectée**, et sur le seul
sous-système visé. Le recensement sort 463 lignes, et la comparaison des deux
listes tient en une seule :

    > 7 HEAPBLOCK heap=0 size=2112 caller=dol.dll+0xfc7c7

Sept blocs, dans le tas visé, depuis une seule adresse — exactement ce qui a été
injecté. L'instrument est vérifié de bout en bout.

### Deux défauts trouvés parce qu'on l'a essayé

**1. `retaddr` valait 0 sur PC. Depuis toujours.** Le premier essai a produit
463 appelants `<hors-module>`. L'en-tête de bloc porte un champ `retaddr` depuis
l'original GameCube, où `mflr` le remplit ; sous `TARGET_PC`,
`src/game/malloc.c` écrivait simplement `u32 retaddr = 0;` aux quatre points
d'entrée de l'allocateur.

Conséquence rétroactive qu'il faut dire : **`PARTYBOARD_ALLOC_TRACE`, écrit le
2026-09-12 pour nommer l'appelant des allocations de D16, ne pouvait rien nommer
du tout.** Il imprimait `caller=00000000` à chaque ligne. Personne ne l'avait
remarqué parce qu'il n'avait jamais tourné jusqu'au bout.

Corrigé par `_ReturnAddress()`, et en `uintptr_t` plutôt qu'en `u32` : sur x64
un `u32` tronque l'adresse en une valeur qui n'appartient à aucun module.

**2. `num` n'est pas comparable entre machines.** Deuxième essai : les appelants
se résolvaient, mais *toutes* les lignes différaient. Le champ `num`, le groupe
d'allocation, est un **pointeur** pour la plupart des tas — 4031053600 d'un côté
contre 1183563552 de l'autre pour la même allocation. Retiré du recensement, qui
ne publie plus que le triplet comparable (tas, taille, appelant). Cela garde
aussi une adresse brute hors d'un fichier destiné à être échangé.

### Ce que cela ne démontre pas

Rien sur la cause du vrai D30. C'est l'instrument qui est sous test, pas le
défaut. La prochaine divergence HEAPS en session réelle nommera sa fonction ;
celle du 2026-09-13 à 11h48 restera nommée seulement par ses chiffres.

## D31 — 156 tirages d'écart d'un coup, et une contradiction utile

2026-09-13, 12h16. Quatrième session à deux machines, quatrième divergence,
deuxième de catégorie RNG. `m456Dll` (`456:MOGUTTE 1BAN`), frame 13994.

    first_desync_frame=13994  last_good_frame=13993
    context=62 overlay=62 minigame=55
    category=RNG   (seul sous-systeme different)

Le comptage des pas du générateur (`docs/preuves/compte_tirages_frand.py`) :

    frame 13991 : local=185  remote=185
    frame 13992 : local=61   remote=61
    frame 13993 : local=61   remote=61
    frame 13994 : local=217  remote=61     <-- +156 sur l'hote

Cette fois c'est **l'hôte** qui tire davantage, et l'écart est massif : 156 d'un
seul coup, là où D23 ce matin en comptait 5.

### L'interprétation évidente, et pourquoi elle ne tient pas telle quelle

156 = **3 × 52**, et trois tirages par particule est exactement le coût de
`Hu3DParticleCreate` (`hsfanim.c:523-525`). Le mini-jeu crée justement des
systèmes de 32, 64, 10, 6, 4 et 1 particules (`m456Dll/stage.c`), dont plusieurs
combinaisons font 52.

**Mais `Hu3DParticleCreate` alloue trois blocs dans `HEAP_DATA`** — et le
sous-système HEAPS est déclaré **identique** sur les deux machines à cette
frame. Une création de particules aurait divergé sur les deux sous-systèmes à
la fois.

Donc, de deux choses l'une : soit les 156 tirages viennent d'ailleurs, soit la
création a bien eu lieu des deux côtés et seule la *quantité* de tirages diffère.
Les deux sont vérifiables, aucune n'est vérifiée. C'est noté comme contradiction
et non résolu par la plus jolie des deux hypothèses.

### Ce que le nouveau paquet aurait donné

Rien de plus ici : le recensement des blocs ne s'écrit que quand HEAPS diverge,
et HEAPS ne diverge pas. Pour une divergence RNG, l'instrument qui manque est
l'équivalent côté générateur — enregistrer les appelants des tirages d'une frame
plutôt que leur nombre. C'est le prochain outil, s'il en faut un.

## D32 — le tampon du logo libéré huit octets trop loin, à chaque démarrage

Trouvé le 2026-09-13 en cherchant pourquoi un run scripté tombait toujours sur
la même ligne du balayage des mini-jeux.

    HuMemMemoryFree      memory.c:223       EXCEPTION_ACCESS_VIOLATION
    HuMemDirectFree      malloc.c:110
    NintendoDataDecode   bootDll/main.c:1109
    BootExec             bootDll/main.c:199

`NintendoDataDecode` lit deux entiers en tête du tampon avec `*src++`, puis
libère **`src`** — c'est-à-dire l'adresse du tampon **plus huit octets**.
L'allocateur cherche alors l'en-tête de bloc dans la charge utile.

Le plus souvent il refuse, et l'imprime : `HuMem>memory free error`, visible à
**chaque démarrage** depuis toujours, avec le tampon fuité au passage. Parfois
les octets trouvés là ressemblent assez à un en-tête valide, la libération est
acceptée, les chaînages voisins sont réécrits depuis un bloc inexistant, et le
processus meurt.

L'écart de huit octets se lit dans un journal réel : tas 3 basé à `97ed7040`,
première charge utile à `97ed7080`, adresse refusée **`97ed7088`**.

Ce n'est pas une régression du jour : le même plantage figure dans deux rapports
de campagne du 2026-09-12.

**Correctif** : évaluer `nintendoData` une seule fois — c'est une macro qui
**alloue** — et libérer ce pointeur-là. **Vérifié** : `memory free error` passe
de une occurrence par démarrage à **zéro**, et le run qui échouait passe.

## Une entrée rejouée ne met plus le jeu en pause

Les fichiers d'entrée générés alternent A et START indéfiniment, parce que START
est nécessaire pour quitter l'écran d'explications (`instDll/main.c:295` ne sort
que sur `btnDown == PAD_BUTTON_START`, une égalité stricte). Ils continuent de
l'envoyer une fois le mini-jeu lancé — où START appelle `MGSeqPauseKill()` et
**avorte la séquence**.

Les captures d'écran du 2026-09-13 le montrent sans ambiguïté : SKI RACE et
MEDREY RACE tous deux figés sur l'écran PAUSE. Le marcheur
(`--netplay-walk-plan`) avait déjà la bonne règle ; les fichiers rejoués ne
passaient pas par cette logique.

START est désormais retiré des entrées **rejouées** quand le contexte est un
overlay de mini-jeu. Le contexte est haché, donc les deux pairs retirent le même
bit à la même image. Cela répare tous les fichiers déjà générés sans les
régénérer. **Vérifié** : le mini-jeu se déroule maintenant jusqu'à son écran
« DRAW! » au lieu d'être avorté.

## Deux instruments ajoutés le 2026-09-13

**Le compteur du hook de particules.** `main.c:295` décrit déjà le mécanisme de
D23 dans son propre commentaire ; personne ne l'avait jamais mesuré. Le nombre
d'exécutions réelles de `ParManHook` est publié dans la trace à côté de
`global_counter` et du nombre d'images rendues. Si deux pairs diffèrent sur ce
compteur alors que le compteur de simulation concorde, la chaîne est prouvée.

**Le registre des tirages.** `frand` et `frandmod` enregistrent leur appelant
dans un anneau de 512 entrées, écrit seulement en ligne et jamais haché ; le
rapport l'imprime quand le sous-système RNG est celui qui diffère, chaque
adresse réduite à `<module>+0x<décalage>`. C'est l'équivalent, côté hasard, du
recensement des blocs — pour le sous-système qui a produit la moitié des
divergences réelles. La résolution d'adresse est désormais une seule fonction
partagée par les deux instruments.

## Avalanche! : une hypothèse écartée, et le bon sous-système

L'arc-en-ciel de losanges photographié dans `m406Dll` faisait penser à un indice
de couleur hors plage : `colorIdx = frandmod(param->colorNum)` indexe
`colorStart[4]` et `colorEnd[4]`, et la structure `HU3DPARMANPARAM` fait
exactement 0x4E octets — un indice supérieur à 3 lit donc au-delà d'elle.

**L'hypothèse ne tient pas pour ce mini-jeu** : `m406Dll` n'appelle jamais
`Hu3DParManCreate`. Le jeu entier n'en compte que trois usages statiques
(`board/roll.c`, `board/star.c`, `resultDll/battle.c`), aucun ici.

Ce que `m406Dll` utilise est `CharEffectLayerSet` — le système d'**effets de
personnage** de `chrman.c`, qui alloue ses données dans **`HEAP_SYSTEM`**. C'est
le tas exact où D30 a divergé de sept blocs. Le rapprochement est noté, pas
conclu.

## D27 / G6 — révision majeure : la fin de partie ne bloque pas

2026-09-13, fin de journée. Une partie de plateau de trois tours, pilotée
automatiquement de bout en bout, a été **menée jusqu'à sa cérémonie de fin et
photographiée**. C'est la première fois que ce projet observe cet écran.

### Ce qui a été vu, image par image

    Toad : « Well done! Here are the results! »
      -> les quatre personnages montent sur scene
      -> ecrans de statistiques : pieces gagnees en mini-jeux (capture 05),
         puis cases Warp (captures 11 et 29)

Trente captures espacées de vingt secondes, soit **dix minutes d'observation**
après l'entrée dans `mstory3Dll`. Résultats :

- **Les trente images sont toutes différentes** : le jeu anime en permanence, il
  n'est pas figé.
- **La page change** entre la capture 05 et la 29 : la séquence progresse d'un
  écran de statistiques à l'autre.
- `mismatch=0` du début à la fin, frame 89 280.
- **`endwait` n'a jamais été émis** : `fn_1_2420`, la fonction d'attente accusée
  jusqu'ici, **n'est même pas atteinte**.

### Ce que cela oblige à réviser

Le registre affirmait que la fin de partie bloque sur une attente sans délai de
sortie dans `fn_1_2420`, sur la foi de neuf runs de campagne « bloqués au
dernier tour ». **Cette hypothèse ne tient pas** : la fonction n'est pas
appelée, et le jeu tourne normalement.

Ce que les campagnes enregistraient comme un blocage est, selon toute
vraisemblance, **le bot incapable de quitter les écrans de statistiques**. Ces
écrans affichent « B Previous Screen » et deux flèches : ils se naviguent, et le
marcheur scripté qui n'envoie que A y tourne indéfiniment sans jamais en sortir.

Autrement dit : un défaut de l'outil de test, présenté pendant des jours comme un
défaut du jeu. Exactement ce que la règle « instrumenter avant de corriger »
existe pour éviter — et il aura fallu **regarder l'écran** pour s'en apercevoir,
ce qu'aucune campagne ne savait faire avant aujourd'hui.

### Ce qui reste ouvert, et qu'il ne faut pas balayer

Valentin a rapporté, en session humaine : *« la fin est buggée avec le jeu qui
continue malgré être le gagnant »*. Un humain appuie sur des boutons et franchit
ces écrans ; son observation décrit donc **autre chose**, et elle reste entière.
Elle sera tranchée par la première partie de plateau à deux machines, pas par un
run scripté.

### Conséquence sur le plan

L'étape 1 du plan — « corriger la fin de partie avant la prochaine session à
deux » — **tombe**. Rien ne prouve qu'il y ait quelque chose à corriger, et la
partie complète à deux machines peut être tentée directement.

## Les ecrans de fin de partie : deux boucles instrumentees, et un detecteur
## qui n'avait jamais ete arme — 2026-09-14

Suite directe de la revision D27 / G6. La fin de partie ne bloque pas, mais un
run automatique n'en sort pas ; avant de modifier le marcheur, on lit le code et
on mesure.

### Ce que le code dit, et qui a ete lu et non suppose

`mstory3Dll/result.c` contient deux boucles `while (TRUE)` ou la ceremonie peut
se garer :

- `fn_1_16924` attend A (ou MENU) **sur une seule manette**, celle que designe
  `unk38[unk04].unk14`. La seule sortie qui ne demande a personne d appuyer est
  la branche `unk14 == -1`, qui expire au bout de 300 images.
- `fn_1_16AD4`, les pages de statistiques, a **exactement une sortie** : B, sur
  cette meme manette unique. Elle est armee par `unk24`, un compteur qui doit
  depasser 5 — et **toute** poussee horizontale remet `unk24` a zero, sauf si la
  page est deja a sa butee (0 ou 11).

Autrement dit, cet ecran peut rester ouvert indefiniment tout en ayant l air de
progresser : les pages tournent, le decor s anime, et la seule porte reste
fermee. C est exactement ce que montraient les trente captures de la veille.

### L instrument

Les deux boucles disent maintenant, dans la trace : quelle manette elles lisent,
quelle page est affichee, combien vaut le garde-fou, et ce que la manette envoie
reellement. Toutes les 600 images, plus une ligne a l'entree et une a la sortie.
Aucune correction du marcheur ne sera ecrite avant de les avoir lues.

### Ou `minTurns` est arme, et ou il ne l est pas

La campagne sait classer `ABNORMAL_EXIT` un run qui atteint ses images sans
atteindre ses tours. Releve fait au lieu de suppose :

| manifeste | scenarios | `minTurns` |
|---|---|---|
| `tests/scenarios/generated.json` | `w04-monkey-1001`, `w01-monkey-2001` | **2** |
| `tests/scenarios/scenarios.json` | les quatre rejeux | absent |
| `tests/scenarios/regression-proofs.json` | les trois preuves | absent |

Le detecteur est donc bien arme la ou il compte le plus — les deux campagnes de
singe, qui sont les seules a pretendre jouer sans script. Les rejeux et les
preuves de regression ne le declarent pas ; pour eux la question se pose moins,
puisqu'un rejeu qui derive est deja attrape par sa comparaison de chemin
d'overlays.

*(Une premiere version de cette entree affirmait que le detecteur n avait jamais
pu se declencher. C etait faux : seul `scenarios.json` avait ete regarde. Le
releve ci-dessus est le bon.)*

### Ce qui manquait vraiment, et qui est ajoute

Le vrai trou est ailleurs, et aucun `minTurns` ne le bouche.

Ni `minFrames` ni `minTurns` ne peuvent voir un run coince a la FIN : les images
continuent de s'accumuler et les tours sont deja gagnes. Les deux passent, et le
verdict est `PASS`.

La campagne mesure desormais, sur chaque run, le sejour dans le **dernier**
overlay — le chemin d overlays porte deja `id@frame`, donc cela ne coute aucune
instrumentation — et un scenario peut le borner par `maxFinalOverlayFrames`.
Au-dela, le verdict devient `ABNORMAL_EXIT`. Absent ou 0 : aucune pretention, ce
qui est correct pour un run cense finir dans une ceremonie longue.

La valeur est enregistree sur **tous** les runs, y compris ceux qui ne la bornent
pas : un chiffre que personne n a demande est ce qui permet a quelqu un de
remarquer un motif plus tard.

### Et la progression, publiee la ou elle manquait

`turn`, `max_turn` et `board` figurent maintenant dans la ligne de diagnostic par
image. Ils existaient dans le fichier `live-state`, que seule la campagne lit ;
ni un paquet de diagnostic, ni un rapport de desynchronisation, ni une capture
pilotee ne pouvaient dire ou en etait la partie.


## D23 — ou l'injection doit frapper, et ou elle ne doit surtout pas — 2026-09-14

Preparation de la mesure prevue par le plan : faire sauter des images rendues a
un seul pair, pour voir si la chaine lue dans le code produit bien les +5 tirages
mesures. Le site a ete cherche dans le code avant d ecrire quoi que ce soit, et le
premier candidat evident est le mauvais.

### Le candidat evident

`src/game/main.c:328` appelle `Hu3DExec()` une fois par passage de boucle, donc
une fois par image presentee. Sauter cet appel reproduit litteralement une image
perdue.

### Pourquoi il est mauvais

Le commentaire de `main.c:102-112` le dit deja : `Hu3DExec` fait aussi avancer
`data->tick`, et cet etat **est hache**. Un pair qui saute `Hu3DExec` diverge donc
immediatement en categorie ANIMATION — une desynchronisation franche, bruyante, et
qui arriverait **avant** celle qu on cherche. La mesure ne dirait rien de D23 ; elle
mesurerait la sonde.

### Le bon site

Ce que D23 accuse est la garde de `ParManHook` (`prevCounter != GlobalCounter`),
mise a jour seulement quand le modele est **dessine**. Le passage qui appelle les
crochets de dessin est `Hu3DDrawPost` (`hsfman.c:341` et `2163`), pas `Hu3DExec`
en entier. L injection doit donc sauter ce seul passage, en laissant la simulation
et les horloges d animation intactes.

Note prise avant la construction, pour que le prochain geste vise le bon endroit.
Trois hypotheses plausibles ont deja ete refutees sur ce defaut ; une sonde mal
placee en aurait fabrique une quatrieme.

