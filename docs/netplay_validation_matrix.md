# Matrice de validation en ligne

Cette matrice est **derivee des donnees du depot**, pas d'une liste de memoire.
Les overlays et leurs numeros viennent de `include/ovl_table.h`, bloc
`TARGET_PC`, dans l'ordre ou le compilateur les numerote. Le meme calcul est
refait par `tools/netplay_campaign.ps1` quand il nomme un overlay dans un
resultat, de sorte qu'un ajout d'overlay ne peut pas desynchroniser ce
document du binaire sans que quelqu'un s'en apercoive.

## Vocabulaire

| statut | signification |
|---|---|
| `UNTESTED` | jamais exerce. C'est le statut par defaut, et il ne change que sur preuve. |
| `PARTIAL` | exerce, mais pas sur toute la liste de controle. Quatre tours valides restent `PARTIAL`. |
| `PASS` | toute la liste de controle exercee, deux pairs, aucun crash, aucune divergence. |
| `FAIL` | un defaut observe et enregistre. Reste `FAIL` jusqu'a un test de non-regression qui le couvre. |
| `BLOCKED` | inaccessible aujourd'hui, avec la raison ecrite. |

## Plateaux

Neuf overlays de plateau existent dans la table. `w01` a `w06` sont les six
plateaux principaux ; `w10`, `w20` et `w21` sont les autres terrains. Les noms
commerciaux ne sont volontairement pas inscrits ici tant qu'ils n'auront pas
ete lus depuis les donnees du disque plutot que depuis un souvenir.

| overlay | dll | sources dans l'arbre | statut | ce qui a ete exerce |
|---|---|---|---|---|
| 89 | `w01Dll` | oui | `UNTESTED` | rien |
| 90 | `w02Dll` | oui | `UNTESTED` | rien |
| 91 | `w03Dll` | oui | `UNTESTED` | rien |
| 92 | `w04Dll` | oui | `PARTIAL` | un replay enregistre : entree, plusieurs tours, quatre mini-jeux, quatre retours par l'ecran de resultats, evenement Big Boo |
| 93 | `w05Dll` | oui | `UNTESTED` | rien |
| 94 | `w06Dll` | oui | `UNTESTED` | rien |
| 95 | `w10Dll` | oui | `UNTESTED` | rien |
| 96 | `w20Dll` | oui | `UNTESTED` | rien |
| 97 | `w21Dll` | oui | `UNTESTED` | rien |

### Liste de controle par plateau

Un plateau n'est `PASS` que lorsque chacune de ces lignes a ete exercee a deux
pairs, sans crash et sans divergence :

- [ ] boot et entree sur le plateau
- [ ] premiers tours
- [ ] deplacements
- [ ] embranchements
- [ ] cases
- [ ] evenements
- [ ] etoiles
- [ ] boutiques
- [ ] Boo
- [ ] objets
- [ ] gimmicks propres au plateau
- [ ] mini-jeux
- [ ] retour mini-jeu vers plateau
- [ ] plusieurs tours consecutifs
- [ ] fin de partie

Etat actuel de `w04Dll` sur cette liste : les lignes *entree*, *deplacements*,
*mini-jeux* et *retour mini-jeu vers plateau* sont exercees par le replay
enregistre ; *evenements* l'est partiellement (Big Boo). Les autres sont
`UNTESTED`. C'est pourquoi le plateau reste `PARTIAL` et non `PASS`.

## Mini-jeux

Deux tables du depot, croisees : `src/REL/selmenuDll/main.c` donne les noms,
`src/game/objsub.c` donne le **type** de chaque mini-jeu dans `mgInfoTbl`.
Rien ici n'est inferé d'un souvenir.

### Ce que le type veut dire

`BoardMGSetupExec` tire `mgType` par `GetMGType()` (`board/mg_setup.c:722`)
d'apres la repartition des couleurs d'equipe, puis ne retient que les entrees
dont `mgInfoTbl[i].type == mgType` (`mg_setup.c:330`). Les trois valeurs qu'il
peut produire sont donc les trois modes de plateau. Le type 4 est nomme par
`board/battle.c:169`, qui ne selectionne que lui pour une case Battle.

| type | signification | source | mini-jeux |
|---|---|---|---|
| 0 | 4 joueurs | `mg_setup.c:330` | 16 |
| 1 | 1 contre 3 | `mg_setup.c:330` | 9 |
| 2 | 2 contre 2 | `mg_setup.c:330` | 9 |
| 3 | **non identifie** | aucun consommateur trouve dans le code decompile | 3 |
| 4 | battle | `battle.c:169` | 6 |
| 5 | **non identifie** | aucun consommateur trouve dans le code decompile | 1 |
| 6 | **non identifie** | aucun consommateur trouve dans le code decompile | 5 |
| 7 | **non identifie** | aucun consommateur trouve dans le code decompile | 7 |
| 8 | **non identifie** | aucun consommateur trouve dans le code decompile | 2 |

Les types 3, 5, 6, 7 et 8 existent dans la table mais aucun chemin decompile
consulte ici ne les nomme. `selmenuDll/main.c:723` traite 3, 5 et 6 a part au
lancement direct, ce qui suggere des categories hors plateau (histoire, duel,
salle de jeux), mais ce n'est pas etabli et ce document ne le prétendra pas.

### La liste

**58 mini-jeux**, dont quatre traverses par l'enregistrement existant.

| id | overlay | dll | nom | type | statut |
|---|---|---|---|---|---|
| 401 | 9 | `m401dll` | WAKUGURI DIVING | 0 (4 joueurs) | `UNTESTED` |
| 402 | 10 | `m402dll` | PURURUN! BIGSLIME | 0 (4 joueurs) | `UNTESTED` |
| 403 | 11 | `m403dll` | TAORERUKABE! | 0 (4 joueurs) | `UNTESTED` |
| 404 | 12 | `m404dll` | CRAYON RUNNER | 4 (battle) | `UNTESTED` |
| 405 | 13 | `m405dll` | MEDREY RACE | 0 (4 joueurs) | `UNTESTED` |
| 406 | 14 | `m406dll` | SKI RACE | 0 (4 joueurs) | `UNTESTED` |
| 407 | 15 | `m407dll` | BATTANDOMINO | 0 (4 joueurs) | `UNTESTED` |
| 408 | 16 | `m408dll` | SKY DIVE | 0 (4 joueurs) | `PARTIAL` |
| 409 | 17 | `m409dll` | CRAY SHOT | 0 (4 joueurs) | `UNTESTED` |
| 410 | 18 | `m410dll` | JANJAN FREE THROW | 0 (4 joueurs) | `UNTESTED` |
| 411 | 19 | `m411dll` | PAZZLE DE PONG | 0 (4 joueurs) | `UNTESTED` |
| 412 | 20 | `m412dll` | SNOW THROW | 0 (4 joueurs) | `UNTESTED` |
| 413 | 21 | `m413dll` | BOMBHEI PAZZLE! | 0 (4 joueurs) | `UNTESTED` |
| 414 | 22 | `m414dll` | NERATTE UTE! | 0 (4 joueurs) | `UNTESTED` |
| 415 | 23 | `m415dll` | PYONPYON STAMP | 0 (4 joueurs) | `UNTESTED` |
| 416 | 24 | `m416dll` | MAMORE FIRE | 1 (1 contre 3) | `PARTIAL` |
| 417 | 25 | `m417dll` | MARIO SURFER | 1 (1 contre 3) | `PARTIAL` |
| 418 | 26 | `m418dll` | TAIHOU KAKURENBO | 1 (1 contre 3) | `UNTESTED` |
| 419 | 27 | `m419dll` | BANANA DE KOROBASE | 1 (1 contre 3) | `UNTESTED` |
| 420 | 28 | `m420dll` | WATER BATTLE | 1 (1 contre 3) | `UNTESTED` |
| 421 | 29 | `m421dll` | BODY BALOON | 1 (1 contre 3) | `UNTESTED` |
| 422 | 30 | `m422dll` | BELCON COIN | 1 (1 contre 3) | `UNTESTED` |
| 423 | 31 | `m423dll` | GOAL AND GOAL | 1 (1 contre 3) | `UNTESTED` |
| 424 | 32 | `m424dll` | CLANE CATCH | 1 (1 contre 3) | `UNTESTED` |
| 425 | 33 | `m425dll` | AIR DOSSUN | 2 (2 contre 2) | `UNTESTED` |
| 426 | 34 | `m426dll` | KYOROKYORO PANIC | 2 (2 contre 2) | `UNTESTED` |
| 427 | 35 | `m427dll` | BOAT RACE | 2 (2 contre 2) | `UNTESTED` |
| 428 | 36 | `m428dll` | THE ROCK CLIME | 2 (2 contre 2) | `UNTESTED` |
| 429 | 37 | `m429dll` | TREASURE FOREST | 2 (2 contre 2) | `UNTESTED` |
| 430 | 38 | `m430dll` | PARASAILING GO | 2 (2 contre 2) | `UNTESTED` |
| 431 | 39 | `m431dll` | GURUGURU BOX | 2 (2 contre 2) | `UNTESTED` |
| 432 | 40 | `m432dll` | PAIR DE RACE | 2 (2 contre 2) | `UNTESTED` |
| 434 | 42 | `m434dll` | KINGYOSUKUI | 2 (2 contre 2) | `UNTESTED` |
| 435 | 43 | `m435dll` | KOOPA DARTS | 3 | `UNTESTED` |
| 436 | 44 | `m436dll` | KOOPANO AREGA TABETAI! | 3 | `UNTESTED` |
| 437 | 45 | `m437dll` | FUSEN RAKUGO | 3 | `UNTESTED` |
| 438 | 46 | `m438dll` | SYAKUNETSU WANWAN ATTACK | 4 (battle) | `UNTESTED` |
| 439 | 47 | `m439dll` | GURUGURU DANGEROUS | 4 (battle) | `UNTESTED` |
| 440 | 48 | `m440dll` | NEO KOOPA BAKUDAN | 4 (battle) | `UNTESTED` |
| 441 | 49 | `m441dll` | HIRAHIRA CHOUCHO | 4 (battle) | `UNTESTED` |
| 442 | 50 | `m442dll` | SUIMYAKU HORE2 | 7 | `UNTESTED` |
| 443 | 51 | `m443dll` | DRUG RACE | 0 (4 joueurs) | `PARTIAL` |
| 444 | 52 | `m444dll` | MIRACLE PINBALL | 5 | `UNTESTED` |
| 445 | 53 | `m445dll` | KINOPIO HAMMER | 6 | `UNTESTED` |
| 446 | 54 | `m446dll` | 3MAI SOROERO! | 6 | `UNTESTED` |
| 447 | 55 | `m447dll` | IQ BLOCK | 6 | `UNTESTED` |
| 448 | 56 | `m448dll` | FUMIKURI | 6 | `UNTESTED` |
| 449 | 57 | `m449dll` | NOKO2 KOURA PAZZLE | 6 | `UNTESTED` |
| 450 | 58 | `m450dll` | LAST GAME | 7 | `UNTESTED` |
| 451 | 59 | `m451dll` | PAZZLE | 7 | `UNTESTED` |
| 455 | 61 | `m455dll` | BURUTTE 1BAN | 4 (battle) | `UNTESTED` |
| 456 | 62 | `m456dll` | MOGUTTE 1BAN | 0 (4 joueurs) | `UNTESTED` |
| 457 | 63 | `m457dll` | SUMOH | 8 | `UNTESTED` |
| 458 | 64 | `m458dll` | PSYCOLO BATTLE | 8 | `UNTESTED` |
| 459 | 65 | `m459dll` | Dr.WARIO | 7 | `UNTESTED` |
| 460 | 66 | `m460dll` | _(sans nom)_ | 7 | `UNTESTED` |
| 461 | 67 | `m461dll` | BOMBHEI SCRANBLE | 7 | `UNTESTED` |
| 462 | 68 | `m462dll` | _(sans nom)_ | 7 | `UNTESTED` |

Les quatre `PARTIAL` ont ete joues une fois, par un seul chemin d'entree. Leur
type dit dans quel mode ils *peuvent* tourner, pas dans lequel ils ont tourne.
## Comment une ligne change de statut

Une seule route : `tools/netplay_campaign.ps1`. Les scenarios vivent dans
`tests/scenarios/scenarios.json`, les enregistrements sont verifies par leur
empreinte avec `tools/verify_replays.ps1`, et chaque run laisse dans
`work/netplay-campaigns/<horodatage>/` de quoi comprendre deux semaines plus
tard ce qui s'est passe.

| scenario | ce qu'il couvre | duree |
|---|---|---|
| `w04-boot-smoke` | boot, selection de mode, entree sur le plateau | 120 s |
| `w04-results-unload` | quatre mini-jeux et quatre retours par l'ecran de resultats | 800 s |
| `w04-board-replay` | le meme enregistrement en entier | 900 s |

Un resultat possible et un seul par run : `PASS`, `CRASH`, `DESYNC`, `TIMEOUT`,
`ABNORMAL_EXIT`, `HARNESS_FAILURE`. Un echec n'est jamais efface parce que la
boucle continue, et un run qui a echoue n'est pas rejoue jusqu'a obtenir un PASS
par chance : il faut le demander avec `-RerunFailures`.

## Ce qui manque encore pour que cette matrice veuille dire quelque chose

- Un enregistrement par plateau. Aujourd'hui il en existe un seul.
- Un moyen de nommer les modes de mini-jeu (4 joueurs, 1v3, 2v2, battle,
  duel) depuis les donnees plutot que depuis une liste ecrite a la main.
- Les tests `SAVE`/`RESTORE`/`REPLAY` locaux, qui sont une dimension
  supplementaire de cette matrice et pas une ligne de plus.
