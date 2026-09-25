# Conversion PAL au runtime

Le jeu est compilé une seule fois avec `VERSION=1` (USA rev 1), mais on peut charger un
disque européen (PAL, 5 langues). Chaque `#if VERSION_PAL / VERSION_NTSC / VERSION_ENG`
qui dépend **des données du disque** (fichiers, messages, polices, mises en page des textes)
exécutait jusqu'ici la variante USA sur des données PAL. Ce document recense ce qui a été
converti en choix au runtime, ce qui a été gardé volontairement, et ce qu'il faut tester.

Branche : `expert/pal`. Base : `d4acdce`.

## Principe

- Nouvel en-tête `include/port/version_runtime.h` :
  - `VERSION_RT_PAL`, `VERSION_RT_NTSC`, `VERSION_RT_ENG`
  - sur PC : le disque chargé (`partyboard_version_is_pal()`) ;
  - ailleurs (build GameCube) : exactement `VERSION_PAL` / `VERSION_NTSC` / `VERSION_ENG`.
- Forme type d'une conversion (même style que les conversions existantes de `bootDll`, `filesel.c`, `window.c`) :

  ```c
  #ifdef TARGET_PC
      if (VERSION_RT_PAL) { ...variante PAL... } else { ...variante USA... }
  #elif VERSION_PAL
      ...code d'origine...
  #else
      ...code d'origine...
  #endif
  ```

  Les expressions `VERSION_NTSC ? a : b` deviennent `VERSION_RT_NTSC ? a : b`.
- Tables statiques : les deux variantes sont gardées sur PC et un pointeur/macro choisit
  (`charWETblPal`, `FontCharFilePal`, `staffDataPal`, `lbl_1_data_230_pal`...). Les grosses
  tables à `#if` internes sont sorties dans un `.inc` inclus deux fois
  (`include/REL/instDll/font_tbl.inc`, `include/REL/mpexDll/font_tbl.inc`,
  `include/REL/staffDll/staff_tbl.inc`).
- **Jamais** de changement pour un disque USA (sauf les deux corrections signalées plus bas, qui
  rétablissent le comportement USA d'origine).
- **Build GameCube inchangé** : pour chaque fichier modifié, la sortie du préprocesseur sans
  `TARGET_PC` (avec `__MWERKS__`, `VERSION` = 0 à 5) est identique token pour token à celle de `d4acdce`.

### Pourquoi le 50 Hz n'est pas converti

Le port fait toujours tourner la logique à 60 Hz. Les branches PAL qui ajustent des nombres
d'images, vitesses, gravités ou durées pour 50 Hz sont donc **gardées en NTSC** : les convertir
ferait tourner un disque PAL trop vite ou trop lentement.

## Bilan (399 lignes `#if/#elif ... VERSION_` au départ)

| Catégorie | Lignes | Remarque |
|---|---:|---|
| Converties au runtime (ce travail) | 115 | + ~75 expressions `VERSION_X ? a : b` |
| Déjà converties par le port (revues) | 31 | 2 erreurs corrigées (voir plus bas) |
| Gardées : TIMING 50/60 Hz, physique | 125 | dont 112 dans les mini-jeux |
| Gardées : vidéo (VI/GX, rendu 528 lignes, luminosité des fondus) | 17 | |
| Gardées : logique/correctifs indépendants du disque | 32 | |
| Gardées : révision (`VERSION_REV*`) | 7 | USA rev 1 et PAL rev 2 sont toutes deux REV2 |
| Gardées : JP | 37 | étape ultérieure |
| Gardées : menu de debug `mstory4Dll` | 17 | la variante USA fonctionne sur disque PAL |
| INCERTAINES | 2 | voir plus bas |
| Fichiers non compilés par le port | 14 | `sreset.c`, `kerent.c`, `safDll`, `OdemuExi2` |
| `include/version.h` (définitions) | 2 | |
| **Non revues** | **0** | |

## Branches converties

### Code commun (`src/game`)

| Fichier | Où | Quoi |
|---|---|---|
| `window.c` | `charWETbl` (~l. 94) + `HuWinInit` (~l. 185) | Deux tables de largeur de police : USA et PAL (lettres accentuées). Avant, le PC utilisait la table PAL **aussi pour un disque USA** : un disque USA retrouve ses largeurs d'origine. |
| `minigame_seq.c` | `SeqWinCharNameGet` (~l. 2729), `MGSeqInitWin` (~l. 2787, 2912), `MGSeqUpdateWin` (~l. 3058-3070) | Nom « MINI BOWSER » au lieu de « KOOPA KID » et mise en page PAL des noms des gagnants. |
| `saveload.c` | `SLSaveDataInfoSet` (~l. 312) | Date du commentaire de la carte mémoire en JJ/MM/AAAA. |
| `saveload.c` | `SAVEWIN_POS` (~l. 516) | Position de la fenêtre de sauvegarde (messages PAL plus hauts). |
| `saveload.c` | `SLMessOut` cas 2, 3, 11 | Numéros de messages du fichier PAL (16/72, 16/76). |
| `board/view.c` | `CreateButtonWin` (~l. 399-431) | Fenêtre d'aide du mode vue placée d'après la largeur du texte PAL. |
| `board/window.c` | `BoardWinCreate` (~l. 112) | Fenêtre élargie de 4 px en PAL. |
| `board/shop.c` | `CreateShopItemChoice` (~l. 990, 1013), `MoveShopItemChoice` (~l. 1097) | Curseur de la boutique aligné sur la fenêtre PAL. |
| `board/roll.c` | `RollWinCreate` (~l. 189) | Variante PAL pour tous (surensemble) : la variante USA laissait `posX` **non initialisé** pour les langues 2-5. |
| `board/ui.c` | `CreatePickerWindow` (~l. 1977) | Idem (`yOfs` non initialisé pour les langues 2-5). |

### RELs

| Fichier | Quoi |
|---|---|
| `bootDll/main.c` (~l. 237) | Après le menu de langue PAL (`SystemInitF` déjà vrai), crée le logo Hudson et le titre comme le code PAL. **Sans cela le titre n'était jamais créé sur disque PAL au premier démarrage.** |
| `instDll/font.c` | Table `FontCharFile` USA et PAL (archive `inst` PAL avec lettres accentuées, numéros de fichiers décalés), suffixe kana NTSC ignoré en PAL, largeurs PAL, octets lus en non signé. |
| `instDll/main.c` | Écran d'instructions : 8 positions/animations de fenêtres PAL (`WIN_ANIM_OFS` 219, etc.). |
| `mpexDll/mgname.c` | Même chose que `instDll/font.c` pour les noms de mini-jeux de la salle extra. |
| `mpexDll/mpex.c` (~l. 2355) | Format du score inséré dans le message PAL. |
| `ztardll/font.c` | Table PAL (lettres prises dans l'archive `inst`, `0x0014xxxx` = `DATADIR_INST`, vérifié avec `datadir_enum.h`), largeurs PAL, octets non signés. |
| `ztardll/main.c` (~l. 1276) | Échelle 0.95 du message PAL. |
| `staffDll/main.c` | Générique : liste PAL (équipe de localisation), logo par langue, fichier suivant décalé (0x20), positions des logos. |
| `messDll/main.c` | Visionneuse de messages (debug) : variante PAL avec changement de langue (X). |
| `mentDll/main.c` | Menu d'entrée des modes (plateau, difficulté) : ligne 1 placée pour le texte PAL, sprites USA masqués. |
| `mgmodedll/battle.c`, `free_play.c`, `main.c` | Messages de description PAL (groupe 0x39), message 57 du mode 2, sprite 85. |
| `mstory2Dll/*.c` (6 fichiers) | **Numéros de fichiers des modèles/animations du mode histoire** (`mstory2.bin` PAL a des fichiers par langue) et sprite de fin par langue. Sans cela un disque PAL chargeait de mauvais modèles. |
| `option/record.c`, `option/window.c` | Écran des records : positions des chiffres, icônes masquées, largeurs de fenêtres PAL. |
| `w04Dll/big_boo.c` | Seul le message USA reçoit le compte inséré. |
| `w06Dll/fire.c` | Numéros de messages des noms d'objets PAL (8/0-13) et joueur courant pendant le message. |
| `m444dll/main.c` | Fenêtres dimensionnées avec les noms de personnages insérés (PAL). |
| `m448Dll/main.c` | L'ordinateur valide chaque page des messages PAL. |
| `m457Dll/main.c` | Message « shove » PAL (fenêtre) au lieu du sprite texte USA + cri de Bowser. |

### Conversions existantes corrigées (`modeseldll/filesel.c`)

- Message « pas de données » (~l. 517) : la condition était **inversée** (message PAL sur disque USA et inversement).
- Chargement d'une sauvegarde depuis l'écran de sélection (~l. 1125) : `_ClearFlag(0x1000B)` manquait
  pour le disque PAL (présent dans le code PAL et dans `FileSelectAutoLoadDefault`).

Les autres conversions déjà présentes (`bootDll/main.c`, `bootDll/language.c`, `window.c`,
`gamework.c`, `modeseldll/main.c`, `data_num/title.h`) ont été relues et sont correctes.

## Branches gardées volontairement

### TIMING (50/60 Hz, vitesses, physique) — gardées NTSC

- `gamework.c` `GWMessDelayGet` et `gamework_data.h` `GWMessSpeedSet` (délais de messages en images).
- `board/main.c:432` vitesse des messages du tutoriel.
- `staffDll/main.c` durées du diaporama (550/430 vs 600/480), `HuPrcSleep` 240/60 et 600/480.
- `mstory2Dll/ending.c` image d'apparition du texte de fin (3300 vs 2725).
- `mpexDll/mpex.c:1859, 2244` seuils de rang (images) ; `3458-3512` messages de log.
- `modeseldll/modesel.c:292` durée du fondu après la vidéo.
- `w04Dll/mg_coin.c:569` gravité.
- Mini-jeux `m401`–`m463` : 112 lignes (vitesses ×1.2, gravités, compteurs d'images, constantes
  `REFRESH_RATE`), p. ex. tout `m423Dll/main.c`, `m408Dll/*`, `m410Dll/*`, `m417Dll/*`, `m427Dll/*`.

### Vidéo — gardées NTSC

`init.c` (4 : VI/GX/mode de rendu), `main.c` (2 : `GXPal528IntDf`), `wipe.c:200` et
`m406Dll/main.c`, `m427Dll/main.c` (fondus limités à 160 en PAL), `m406Dll/map.c`,
`m430Dll/main.c` (cadrage 0x1D0 + bande PAL), `m439Dll/main.c` (zoom caméra),
`bootDll/main.c:61, 446` (écran balayage progressif, NTSC seulement).

### Logique indépendante du disque — gardées NTSC

`minigame_seq.c:382/387` (groupe du chrono placé hors écran), `board/window.c:393`
(`BoardWinPlayerSet`), `board/char_wheel.c` (3 arrêts de son), `card.c:111`, `audio.c:757`,
`port/audio.c:626`, `saveload.c:19` (drapeau pendant l'écriture), `REL/m431Dll.h`,
`bootDll/main.c:591` (caméra debug), `ztardll/main.c:1116/1132/1437` (groupes audio),
mini-jeux `m401`, `m409`, `m431`, `m432`, `m433`, `m440`, `m442`, `m455` (17 lignes).

### JP (37 lignes) — non touchées

`window.c` (10), `minigame_seq.c` (6), `saveload.c` (3), `gamework.c` (2), `ovl_table.h` (2),
`board/main.c`, `board/player.c`, `w01`–`w06` (6), `ztardll` (5), `m444dll/main.c:131`.

### Menu de debug `mstory4Dll` (17) — gardé NTSC

Accessible seulement par `selmenuDll` (« STORY TEST »). La variante USA fonctionne sur disque
PAL ; la variante PAL remplace « CLEAR FLAG » par un choix de langue.

## INCERTAINES (non modifiées)

| Fichier | Raison |
|---|---|
| `mgmodedll/mgmode.c:1193` | `Hu3DModelShadowMapSet` (USA) vs `Hu3DModelShadowMapObjSet(..., "base_fix9-base")` (PAL) : ombre ; dépend peut-être du modèle PAL, peut-être un simple correctif. |
| `m459dll/main.c:1308` | Effet sonore 0x814 (USA) vs 0x815 (PAL) : probablement un correctif (les autres numéros de sons sont identiques). |

## Risques connus

- Les délais 50 Hz gardés en NTSC peuvent décaler des séquences plus longues en PAL
  (générique : la liste PAL est plus longue que le diaporama USA ; fin du mode histoire).
- `window.c` : `mesWInsert` reste en `u16` pour les deux disques (choix existant du port).
- Les tables de `ztardll` PAL utilisent `DATADIR_INST` d'après les numéros bruts du code PAL
  (`0x0014xxxx`) ; la numérotation des répertoires est la même (141 entrées dans toutes les versions).
- Rien n'a pu être exécuté ici (pas de disque) : seulement des vérifications de syntaxe et du
  préprocesseur.

## À tester sur disque PAL

À faire dans au moins deux langues (allemand ou français pour les accents, espagnol pour `roll.c`).

1. **Démarrage** : premier lancement → menu de langue → logo Hudson puis titre affichés.
2. **Textes** : lettres accentuées (é, è, ü, ß, ñ) bien espacées dans les fenêtres (plateau, menus).
3. **Sélection de fichier** : carte vide (« pas de données »), chargement d'une sauvegarde.
4. **Sauvegarde** : position de la fenêtre, messages de l'emplacement, date JJ/MM dans le gestionnaire de carte.
5. **Plateau** : fenêtre du dé (toutes langues, espagnol plus bas), curseur de la boutique aligné sur
   les objets, fenêtre du mode vue (bouton), fenêtre de choix (`ui.c`), plateau Bowser : feu qui vole
   des objets (noms), plateau Boo : grand Boo.
6. **Fin de mini-jeu** : « MINI BOWSER » comme gagnant, 1 et 4 gagnants.
7. **Écran d'instructions** : titre avec accents, fenêtres qui glissent (règles, pages).
8. **Salle extra (mpex)** : noms de mini-jeux avec accents, message de record.
9. **Mode histoire** (prioritaire) : entrée de plateau, plateau gagné/perdu, mini-jeu gagné/perdu,
   fin : bons modèles et animations, sprite de fin dans la langue.
10. **Générique** : liste PAL, logos, fin.
11. **Salle d'options** : écran des records.
12. **Menus** : entrée des modes histoire/fête (`mentDll` : choix du plateau, difficulté), mode
    mini-jeux : bataille (descriptions), jeu libre.
13. **Mini-jeux** `m444`, `m448`, `m457` (textes) et `ztardll` (écran qui lance `m433` : noms en lettres-sprites, message).

Sur **disque USA**, refaire au moins 2, 6, 7 et 9 : aucune différence attendue, sauf les largeurs de
police (2) qui reviennent à celles du jeu USA d'origine.

## Vérifications faites

- `clang` et `gcc` `-fsyntax-only` (options PC du dépôt, + `-I$M/extern/musyx/include -DMUSY_TARGET=0`
  pour les fichiers qui incluent `msm.h`) sur tous les fichiers modifiés : OK.
- Build GameCube : préprocesseur sans `TARGET_PC`, avec `__MWERKS__`, pour `VERSION` 0 à 5,
  comparé à `d4acdce` : identique pour tous les fichiers modifiés.
- Nombre d'entrées des tables `.inc` : 198 (USA) / 304 (PAL), comme les tailles déclarées.
