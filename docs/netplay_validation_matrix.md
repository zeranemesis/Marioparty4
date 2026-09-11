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

La table de selection du menu developpeur (`src/REL/selmenuDll/main.c`) nomme
**58 mini-jeux**, de 401 a 462. Les noms ci-dessous sont ceux du
depot, pas des noms commerciaux : ceux-la vivent dans les donnees du disque et
seront lus de la quand quelqu'un en aura besoin.

Quatre ont ete traverses par l'enregistrement existant. Les autres n'ont jamais
tourne en ligne, dans aucun mode.

| id | overlay | dll | nom | statut |
|---|---|---|---|---|
| 401 | 9 | `m401dll` | WAKUGURI DIVING | `UNTESTED` |
| 402 | 10 | `m402dll` | PURURUN! BIGSLIME | `UNTESTED` |
| 403 | 11 | `m403dll` | TAORERUKABE! | `UNTESTED` |
| 404 | 12 | `m404dll` | CRAYON RUNNER | `UNTESTED` |
| 405 | 13 | `m405dll` | MEDREY RACE | `UNTESTED` |
| 406 | 14 | `m406dll` | SKI RACE | `UNTESTED` |
| 407 | 15 | `m407dll` | BATTANDOMINO | `UNTESTED` |
| 408 | 16 | `m408dll` | SKY DIVE | `PARTIAL` |
| 409 | 17 | `m409dll` | CRAY SHOT | `UNTESTED` |
| 410 | 18 | `m410dll` | JANJAN FREE THROW | `UNTESTED` |
| 411 | 19 | `m411dll` | PAZZLE DE PONG | `UNTESTED` |
| 412 | 20 | `m412dll` | SNOW THROW | `UNTESTED` |
| 413 | 21 | `m413dll` | BOMBHEI PAZZLE! | `UNTESTED` |
| 414 | 22 | `m414dll` | NERATTE UTE! | `UNTESTED` |
| 415 | 23 | `m415dll` | PYONPYON STAMP | `UNTESTED` |
| 416 | 24 | `m416dll` | MAMORE FIRE | `PARTIAL` |
| 417 | 25 | `m417dll` | MARIO SURFER | `PARTIAL` |
| 418 | 26 | `m418dll` | TAIHOU KAKURENBO | `UNTESTED` |
| 419 | 27 | `m419dll` | BANANA DE KOROBASE | `UNTESTED` |
| 420 | 28 | `m420dll` | WATER BATTLE | `UNTESTED` |
| 421 | 29 | `m421dll` | BODY BALOON | `UNTESTED` |
| 422 | 30 | `m422dll` | BELCON COIN | `UNTESTED` |
| 423 | 31 | `m423dll` | GOAL AND GOAL | `UNTESTED` |
| 424 | 32 | `m424dll` | CLANE CATCH | `UNTESTED` |
| 425 | 33 | `m425dll` | AIR DOSSUN | `UNTESTED` |
| 426 | 34 | `m426dll` | KYOROKYORO PANIC | `UNTESTED` |
| 427 | 35 | `m427dll` | BOAT RACE | `UNTESTED` |
| 428 | 36 | `m428dll` | THE ROCK CLIME | `UNTESTED` |
| 429 | 37 | `m429dll` | TREASURE FOREST | `UNTESTED` |
| 430 | 38 | `m430dll` | PARASAILING GO | `UNTESTED` |
| 431 | 39 | `m431dll` | GURUGURU BOX | `UNTESTED` |
| 432 | 40 | `m432dll` | PAIR DE RACE | `UNTESTED` |
| 434 | 42 | `m434dll` | KINGYOSUKUI | `UNTESTED` |
| 435 | 43 | `m435dll` | KOOPA DARTS | `UNTESTED` |
| 436 | 44 | `m436dll` | KOOPANO AREGA TABETAI! | `UNTESTED` |
| 437 | 45 | `m437dll` | FUSEN RAKUGO | `UNTESTED` |
| 438 | 46 | `m438dll` | SYAKUNETSU WANWAN ATTACK | `UNTESTED` |
| 439 | 47 | `m439dll` | GURUGURU DANGEROUS | `UNTESTED` |
| 440 | 48 | `m440dll` | NEO KOOPA BAKUDAN | `UNTESTED` |
| 441 | 49 | `m441dll` | HIRAHIRA CHOUCHO | `UNTESTED` |
| 442 | 50 | `m442dll` | SUIMYAKU HORE2 | `UNTESTED` |
| 443 | 51 | `m443dll` | DRUG RACE | `PARTIAL` |
| 444 | 52 | `m444dll` | MIRACLE PINBALL | `UNTESTED` |
| 445 | 53 | `m445dll` | KINOPIO HAMMER | `UNTESTED` |
| 446 | 54 | `m446dll` | 3MAI SOROERO! | `UNTESTED` |
| 447 | 55 | `m447dll` | IQ BLOCK | `UNTESTED` |
| 448 | 56 | `m448dll` | FUMIKURI | `UNTESTED` |
| 449 | 57 | `m449dll` | NOKO2 KOURA PAZZLE | `UNTESTED` |
| 450 | 58 | `m450dll` | LAST GAME | `UNTESTED` |
| 451 | 59 | `m451dll` | PAZZLE | `UNTESTED` |
| 455 | 61 | `m455dll` | BURUTTE 1BAN | `UNTESTED` |
| 456 | 62 | `m456dll` | MOGUTTE 1BAN | `UNTESTED` |
| 457 | 63 | `m457dll` | SUMOH | `UNTESTED` |
| 458 | 64 | `m458dll` | PSYCOLO BATTLE | `UNTESTED` |
| 459 | 65 | `m459dll` | Dr.WARIO | `UNTESTED` |
| 460 | 66 | `m460dll` | _(sans nom dans la table)_ | `UNTESTED` |
| 461 | 67 | `m461dll` | BOMBHEI SCRANBLE | `UNTESTED` |
| 462 | 68 | `m462dll` | _(sans nom dans la table)_ | `UNTESTED` |

Les quatre `PARTIAL` le sont parce qu'ils ont ete joues une fois, par un seul
chemin d'entree, dans un seul mode. Aucun n'a encore ete exerce en 1v3, en 2v2,
en battle ni en duel.

### Les modes ne sont pas encore enumerables

`GWSystem.mg_type` porte le mode, et `instDll/main.c:75` le tire au sort avec
`frandmod(3)` quand le plateau ne l'impose pas. Mais **aucune table du depot ne
dit quel mini-jeu accepte quel mode** : cette information vit dans les donnees
du disque. Tant qu'elle n'en aura pas ete extraite, une ligne `mini-jeu x mode`
serait inventee, et la matrice n'en contient pas.

Ce qu'il faut pour la construire : lire la table des mini-jeux depuis l'image
disque, comme `tools/extract_assets.py` le fait deja pour d'autres ressources,
et croiser avec les 58 identifiants ci-dessus.
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
