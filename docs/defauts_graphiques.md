# Défauts de rendu — observés par un humain, invisibles pour tout le reste

Cette page existe parce qu'une classe entière de défauts échappe à **tout** ce que
ce dépôt a construit pour se valider.

## Pourquoi aucun outil d'ici ne peut les trouver

Le hachage canonique exclut délibérément la présentation. `netplay_canonical.hpp`
le dit en toutes lettres :

> *Pointers, addresses, handles, padding, presentation buffers and physical audio
> state are never exported.*

Et `wipe.c` explique la même exclusion pour la couleur d'un fondu :

> *color.a is excluded on purpose: it is a presentation value […] It is only ever
> read by GXSetChanMatColor/GXSetTevColor, never by game logic.*

C'est un choix correct : deux machines qui affichent la même partie doivent être
déclarées d'accord même si leurs images diffèrent d'un pixel. Mais la conséquence
est nette — **une ombre manquante, un HUD décalé, un feu d'artifice qui bave, un
écran de fin qui ne se ferme pas : le hachage n'en saura jamais rien.**

Les 151 runs automatiques du 2026-09-12 étaient structurellement aveugles aux
neuf défauts ci-dessous. Seul quelqu'un qui regarde l'écran peut les voir.

## Ce qui a été rapporté

Relevé par Valentin le 2026-09-12, en jouant. Les noms sont les siens, tels que
le jeu les affiche. La colonne « module » est ma **supposition** : la matrice de
ce dépôt utilise les noms de développement japonais, qui ne correspondent pas
aux noms localisés, et je n'ai vérifié aucune de ces correspondances.

| # | ce qui a été vu | module supposé, NON vérifié |
|---|---|---|
| G1 | Problème graphique pendant les feux d'artifice de **Slime Time** | `m402Dll` PURURUN! BIGSLIME ? |
| G2 | Problème graphique sur **Avalanche!** | `m412Dll` SNOW THROW ? |
| G3 | Bug graphique sur **Right Oar Left** | `m427Dll` BOAT RACE ? |
| G4 | **Beaucoup** de bugs graphiques sur **Para-sailing** | `m430Dll` PARASAILING GO ? |
| G5 | Bug graphique sur **Trace Race** | `m404Dll` CRAYON RUNNER ? |
| G6 | Explosion de la tête de Bowser **en accéléré**, et fin buggée : **le jeu continue malgré la victoire** | non identifié |
| G7 | Les papillons de **Butterfly Blitz** n'ont **pas d'ombre** | `m441Dll` HIRAHIRA CHOUCHO ? |
| G8 | Plateau : le **ticket à gratter n'apparaît pas au fur et à mesure** du grattage | `src/game/board/lottery.c` ? |
| G9 | **HUD mal positionné** lors de l'utilisation du bloc dé | `src/game/board/block.c` ? |

## Celui qui n'est pas qu'un défaut d'affichage

**G6** mérite d'être distingué des huit autres. *« La fin est buggée avec le jeu
qui continue malgré être le gagnant »* ne décrit pas un problème de rendu : cela
décrit une **condition de fin qui ne se déclenche pas**. Si la séquence de
victoire ne rend jamais la main, la partie ne peut pas se terminer — ce qui
touche directement le critère d'acceptation du projet, *pouvoir jouer normalement
jusqu'à décider soi-même de quitter*.

Et l'accélération de l'explosion est la signature d'une animation pilotée par le
nombre d'images affichées plutôt que par les ticks de simulation — exactement le
mécanisme de D14 (la fin de vidéo) et de D6 (l'horloge d'animation). Ce n'est pas
une preuve, c'est une famille à laquelle il ressemble.

## Ce qu'il faudrait pour les instruire

Aucun de ces défauts ne se reproduira dans une campagne automatique telle
qu'elle existe. Pour les rendre observables il faudrait comparer **des images**,
pas des états : capturer le framebuffer à des frames données et le confronter à
une référence.

C'est un chantier entier, il n'existe pas, et il n'est pas dans le plan. En
attendant, ces défauts ne peuvent être trouvés **que** par quelqu'un qui joue —
et ils ne doivent pas disparaître pour autant, d'où cette page.

## Statut

Aucun n'est corrigé. Aucun n'est reproduit sous instrumentation. Aucune
correspondance module n'est confirmée. Ils sont ici pour ne pas être perdus,
pas pour être considérés comme compris.

## Relevé du 2026-09-13, en session réelle à deux machines

Première fois que ces défauts sont observés **sur un vrai réseau**, entre deux
PC, sur le paquet `1e541cac…acf77a7`. Le lien lui-même était sain pendant tout
le relevé : ping 15–16 ms, `udp_dropped=0`, `udp_error=0`, chaque paquet du pair
livré, `mismatch=0`. Rien de ce qui suit ne vient du réseau.

### G2 est toujours là

**Avalanche!** présente toujours son défaut graphique. Le paquet du jour contient
pourtant le correctif D14 (l'audio ne décide plus de la fin des vidéos) et la
correction du hook de dessin nul (D24) : **ni l'un ni l'autre ne le corrige.**
C'est une information utile — elle élimine deux hypothèses d'un coup.

### G10 — l'écran de choix du mode s'affiche en blanc

Nouveau, rapporté le 2026-09-13. Le diagnostic natif de la session dit
exactement ce que le jeu a fait sur cet écran :

    event=mode_select        online=1 menu_event=0
    event=modesel_loop       iter=0  cursor=0
    event=modesel_loop       iter=36 cursor=1
    event=modesel_loop       iter=56 cursor=2
    event=modesel_movie_wait iter=0   thp_frame=92
    event=modesel_movie_wait iter=240 thp_frame=212
    event=thp_end            ticks=433 logical=216 playback=216 frames=216
                             stopped=0 drained=1 looped=0
    event=modesel_movie_end  thp_frame=215 thp_total=216 wipe_stat=0

Ce que cela **élimine** :

- La logique du menu fonctionne : le curseur a bougé de 0 à 2, donc l'écran
  lisait la manette et réagissait.
- La vidéo a joué **en entier** : 216 images sur 216, position finale 215.
- `drained=1 stopped=0` : le périphérique audio s'est vidé, et la vidéo a
  **quand même** joué jusqu'au bout. C'est le correctif D14 en fonctionnement,
  vérifié ici pour la première fois sur deux machines.

Il reste donc un défaut d'**affichage** : la vidéo se décode et avance
correctement, mais quelque chose ne se voit pas. Deux sous-cas possibles, que
seul un œil devant l'écran peut départager — l'écran est-il blanc **tout le
temps** qu'il est affiché, ou **seulement pendant la petite vidéo** ? Ce n'est
pas la même piste : le premier cas désigne le dessin du menu, le second le
chemin de présentation de la vidéo (`thp_player.cpp:349 draw()` /
`upload_frame()`), et il n'y a aucune raison de chercher dans les deux.

Tant que ce n'est pas tranché, G10 reste **non diagnostiqué**.

### Une correspondance de module, obtenue par corrélation et non par lecture

Au moment où Valentin a signalé *« toujours le bug graphique dans Avalanche! »*,
le diagnostic natif montrait la session dans le contexte **14**, c'est-à-dire
`m406Dll`, que `selmenuDll/main.c:56` nomme **`406:SKI RACE`**. Le chemin
complet de la session était :

    1@115 -> 74@1506 -> 72@2401 -> 3@4317 -> 14@4561 -> 72@6998
         -> 3@7439 -> 15@8029 -> 72@10439 -> 3@10798 -> 16@11013

soit trois mini-jeux enchaînés : `m406Dll` (SKI RACE), `m407Dll`
(BATTANDOMINO), `m408Dll` (SKY DIVE).

La ligne G2 supposait `m412Dll SNOW THROW`. **`m406Dll SKI RACE` est un candidat
nettement meilleur**, et il vient d'une mesure horodatée plutôt que d'une
ressemblance de nom. Ce n'est pas encore une certitude : la corrélation repose
sur l'instant où le message est arrivé, pas sur une observation conjointe. À
confirmer par Valentin, qui peut simplement relancer ce mini-jeu et dire si
c'est bien celui-là.

Cette méthode vaut pour les huit autres : **il suffit de dire le nom du mini-jeu
au moment où on voit le défaut**, et le diagnostic natif donne le module exact.
Tant que cette page portera des « ? », c'est faute d'avoir fait ce rapprochement,
pas faute d'outil.

## G10 — diagnostiqué et corrigé le 2026-09-13

Valentin a précisé : *« Sur le choix du mode de jeux c'est blanc je ne vois
rien »* — **tout l'écran**, pas seulement la vidéo. Cette précision a suffi : la
chaîne se lit d'un bout à l'autre dans le code, sans instrumentation.

1. `bootDll/main.c:384` : `WipeColorSet(255, 255, 255)`, puis
   `WipeCreate(WIPE_MODE_OUT, ...)` ligne 386, **avec attente de la fin**.
   L'écran-titre s'efface **vers le blanc**. L'écran est entièrement blanc.
2. `modeseldll/main.c:91` : `skipFileSelect = skipFileSelect || online;`
   En ligne, l'écran de choix du fichier de sauvegarde est **sauté** — ce
   raccourci existe pour que les deux machines n'aient pas à s'accorder sur un
   fichier.
3. `filesel.c:188` : `WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 30)`.
   **L'écran de sauvegarde est la seule chose qui lève le rideau blanc.** Le
   sauter, c'est sauter le fondu d'ouverture avec lui.
4. `modesel.c:58` : `if (omovlevtno) { WipeCreate(WIPE_MODE_IN, ...); }`
   Et la trace de la session dit `mode_select online=1 menu_event=0` : en ligne
   `omovlevtno` vaut 0, donc ce fondu-là ne s'exécutait pas non plus.

Personne ne levait le rideau. Le menu, lui, tournait : la même trace montre le
curseur passer de 0 à 1 puis 2. **Valentin naviguait dans un menu vivant et
invisible.**

Hors ligne, le défaut est impossible : l'écran de sauvegarde s'affiche et fait
son fondu. Le raccourci « sauter le démarrage » hors ligne ne peut pas le
déclencher non plus, puisqu'il arrive avec `omovlevtno == MODESEL_EVENT_SKIP_BOOT`
(2), ce qui rend le test vrai. **C'est exactement et uniquement le raccourci en
ligne qui perdait le fondu.**

### Le correctif

`modesel.c:58` fait le fondu d'ouverture aussi quand la partie est en ligne. Il
est posé là où le code d'origine l'avait déjà placé — **après** que l'écran se
soit mis en place — et non greffé sur la branche du raccourci, où il aurait
révélé un écran pas encore prêt. La couleur est laissée telle que `bootDll` l'a
posée : ouvrir **depuis le blanc** est aussi ce que fait l'écran de sauvegarde,
qui ne pose pas de couleur non plus.

Déterminisme : les deux pairs exécutent cette ligne au même tick de simulation,
avec la même durée fixe, et le fondu avance par ticks. Rien n'est lu ni de
l'horloge murale ni de l'affichage.

### Ce qui reste à faire

**Le correctif n'est pas démontré.** Il compile ; personne ne l'a encore vu à
l'écran. Il ne sera considéré comme acquis que lorsque Valentin aura relancé une
session en ligne et vu le menu. Tant que ce n'est pas fait, G10 est *corrigé en
attente de vérification*, pas *corrigé*.

## Ce que le mode en ligne impose, et pourquoi ça change ce qu'on voit

Découvert le 2026-09-13 en cherchant l'origine d'un défaut « mal placé /
déformé » sur Avalanche!. **Sept réglages sont forcés dès qu'une partie est en
ligne, quels que soient les choix du joueur.** Certains changent l'image de
façon spectaculaire, et aucun n'est un défaut : ce sont des décisions.

| réglage | hors ligne | en ligne | pourquoi |
|---|---|---|---|
| `internalResolutionScale` | au choix (**12** chez Valentin) | **1** | `portmain.cpp:502` |
| politique de viewport | étirée si non verrouillée | **FIT**, letterbox 4:3 | `portmain.cpp:495` |
| `adaptiveWidescreen` | au choix | **désactivé** | `settings.cpp:178` |
| `targetFrameRate` | au choix (**144**) | **60** | `imgui.cpp:303` |
| `skipBootSequence` | au choix | **désactivé** | « les deux pairs doivent exécuter les mêmes ticks de démarrage » |
| `unlockAllMinigames` | au choix | **activé** | « le même appui sur A ne doit pas lancer un mini-jeu chez l'un et un verrou chez l'autre » |
| `pauseOnFocusLost` | au choix | **désactivé** | une fenêtre au second plan ne doit pas geler la simulation |

### Conséquence à retenir avant de rapporter un défaut graphique

Entre sa partie solo et sa partie en ligne, Valentin passe de **12×** à **1×** de
résolution interne, d'une image étirée à une image letterboxée en 4:3, et de 144
à 60 images par seconde. **L'image en ligne est forcément très différente de
celle dont il a l'habitude**, sans qu'aucun défaut soit en cause.

D'où la règle de tri, à appliquer avant d'inscrire un défaut sur cette page :
**rejouer le même mini-jeu hors ligne.** S'il est correct hors ligne et fautif en
ligne, le coupable est dans ce tableau et non dans le mini-jeu. S'il est fautif
dans les deux, c'est un vrai défaut de rendu.

### Une confirmation gratuite de G10

`skipBootSequence` forcé à *désactivé* en ligne explique le dernier maillon de
G10 : hors ligne, sauter le démarrage fait entrer dans l'écran des modes avec
`omovlevtno == MODESEL_EVENT_SKIP_BOOT`, ce qui déclenchait le fondu. En ligne ce
raccourci est interdit, `omovlevtno` vaut 0, et le fondu ne se déclenchait plus.
Les deux chemins qui pouvaient lever le rideau blanc étaient donc fermés
**exactement et uniquement en ligne** — ce que la trace disait déjà
(`mode_select online=1 menu_event=0`), et que le code confirme ici par un
deuxième chemin indépendant.

### G10 : confirmé corrigé par Valentin, 2026-09-13

*« Ce n'est plus blanc »*. Le menu de choix du mode s'affiche.

C'est le **seul défaut de cette page à être passé de rapporté à corrigé-et-vérifié**,
et la vérification est celle qui compte : un œil humain devant l'écran. Aucun
test automatique de ce dépôt n'aurait pu la produire — ni la détecter au départ.

Rappel de la chaîne, pour mémoire : `bootDll` termine l'écran-titre par un fondu
**vers le blanc** ; en ligne, l'écran de choix du fichier de sauvegarde est sauté,
or c'était la seule chose qui rouvrait le fondu ; et le second fondu possible
était conditionné à un événement d'overlay qui vaut zéro en ligne. Les deux
chemins capables de lever le rideau étaient fermés, exactement et uniquement en
ligne.

## G2 — l'avalanche : défaut confirmé, et indépendant des réglages

2026-09-13. Valentin, en regardant les captures automatiques : *« l'avalanche a
des bugs de texture sur tes photos »*.

Preuves : `docs/preuves/avalanche/avalanche-frame10200-res1x.png` et
`…-res12x.png`.

### Le protocole, et pourquoi il compte

Les deux images sont prises **à la même frame de simulation** (10 200), avec le
même fichier d'entrées, dans le même mini-jeu — seule la **résolution interne**
diffère : 1× (ce que le mode en ligne force) contre 12× (le réglage habituel de
Valentin).

Ce détail n'est pas cosmétique. Mes premières captures étaient déclenchées à
l'horloge, donc deux runs ne montraient jamais le même instant et aucune
comparaison n'était possible. Le pilote déclenche désormais sur le **numéro de
frame** lu dans la trace (`tools/capture_minigame.ps1 -AtFrames`).

### Le résultat

Les deux images montrent, à l'identique :

- la masse d'avalanche sur le bord gauche, rendue en **facettes blanches
  anguleuses à arêtes dures**, au lieu d'une masse de neige ;
- le même sapin au rendu étrange ;
- le même rocher, les mêmes skieurs, le même terrain.

**Le défaut est présent à 12× comme à 1×.** Il n'est donc causé ni par le
bridage de résolution du mode en ligne, ni par un réglage : c'est un vrai défaut
de rendu.

Seule différence visible entre les deux : quelques points colorés près du bord à
1×, qui sont les particules — grossières à basse résolution, fines à haute. Cela
explique en partie les « losanges multicolores » vus plus tôt, mais **pas**
l'avalanche.

### Ce qui reste à identifier

Des facettes blanches à arêtes dures, c'est la signature d'une géométrie dessinée
**sans sa texture**, prenant la couleur du matériau. À confirmer en instrumentant
le chemin de dessin de cet objet plutôt qu'en le supposant : quatre hypothèses
« plausibles » ont déjà été écartées sur ce seul mini-jeu aujourd'hui.

## Relevé externe du 2026-09-16 — premier testeur qui n'est pas Valentin

`GerasSB` a ouvert trois tickets après avoir joué **tous** les Free-for-All et
1v3 en mode mini-jeu, en 4:3 verrouillé et ombres 8x, hors ligne :
[#1](https://github.com/zeranemesis/Marioparty4/issues/1),
[#2](https://github.com/zeranemesis/Marioparty4/issues/2),
[#3](https://github.com/zeranemesis/Marioparty4/issues/3).

C'est la première fois que cette page reçoit des observations d'un œil
extérieur, et deux d'entre elles retombent exactement sur des lignes déjà
inscrites ici. G1 et G2 sont donc **reproduits par un second testeur, sur une
autre machine, sans concertation** :

| ligne existante | ce que GerasSB décrit |
|---|---|
| G1 — feux d'artifice de Slime Time | « Slime Time light/confetti at the end displays big white solid boxes » |
| G2 — Avalanche! | « Avalanche has several visual bugs that make geometry pop in front of the game » |

Le reste de son relevé est nouveau : ombres qui disparaissent dans Stamp Out!,
bords de l'eau de Makin' Waves, gros carré noir autour du joueur en prenant la
banane de Tree Stomp, géométrie parasite d'une frame dans Hop or Pop.

## La colonne « module » n'a plus à rester supposée

Cette page portait des « ? » depuis le début, avec cette justification : *« la
matrice de ce dépôt utilise les noms de développement japonais, qui ne
correspondent pas aux noms localisés »*. C'était vrai de `selmenuDll/main.c`,
qui ne connaît que `402:PURURUN! BIGSLIME`.

Mais la correspondance existe ailleurs, et elle est écrite noir sur blanc :
**`configure.py` commente chaque `Rel(...)` avec le nom localisé**. Elle n'a
jamais eu besoin d'être devinée.

| module | nom localisé | nom de développement |
|---|---|---|
| `m401Dll` | Manta Rings | 401:WAKUGURI DIVING |
| `m402Dll` | Slime Time | 402:PURURUN! BIGSLIME |
| `m404Dll` | Trace Race | 404:CRAYON RUNNER |
| `m406Dll` | Avalanche! | 406:SKI RACE |
| `m412Dll` | Mr. Blizzard's Brigade | 412:SNOW THROW |
| `m415Dll` | Stamp Out! | 415:PYONPYON STAMP |
| `m417Dll` | Makin' Waves | 417:MARIO SURFER |
| `m419Dll` | Tree Stomp | 419:BANANA DE KOROBASE |
| `m421Dll` | Hop or Pop | 421:BODY BALOON |
| `m423Dll` | GOOOOOOOAL!! | 423:GOAL AND GOAL |
| `m425Dll` | The Great Deflate | 425:AIR DOSSUN |
| `m427Dll` | Right Oar Left? | 427:BOAT RACE |
| `m430Dll` | Pair-a-sailing | 430:PARASAILING GO |
| `m441Dll` | Butterfly Blitz | 441:HIRAHIRA CHOUCHO |

### Ce que cela corrige

**G2 était attribué au mauvais module.** La ligne supposait `m412Dll SNOW
THROW` par ressemblance de nom ; or `m412Dll` est **Mr. Blizzard's Brigade**, et
**Avalanche! est `m406Dll`**. La corrélation horodatée du 2026-09-13, qui avait
placé la session dans le contexte 14 = `m406Dll`, avait donc raison contre la
supposition — et c'est maintenant établi par lecture, plus par coïncidence.

Les autres suppositions se vérifient : G1 `m402Dll`, G3 `m427Dll`, G4
`m430Dll`, G5 `m404Dll`, G7 `m441Dll`. Les nouvelles lignes de GerasSB
s'attribuent directement : Stamp Out! `m415Dll`, Makin' Waves `m417Dll`,
Tree Stomp `m419Dll`, Hop or Pop `m421Dll`, Manta Rings `m401Dll`.

## Le défaut qui n'est pas un défaut de mini-jeu

GerasSB ouvre son ticket par une observation qui vaut pour **tout le jeu** :

> *The game does not seem to pre-compile any shaders, so nearly every new scene
> has missing textures and geometry for a few seconds when started up for the
> first time.*

Celui-là se lit entièrement dans le code, sans instrumentation et sans
reproduire quoi que ce soit.

1. `lib/gfx/pipeline_cache.cpp` — `find_pipeline_impl()` ne construit un
   pipeline immédiatement que si aucun fil de compilation n'existe et que le
   quota `BuildPipelinesPerFrame` de la frame n'est pas épuisé. Sinon il place
   la demande dans `g_priorityPipelines` / `g_backgroundPipelines` et rend la
   main aussitôt.
2. `lib/gfx/pipeline_cache.cpp:1090` — `get_pipeline()` échoue tant que le
   pipeline n'est pas dans `g_pipelines`.
3. `lib/gfx/common.cpp:1447` — `bind_pipeline()` propage cet échec.
4. `lib/gx/pipeline.cpp:18` — `render()` fait alors `return;`.

**Un pipeline pas encore compilé ne retarde donc pas le dessin : il le
supprime.** La géométrie concernée n'est pas affichée du tout, jusqu'à ce que
le fil de compilation rattrape son retard. C'est exactement « missing textures
and geometry for a few seconds », et c'est pire dans une scène neuve, où tous
les pipelines sont neufs en même temps — d'où Manta Rings, cité comme le cas le
plus visible.

### Le remède est déjà écrit, et n'est jamais livré

Les deux moitiés du mécanisme existent :

- `src/port/portmain.cpp:323` — `EnsureInitialPipelineCache()`, appelée depuis
  `portmain.cpp:487`, copie `initial_pipeline_cache.db` depuis le dossier de
  l'exécutable vers `pipeline_cache.db` du dossier de configuration, au premier
  lancement seulement.
- `lib/gfx/pipeline_cache.cpp:522` — `seed_pipeline_cache()` fusionne une base
  fournie dans le cache local.

Il manque la base elle-même. **Aucun `initial_pipeline_cache.db` n'est présent
dans les paquets distribués** — vérifié sur `PartyBoard-win-x64.zip` (212
entrées) et `partyboard_alpha_0.2.0_x64.zip` (99 entrées) — et **rien dans
`CMakeLists.txt`, `ci/`, `dist/` ni `tools/` ne la produit ni ne la copie**. Le
seul effet observable aujourd'hui est la ligne d'erreur
« No bundled initial pipeline cache found at '…' » au premier lancement.

Chaque joueur part donc d'un cache vide et paie la compilation de chaque
pipeline la première fois qu'il voit chaque scène. Le deuxième passage est
propre — ce que GerasSB décrit aussi (« when started up for the first time »),
et ce qui distingue ce défaut des huit autres de cette page, qui eux
**persistent aux relectures**.

Produire cette base est un travail de build, pas de rendu : il faut parcourir
une fois les scènes, récupérer le `pipeline_cache.db` engendré, et le livrer
sous le nom attendu à côté de `partyboard.exe`. Ce n'est pas fait, et rien dans
le plan ne le prévoit.

## Statut, mis à jour

G10 reste le seul corrigé-et-vérifié. G1 et G2 sont désormais **confirmés par
deux testeurs indépendants**. La colonne module n'est plus une supposition. Le
défaut de compilation de pipelines est **diagnostiqué de bout en bout et non
corrigé**, et il est le seul de cette page dont la cause soit établie sans
avoir eu besoin de le reproduire.

## La copie de framebuffer — une famille, et un défaut prouvé dedans

Quatre des six défauts de mini-jeu signalés par GerasSB tombent dans des modules
qui font une **copie de l'EFB re-liée en texture** (`GXCopyTex`) : Stamp Out!
(`m415Dll`), Makin' Waves (`m417Dll/water.c:885`), Tree Stomp
(`m419Dll/main.c:246`), Hop or Pop (`m421Dll/player.c:1740`). Deux lignes de
cette page s'y ajoutent : Right Oar Left? (`m427Dll`, qui porte déjà un
`// TODO PC why do we need to skip the clear?`) et Pair-a-sailing (`m430Dll`).

Vingt modules sur soixante et un utilisent `GXCopyTex` : six défauts sur dix
dans un tiers des modules, c'est une **piste**, pas une démonstration. Ce qui
suit en est une.

### Ce que `GXCopyTex` fait réellement sur PC

`extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp:150` — le paramètre `dest`
n'est **qu'une clé de cache** (`CopyTextureKey{.dest = dest, …}`). Aurora
résout l'EFB dans une texture GPU et **n'écrit jamais un octet dans `dest`**.

Conséquence directe : tout code de jeu qui **relit ces octets côté CPU** lit de
la mémoire non initialisée sur PC. Le port le sait — `m415Dll/main.c:1585` le
documente et contourne le problème en faisant pointer le bitmap du canevas sur
le même `Hu3DShadowData.buf`, pour que l'identité du pointeur retrouve la
texture résolue :

```c
// Hu3DShadowData was copied by GXCopyTex and Aurora doesn't actually copy it there
// it just holds a reference to the pointer
// TODO PC does this fix cause issues?
temp_r31->data = Hu3DShadowData.buf;
```

Mais **`m415Dll/main.c:433`, mille lignes plus haut, fait toujours la relecture
brute**, sans `#ifdef TARGET_PC` :

```c
memcpy((*temp_r3)->bmp->data, OSCachedToUncached(Hu3DShadowData.buf), temp_r29);
```

Deux relectures sœurs dans le même fichier, une corrigée pour PC et l'autre
non. Elle s'exécute dans `fn_1_1960` **case 1**, juste avant le `case 2` qui
bascule la carte d'ombre sur le canevas — c'est-à-dire exactement au moment que
GerasSB décrit, *« before the game begins »*. Le mécanisme est prouvé ; le fait
qu'il produise précisément la disparition des ombres ne l'est pas.

### Tree Stomp : défaut prouvé de bout en bout, dans Aurora

Tree Stomp est le seul des modules cités à copier la **profondeur** :

```c
GXSetTexCopySrc(sp8.x, sp8.y, 192, 192);
GXSetTexCopyDst(96, 96, GX_TF_Z24X8, 1);
GXCopyTex(lbl_1_bss_64[lbl_1_bss_60], 0);   // m419Dll/main.c:249
```

Le chemin se lit sans ambiguïté :

1. `tex_copy_conv.cpp:270` — `DepthConvPipelines` ne contient **qu'une seule
   entrée, `GX_TF_Z16`**. Il n'existe aucun pipeline pour `GX_TF_Z24X8`, donc
   `needs_conversion(GX_TF_Z24X8)` est faux.
2. `gx.hpp:431` — `is_depth_format(GX_TF_Z24X8)` est **vrai**.
3. `common.cpp:1332` — pas de conversion, mais 192→96 impose une mise à
   l'échelle, donc l'appel part dans `tex_copy_conv::blit()`.
4. `tex_copy_conv.cpp:508` — `blit()` exécute **`g_blitPipeline`**, construit
   ligne 393 avec `g_bindGroupLayout` (sampler @0, texture @1, uniforme @2).
5. `tex_copy_conv.cpp:441` — mais `execute()` choisit son bind group sur le seul
   critère `is_depth_format(req.fmt)`, et fabrique donc un groupe au layout
   **`g_depthBindGroupLayout`** (texture @0, uniforme @1, pas de sampler).

**Le bind group et le pipeline n'ont pas le même layout** — ni le même nombre
d'entrées. WebGPU rejette le `SetBindGroup`, la passe est invalidée, et la copie
ne produit rien. La texture que l'effet échantillonne ensuite reste vide, donc
noire : *« Grabbing the Tree Stomp speedup banana causes major visual glitch,
huge black box around the player »*.

Le défaut vaut pour **tous les formats de profondeur sauf `GX_TF_Z16`** — le
seul qui dispose d'un pipeline de conversion, et donc le seul qui n'emprunte
jamais `blit()`. `Z16` marche par accident de couverture, pas par conception.

Deux corrections possibles, toutes deux dans `tex_copy_conv.cpp` :

- créer un `g_depthBlitPipeline` avec `g_depthBindGroupLayout` et
  `DepthShaderPreamble`, et le sélectionner dans `blit()` — corrige la famille
  entière ;
- ou ajouter une entrée `GX_TF_Z24X8` à `DepthConvPipelines`, ce qui rend
  `needs_conversion` vrai et fait passer par `run()` avec le bon pipeline —
  corrige `Z24X8` seul, mais c'est la conversion que ce format réclame de toute
  façon.

**Ces deux fichiers sont dans le sous-module `extern/aurora`, qui pointe sur
`encounter/aurora` en amont.** Le correctif ne peut donc pas être porté par une
PR de ce dépôt seul : il faut une PR amont, ou un fork, puis un relèvement du
sous-module.

### Ce que cela ne dit pas

Slime Time (`m402Dll`), Avalanche! (`m406Dll`), Trace Race (`m404Dll`) et
Butterfly Blitz (`m441Dll`) **n'appellent pas `GXCopyTex`**. Leurs défauts ont
une autre cause, et le raisonnement ci-dessus ne s'y applique pas.

## Un défaut prédit, dans un mini-jeu que personne n'a encore testé

En cherchant l'origine de G1 (Slime Time) du côté du *reflection mapping*, une
autre chose est tombée — sans rapport avec G1, mais réelle.

`hsfman.c:1983`, `Hu3DReflectMapSet()`, l'API qui installe une carte de
réflexion, est écrite ainsi :

```c
void Hu3DReflectMapSet(ANIMDATA* arg0) {
#ifndef BYTESWAPPING
    ...  reflectAnim[0] = HuSprAnimRead(arg0);  ...
#else
    assert(0 == 1);
    OSReport("PC TODO: Hu3DReflectMapSet ran which tries to reallocate an anim
");
#endif
    reflectMapNo = 0;
}
```

**`BYTESWAPPING` est défini par toutes les cibles qui compilent ce fichier** —
`CMakeLists.txt:207` (`dol`) et `:339` (les DLL de REL). Seul `partyboard`
(ligne 279), qui ne compile pas le code du jeu, ne le définit pas. La branche
utile n'existe donc dans aucun binaire livré : sur PC la fonction se réduit à

```c
assert(0 == 1);
reflectMapNo = 0;
```

La carte demandée, `arg0`, est **purement ignorée**. En build `Release` /
`RelWithDebInfo` (où `NDEBUG` supprime l'`assert`) la fonction échoue en
silence ; en `Debug` elle **avorte le processus**.

Un seul appelant : `m444dll/main.c:1262` — **Reversal of Fortune** :

```c
Hu3DReflectMapSet(HuDataSelHeapReadNum(DATA_MAKE_NUM(DATADIR_M444, 0x23),
                                       MEMORY_DEFAULT_NUM, HEAP_DATA));
```

Deux conséquences, toutes deux non observées à ce jour parce que **personne n'a
rapporté avoir joué ce mini-jeu** : la réflexion propre à Reversal of Fortune
n'est jamais installée (la scène garde la carte 0 chargée au démarrage), et
l'`ANIMDATA` lue dans `HEAP_DATA` juste avant n'est jamais relâchée — **une
fuite à chaque appel**.

C'est la première ligne de cette page à être **prédite avant d'être vue**.
Elle se vérifie en une partie : lancer Reversal of Fortune et regarder.

### Ce que cela règle au passage, et ce que cela ne règle pas

Le même motif existe dans `Hu3DAllKill()` (`hsfman.c:375-386`), avec le
commentaire *« the game expects this to be executed »* — et il est lui aussi
compilé hors du binaire. **Ce n'en est pas un défaut** : ce rechargement
n'existait que pour défaire `Hu3DReflectMapSet()`, qui est inerte sur PC.
`reflectAnim[0]` garde donc sur PC la valeur posée à l'initialisation, ce qui
est cohérent. Les deux blocs se neutralisent l'un l'autre ; la question est
close, il n'y a pas de piste de ce côté.

**G1 et G2 restent sans mécanisme.** Slime Time (`m402Dll`) et Hop or Pop
(`m421Dll`) sont, avec `m438Dll`, les trois seuls modules à appeler
`Hu3DModelReflectTypeSet()`, et deux des trois sont dans la liste de GerasSB —
mais aucun n'appelle `Hu3DReflectMapSet()`, donc cette corrélation **n'a aucun
mécanisme derrière elle** à ce stade. Elle est notée ici pour ne pas être
recherchée deux fois, pas comme une piste établie.

## Correction — Tree Stomp : la copie n'était que la moitié du problème

Écrit plus haut : *« la copie ne produit rien […] donc noire »*. C'est exact mais
**insuffisant**, et présenté comme une cause complète, ce qui était une erreur.
En lisant l'effet en entier, le coupable dominant est ailleurs.

### Ce que l'effet fait réellement

`m419Dll` est une **traînée de mouvement**. Il garde un anneau de huit couples de
textures — une copie couleur (`lbl_1_bss_84[]`, RGB5A3) et une copie de
profondeur (`lbl_1_bss_64[]`, Z24X8) — et redessine les sept dernières frames
par-dessus la scène. Chaque fantôme est **un quad plein écran** :

```c
sp2C = {0,0,0};  sp20 = {640,0,0};  sp14 = {640,480,0};  sp8 = {0,480,0};
```

Ce qui empêche ce quad de recouvrir tout l'écran, c'est uniquement la ligne :

```c
GXSetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);   // m419Dll/main.c:279
```

La profondeur du fragment est **remplacée** par le texel de TEXMAP1, c'est-à-dire
la profondeur capturée au moment de la frame fantôme ; combinée à
`GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE)`, elle confine chaque fantôme à la
silhouette qu'avait la géométrie. La traînée *est* ce test de profondeur.

### Le vrai coupable

`extern/aurora/lib/dolphin/gx/GXTev.cpp:171` :

```cpp
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
  // TODO
}
```

**Un stub vide.** Et `ztex` n'apparaît nulle part dans le générateur de shaders
(`lib/gx/shader.cpp`, `shader_info.cpp`) : le Z-texturing n'existe pas dans
Aurora, ni comme état, ni comme écriture de `frag_depth`.

Sans lui, les sept quads ne sont plus confinés à rien et couvrent 640×480
chacun, empilés, devant la scène. **C'est la grosse boîte.** La copie de
profondeur défaillante y contribue, mais même une copie parfaite ne changerait
rien : sans `GX_ZT_REPLACE`, la texture de profondeur n'est lue par personne.

### Ce qui a quand même été corrigé, et pourquoi

`GX_TF_Z24X8` a été ajouté à `DepthConvPipelines` (`tex_copy_conv.cpp`). Ce
n'est pas suffisant pour Tree Stomp, mais ce n'est pas cosmétique non plus :
sans cette entrée, chaque frame de l'effet soumettait un `SetBindGroup` au
layout incompatible, ce qui **invalide la passe de rendu entière** — et donc
potentiellement des dessins qui n'ont rien à voir avec cet effet. Supprimer une
passe invalide par frame vaut d'être fait, et c'est de toute façon un
prérequis à toute implémentation future du Z-texturing.

`GX_TF_Z24X8` est le **seul** format de copie de profondeur employé par le jeu
entier, et uniquement ici (`m419Dll/main.c:248`, `:279`, `:305`). Le décalage de
layout de `blit()` subsiste pour les autres formats de profondeur, mais il est
désormais **inatteignable dans ce jeu**.

### Ce qui n'a pas été tenté

Implémenter `GXSetZTexture` demande d'ajouter au générateur de shaders une
écriture de `@builtin(frag_depth)` depuis un texel, et l'état de pipeline qui va
avec. C'est une fonctionnalité, pas un correctif ; elle est dans un sous-module
amont ; et rien ici ne peut la compiler. L'écrire à l'aveugle serait pire que de
ne rien faire. **Tree Stomp reste non corrigé**, et sa cause est maintenant
nommée.

## Ce qui a été corrigé, et ce qui ne l'est pas

Aucun de ces changements n'a été compilé : la machine où ils ont été écrits n'a
pas de compilateur C/C++. À lire comme des propositions étayées, pas comme des
correctifs validés.

| | fichier | état |
|---|---|---|
| Reversal of Fortune : carte de réflexion ignorée + fuite | `src/game/hsfman.c` | **corrigé** |
| Copie de profondeur `Z24X8` : passe invalidée chaque frame | `extern/aurora/lib/gfx/tex_copy_conv.cpp` | **corrigé**, dans le sous-module |
| Cache de pipelines jamais livré | `CMakeLists.txt`, `tools/capture_pipeline_cache.ps1` | **plomberie posée**, la base reste à enregistrer |
| Tree Stomp : `GXSetZTexture` non implémenté | — | **non corrigé**, cause nommée |
| Stamp Out! : relecture CPU non corrigée | `src/REL/m415Dll/main.c:433` | **non corrigé**, délibérément |
| Slime Time, Avalanche! | — | **sans mécanisme** |

### Reversal of Fortune

`Hu3DReflectMapSet()` installe maintenant la carte demandée. La raison pour
laquelle l'original ne le pouvait pas est que `HuMemDirectFree(reflectAnim[0])`
ne libère que l'`ANIMDATA` en laissant fuir les tableaux `bank`/`pat`/`bmp`
alloués à côté sur un build `BYTESWAPPING`. `HuSprAnimKill()` les libère tous et
respecte `useNum` : c'est le bon destructeur, il existait déjà.

`Hu3DAllKill()` restaure la carte de démarrage au lieu de relire `refMapData0` —
une relecture construirait une seconde `ANIMDATA` à partir de la même source et
perdrait la première. Un pointeur capturé à l'initialisation suffit. Les chemins
`__MWERKS__` et non-`BYTESWAPPING` ne sont pas touchés, pour ne pas casser les
builds *matching*.

### Le cache de pipelines

La base ne peut pas être fabriquée par le build : c'est un **enregistrement**,
produit en jouant. Ce qui manquait n'était donc pas seulement le fichier mais le
chemin pour le fabriquer et le livrer. Les deux existent maintenant :
`tools/capture_pipeline_cache.ps1` prélève le `pipeline_cache.db` du dossier de
configuration, et `CMakeLists.txt` l'installe `OPTIONAL` à côté de
l'exécutable — un arbre sans base compile toujours.

Le script refuse de travailler si un `-wal` traîne (le jeu est encore ouvert, ou
s'est mal fermé, et la copie manquerait ses lignes les plus récentes) et refuse
d'écraser une base par une plus petite sans `-AllowShrink`. Reste à faire, et
cela demande quelqu'un devant le jeu : parcourir les scènes, capturer, et
**commiter le fichier** — sinon la CI continuera de livrer des paquets sans
graine. À refaire à chaque changement de schéma, qu'Aurora rejette en clair
(*« does not use schema version »*).

### Stamp Out! : pourquoi rien n'a été touché

Le correctif évident serait de refléter `fn_1_66AC` — remplacer le `memcpy` de
`fn_1_1960` case 1 par l'aliasing du pointeur. Il n'a pas été appliqué : le lien
entre cette relecture et la disparition des ombres **n'est pas démontré**, et
l'aliasing transfère la propriété d'un tampon (qui le libère ?) dans un fichier
que rien ici ne peut compiler ni exécuter. Un correctif spéculatif, non testé,
sur un symptôme non reproduit, vaut moins qu'une ligne dans ce registre.

## G2 (Avalanche!) — une cinquième hypothèse écartée

L'avalanche est un **maillage procédural**, pas un modèle : `m406Dll/map.c:1104`
dessine trente bandes de trente-cinq sommets, sans texture (`GX_TEXMAP_NULL`,
`GX_REPLACE`), éclairées avec spéculaire, en re-pointant les tableaux de sommets
entre chaque appel d'un **même** display list :

```c
GXCallDisplayList(var_r31->unk_A4, var_r31->unk_A0);
for (var_r30 = 1; var_r30 < 29; var_r30++) {
    var_r29 = var_r30 * 35;
    GXSETARRAY(GX_VA_POS, &var_r31->unk_84[var_r29], ...);
    GXSETARRAY(GX_VA_NRM, &var_r31->unk_88[var_r29], ...);
    GXSETARRAY(GX_VA_CLR0, &var_r31->unk_90[var_r29], ...);
    GXCallDisplayList(var_r31->unk_A4, var_r31->unk_A0);
}
```

Deux choses en découlent, et la première est un **avertissement** : *« facettes
blanches à arêtes dures, prenant la couleur du matériau »* ne peut pas être la
signature d'une texture manquante ici — cette géométrie n'a **jamais** de
texture, par conception. `GXSetChanMatColor(GX_COLOR0A0, lbl_1_data_88F)` pose
sa couleur, et l'aspect vient entièrement de l'éclairage et des couleurs par
sommet. L'hypothèse inscrite plus haut sur cette page (« géométrie dessinée sans
sa texture ») est donc **fausse pour G2**.

La seconde était prometteuse : `GXCallDisplayList` **ne draine pas** la FIFO —
sa variante `GXCallDisplayListLE` le fait explicitement, en disant pourquoi
(*« so that any pending CP register writes (VCD, VAT, etc.) are processed into
g_gxState before the display list's draw commands reference them »*). Si les
`GXSETARRAY` écrivaient directement `g_gxState` pendant que les trente display
lists s'empilaient dans la FIFO, les trente bandes seraient dessinées avec le
**dernier** pointeur : la même bande répétée trente fois, à arêtes dures, au
lieu d'une masse continue. Cela décrivait exactement la capture.

**Écartée.** `GXSetArray` (`lib/dolphin/gx/GXGeometry.cpp:218`) écrit dans la
FIFO — `GX_WRITE_AURORA(GX_LOAD_AURORA_ARRAYBASE | cpIdx)` puis le pointeur, la
taille et le stride — et non dans `g_gxState`. L'ordre entre les liaisons de
tableaux et les display lists est donc préservé, et chaque bande est dessinée
avec la sienne.

Cinq hypothèses écartées sur ce seul mini-jeu. G2 reste **non diagnostiqué**, et
la piste « texture manquante » qui l'accompagnait depuis le début est à
abandonner.

## Le cache de pipelines : deux dossiers, et une graine qui atterrissait dans le mauvais

Vérifié en lançant le jeu le 2026-09-16, log à l'appui. Le démarrage réclame la
graine **deux fois**, une par moitié du mécanisme :

```
[error] [partyboard::main] No bundled initial pipeline cache found at
        'C:\…\partyboard\initial_pipeline_cache.db'
[INFO | aurora::gfx::pipeline_cache] No bundled initial pipeline cache found at
        'C:\…\partyboard\initial_pipeline_cache.db'
```

Confirmation directe de ce qui n'était jusque-là qu'une lecture de code. Mais
l'inspection des dossiers a montré autre chose :

| | appel | dossier réel |
|---|---|---|
| cache vivant (Aurora) | `SDL_GetPrefPath(nullptr, "Party Board")` | `%APPDATA%\Party Board\` |
| config du port | `SDL_GetPrefPath("MarioPartyRD", "Party Board")` | `%APPDATA%\MarioPartyRD\Party Board\` |

`pipeline_cache.db` (1,8 Mo) et `dawn_cache.db` sont dans le **premier**.
`config.json` et la carte mémoire sont dans le **second**.

Or `EnsureInitialPipelineCache()` (`portmain.cpp:323`) copie la graine vers
`PartyBoard_ConfigPath / "pipeline_cache.db"`, c'est-à-dire le **second**. Aurora
ne lit jamais là. **Cette fonction est donc inerte**, indépendamment du fait que
la graine n'existe pas : même livrée, sa copie atterrirait où rien ne regarde.

Ce n'est pas bloquant, parce que la moitié qui compte marche : Aurora lit
`initial_pipeline_cache.db` **à côté de l'exécutable** (`g_config.resourcesPath`)
et le fusionne elle-même dans son cache (`seed_pipeline_cache()`). Livrer le
fichier près de `partyboard.exe` suffit donc, et c'est ce que fait la règle
`install()` ajoutée à `CMakeLists.txt`.

`tools/capture_pipeline_cache.ps1` visait initialement le mauvais dossier, pour
la même raison. Corrigé.

### Ce que la machine de test ajoute au tableau

GPU **Intel(R) Graphics (integré)**, D3D12, 1280×960. Sur un GPU intégré la
compilation de pipelines est nettement plus lente que sur une carte dédiée, ce
qui rend l'absence de graine d'autant plus visible — et explique qu'un défaut
décrit comme « quelques secondes » puisse durer plus longtemps ici.

## G2 — Avalanche! : diagnostiqué et corrigé le 2026-09-16

Ouvert depuis le 2026-09-12, confirmé par deux testeurs, **cinq hypothèses
écartées**. Résolu en trois quarts d'heure le jour où quelqu'un a regardé
l'image. C'est la leçon de cette page, et `tools/capture_fenetre.ps1` la disait
déjà en tête de fichier.

### Ce que l'image a donné, et que le code n'avait pas donné

Valentin a précisé : **la masse de neige, dès la première image**. Cela élimine
d'un coup le cache de pipelines (qui se corrige tout seul) et toute piste de
texture (cette géométrie n'en a pas). Une capture agrandie de la coulée montre
alors des **rubans parallèles réguliers à arêtes franches**, plus un grand
triangle blanc étiré — signature d'indices de sommets hors de leur fenêtre, pas
d'un défaut d'éclairage.

### La preuve

`map.c:941` construit le display list de la coulée :

```c
GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 70);
for (var_r29 = 0; var_r29 < 35; var_r29++) {
    GXPosition1x16(var_r29 + 35);   /* indices 35..69 */
    GXNormal1x16(var_r29 + 35);
    GXColor1x16(var_r29 + 35);
    GXPosition1x16(var_r29);        /* indices 0..34  */
    ...
}
```

**70 sommets, indices 0 à 69.** Chaque appel dessine la bande *entre* deux
rangées de 35. C'est cohérent avec tout le reste : 1050 sommets = 30 rangées, et
la boucle de dessin fait 29 appels — un par intervalle.

Or la boucle rebase les tableaux par fenêtres de **35** :

```c
GXSETARRAY(GX_VA_POS, &var_r31->unk_84[var_r29], 35 * sizeof(Vec), sizeof(Vec), TRUE);
```

Les indices 35 à 69 — **la rangée supérieure de chaque bande** — tombent hors de
la fenêtre déclarée.

### Pourquoi ça ne se voyait que sur PC

`GXSetArray` du vrai GX ne prend **pas de taille** : base et pas, rien d'autre.
Le matériel lit `base + index × stride` sans borne, donc sortir de 35 ne
signifie rien pour lui. La macro le dit :

```c
#define GXSETARRAY(attr, data, size, stride, le) GXSetArray((attr), (data), (size), (stride), (le))  /* Aurora */
#define GXSETARRAY(attr, data, size, stride, le) GXSetArray((attr), (data), (stride))                /* GameCube */
```

Le paramètre `size` est une **invention du port**, et Aurora s'en sert pour de
bon : `push_storage(array.data, array.size)`
(`command_processor.cpp:1639`) téléverse exactement ces octets. Le décompilateur
a dû inventer une taille à chacun des ~159 sites d'appel, et ici il a écrit la
hauteur d'une rangée au lieu de deux.

Détail qui confirme : le **premier** appel, hors boucle, passe le tableau entier
(`unk_80 * sizeof(Vec)`). Une seule bande était donc correcte, les vingt-huit
autres tronquées — ce que l'image montre, un bord lisse et le reste en rubans.

### Le correctif

Chaque fenêtre reçoit le reste du tableau, `unk_80 - var_r29`, ce que le
matériel autorise de fait. À la dernière itération cela vaut exactement 70, soit
le strict nécessaire. **Aucun risque pour les builds *matching* : la macro
GameCube ignore l'argument.**

### Ce que cela ouvre

Le motif est systémique, pas local. Une taille sous-estimée ne produit ni
erreur ni avertissement : elle tronque la géométrie en silence. D'autres sites
déclarent une fenêtre d'**un seul élément** — `m421Dll/player.c:1809` (Hop or
Pop), `m423Dll/main.c:5367` (GOOOOOOOAL!!), `m425Dll/thwomp.c:2135` (The Great
Deflate), `m428Dll/player.c:2194`. C'est **légitime** si le display list n'y
indexe que 0, et ces trois-là sont précisément des mini-jeux signalés. Rien ne
prouve qu'ils soient fautifs ; il suffit de lire leur display list comme on
vient de le faire ici.

### Le même défaut ailleurs : deux autres cas, deux faux positifs

Le motif de G2 n'était pas isolé. Quatre autres sites déclaraient une fenêtre
`CLR0` d'**un seul élément** ; les lire un par un les départage sans ambiguïté —
il suffit de retrouver le display list qui les consomme et de regarder ses
indices.

| module | mini-jeu | indices `CLR0` du display list | verdict |
|---|---|---|---|
| `m421Dll/player.c:1809,1825` | **Hop or Pop** | `GXColor1x8(1)` autant que 0 | **fautif** |
| `m423Dll/main.c:5367` | **GOOOOOOOAL!!** | `GXColor1x16(i)`, i < unk26 | **fautif** |
| `m425Dll/thwomp.c:2135` | The Great Deflate | `GXColor1x16(0)` seul | correct |
| `m428Dll/player.c:2194` | Cliffhangers | `GXColor1x16(0)` seul | correct |

**Hop or Pop.** L'éventail est un dégradé radial : `unk_40[0]` a un alpha de
0x40, `unk_40[1]` un alpha de 0. La fenêtre d'un élément laissait la couleur de
bord hors du téléversement — le bord ne s'efface donc jamais. GerasSB décrit
*« random geometry appears in front of the screen for a frame »*, ce qui est
compatible, sans que cela le prouve.

**GOOOOOOOAL!!** Le display list construit autour de `main.c:5118` émet une
couleur par quad, `GXColor1x16(i)` pour les `unk26` quads. La fenêtre de
position juste au-dessus compte bien `unk26 * 4` sommets ; celle des couleurs en
comptait une. Seul le premier quad recevait la sienne.

Aucun des deux ne peut planter : WebGPU borne les lectures hors d'un buffer de
stockage. Ce sont des défauts d'image, et cela **n'explique pas** le crash de
l'issue #3 sur ce même mini-jeu.

Les deux correctifs suivent la règle de G2 : donner à la fenêtre ce que le
display list indexe réellement. Et comme la macro GameCube jette l'argument,
aucun n'a d'effet sur un build *matching*.

## G6 — Bowser's Bigger Blast : module identifié, hypothèse du registre invalidée

Valentin, 2026-09-16, sur la build fraîche : *« dans le jeu Bowser's Bigger
Blast l'explosion est accélérée »*. La ligne G6 portait « module non identifié »
depuis le 2026-09-12 ; c'est **`m440Dll`**.

### L'hypothèse inscrite ici était fausse

Cette page disait de G6 : *« l'accélération de l'explosion est la signature
d'une animation pilotée par le nombre d'images affichées plutôt que par les
ticks de simulation — exactement le mécanisme de D14 et de D6 »*.

Le détecteur de D6 dit l'inverse, dans son propre commentaire
(`src/game/main.c:301`) :

> *a frame that batches two simulation ticks advances the animation clock
> **once, for both**. Below 61 frames per second frame_pacer_simulation_tick
> always returns 1 and this can never fire.*

D6 fait donc **perdre** des pas d'animation, pas en gagner : il **ralentit**, et
uniquement au-dessus de 60 images par seconde. La machine de test tourne à 60
(`video.targetFrameRate: 60`, surimpression FPS à 60). **D6 est éliminé pour
G6**, dans les deux sens : mauvaise direction, et hors de sa plage.

### Ce qui est établi

`m440Dll/main.c:795`, dans l'état 3 de la séquence :

```c
Hu3DModelAttrReset(object->model[3], HU3D_MOTATTR_PAUSE);
Hu3DMotionSpeedSet(object->model[3], 2.0f);
```

L'explosion est jouée à **vitesse 2× par le jeu d'origine**. Elle est donc
rapide par conception, et la question n'est pas « pourquoi est-elle rapide »
mais « pourquoi est-elle **plus** rapide qu'elle ne devrait ».

Les deux hooks de dessin du module (`fn_1_806C`, `fn_1_9C04`) n'avancent aucun
état — ils ne font que dessiner. Le mécanisme du correctif de `m417Dll`
(`if (HuSysVWaitGet(0) == 0) return;`, qui empêche un hook de faire avancer la
simulation à la cadence d'affichage) **ne s'applique pas ici** : il n'y a rien à
garder.

### Ce qu'il reste à trancher, et qui ne se lit pas dans le code

G6 a deux moitiés : l'explosion accélérée **et** *« la fin est buggée avec le
jeu qui continue malgré être le gagnant »*. Si les deux tiennent encore, il faut
savoir si c'est **tout le mini-jeu** qui tourne trop vite ou **seulement**
l'explosion. Le premier cas désigne l'horloge de simulation du module ; le
second, cette animation-là. Aucune lecture de code ne le départage, et se
tromper de moitié coûte une journée — c'est exactement ce qui vient d'arriver
avec D6.

**Statut : module identifié, mécanisme inconnu, hypothèse antérieure écartée.**

## G2 — corrigé et VÉRIFIÉ, 2026-09-16

*« Avalanche est parfait ! »* — Valentin, sur une build compilée à l'instant
contenant le correctif.

C'est le **deuxième défaut de cette page à passer de rapporté à
corrigé-et-vérifié**, après G10, et la vérification est celle qui compte : un
œil humain devant l'écran. G2 était ouvert depuis le 2026-09-12, confirmé par
deux testeurs sur deux machines, et avait résisté à **cinq hypothèses**. Il a
cédé le jour où quelqu'un a regardé une capture agrandie.

La méthode, pour mémoire : décrire précisément *quoi* (la masse de neige) et
*quand* (dès la première image) ; capturer ; agrandir ; lire le display list.
Trois quarts d'heure. Les cinq hypothèses précédentes avaient coûté plusieurs
sessions de lecture de code.

Également vérifié dans la même session : **la fin buggée de G6 n'est plus là**.
Bowser's Bigger Blast se termine normalement. Seule l'accélération de
l'explosion subsiste.

## G7 — Butterfly Blitz : ce n'est pas « les papillons »

Relevé initial : *« les papillons n'ont pas d'ombre »*. La capture montre autre
chose, et c'est beaucoup plus net : **aucun objet de la scène n'a d'ombre** —
ni les papillons, ni Mario, ni Luigi, ni Yoshi, ni Peach. Le sol carrelé est
uniformément non ombré.

Ce n'est donc pas un défaut d'un modèle particulier : **toute la passe d'ombre
est éteinte** dans ce mini-jeu.

### Ce que cela élimine

`m441Dll` fait exactement les mêmes appels que `m406Dll`, qui lui **a** des
ombres (l'ombre du sapin est visible sur `aval-05.png`, quoique à arêtes
franches) :

| | `m406Dll` (ombres OK) | `m441Dll` (aucune ombre) |
|---|---|---|
| `Hu3DShadowCreate` | `(45.0f, 1000.0f, 250000.0f)` | `(30, 20, 20000)` |
| `Hu3DShadowTPLvlSet` | oui | oui |
| `Hu3DShadowPosSet` | oui | oui |
| `Hu3DModelShadowMapSet` | oui | oui |
| `Hu3DModelShadowSet` | — | 3 sites |

Aucun appel ne manque. Le dessin est conditionné à
`Hu3DShadowF != 0 && Hu3DShadowCamBit != 0` (`hsfdraw.c`, cinq sites) : **l'un
des deux vaut zéro**, et lequel ne se déduit pas du code.

Hypothèses écartées en chemin : le filtrage par layer (`Hu3DShadowExec` itère
tous les modèles sans regarder le layer) ; l'asymétrie du compteur
(`Hu3DModelShadowReset` décrémente inconditionnellement là où `...Set`
incrémente sous condition) — réelle, mais `m441Dll` n'appelle jamais `Reset`,
et dans `m415Dll` les `Reset` sont **appariés** à des `Set`. `hsfman.c` étant un
objet `Matching`, cette asymétrie est de toute façon du code d'origine, à ne pas
toucher.

### Ce qu'il faut maintenant

Une sonde de deux lignes qui dit lequel des deux drapeaux est nul. C'est
désormais possible : cette machine compile depuis aujourd'hui.

## Références console, 2026-09-17 — la page cesse d'être aveugle

Cette page s'ouvre sur : *« une classe entière de défauts échappe à tout ce que
ce dépôt a construit pour se valider »*, et `tools/capture_fenetre.ps1` ajoute :
*« every rendering defect was reported by a human describing what he saw, and
answered by someone reading code and guessing »*.

Il manquait la moitié de la comparaison : **à quoi cela ressemble sur la
console**. Valentin a fourni une vidéo de référence — *Mario Party 4 - All Mini
Games*, Typhlosion4President, 1:07:05 — et elle a été lue image par image dans
le navigateur intégré, en mettant la lecture en pause aux horodatages voulus.

Trois comparaisons en sont sorties, et l'une d'elles **retire** un défaut.

### Avalanche! — 3:57 — défaut d'ombre CONFIRMÉ

| console | port |
|---|---|
| chaque sapin porte une **petite ombre sombre et compacte** près du tronc | **quadrilatère bleu clair à arêtes franches** |

Le sol est lisse dans les deux cas. L'écart ne porte donc pas sur le terrain
mais sur la **forme** de l'ombre projetée : une silhouette contre un bloc uni.

### Makin' Waves — 14:40 — défaut CONFIRMÉ

| console | port |
|---|---|
| eau bleu clair **uniforme**, ondulations fines | **sombre, marbrée** de noir et de marine |
| bord du bassin **net et régulier** | traînées sales, concentrées sur les bords |

C'est la signature d'une distorsion **beaucoup trop ample**. L'eau emploie trois
étages de texturage indirect (`m417Dll/water.c:810-826`) avec des exposants
d'échelle **négatifs** (`-2`, `0`, `-3`). `GXSetIndTexMtx` d'Aurora encode
pourtant correctement (`scaleExp + 17`, conforme au SDK) : c'est donc
l'**application** du facteur dans le shader qu'il faut instruire, pas son
encodage.

### Slime Time — 1:28 — défaut PARTIELLEMENT RETIRÉ

**Les confettis sont blancs et gris sur la console.** Ils sont donc **corrects
dans le port**, et la ligne G1 les accusait à tort — moi le premier, en les
décrivant comme « des rectangles gris au lieu d'être colorés ».

L'écart réel est ailleurs, et il est net : les **projecteurs** sont des cônes
**roses/magenta à dégradé doux** sur la console, et des cônes **blancs et
pleins** dans le port. Perte de teinte et de dégradé.

Cela resserre beaucoup la cible. Le chemin de particules prend sa couleur de
`GX_CC_RASC` — la **couleur du sommet** — avec la texture en simple masque
alpha (`hsfanim.c:770-775`). Un cône blanc et plein, c'est une couleur de
sommet blanche au lieu de rose, et un alpha qui sature.

### Ce que la méthode change

Deux défauts passent de *supposé* à *confirmé par comparaison*, un troisième est
amputé de sa moitié fausse, et une cible de plusieurs jours se réduit à deux
valeurs à mesurer. En un quart d'heure.

La leçon de G2 se répète : **regarder l'image coûte moins cher que raisonner
sur le code.** Il aura suffi d'ajouter la référence à côté de la capture.

**Toujours manquant : Stamp Out!.** Personne n'a encore vu si le cahier, les
crayons et les jouets portent une ombre sur la console. Tant que cette image
n'existe pas, on ne sait pas s'il y a un défaut à corriger.

## G1 — Slime Time : les projecteurs, pas les confettis

Mesuré le 2026-09-17 avec une sonde dans `particleFunc` (`hsfanim.c`), sur une
build compilée localement. La sonde imprime, pour chaque système de particules,
le format du bitmap, la branche TEV choisie et la couleur du premier sommet.

Trois systèmes tournent pendant la fin de Slime Time :

| système | format | branche TEV | couleur sommet |
|---|---|---|---|
| confettis | `bmpFmt=8` | `RASC-only` | gris, 126,126,125 → 88,88,68, alpha 250 → 155 |
| éclat/bulles | `bmpFmt=8` | `RASC-only` | blanc bleuté, 237,233,251, alpha 98-170 |
| **projecteurs** | **`bmpFmt=3`** | **`RASC*TEXC`** | **255,255,255,255** |

### Ce que cela règle

Les **confettis fonctionnent** : 150 particules, couleur qui s'assombrit, alpha
qui décroît — un fondu propre. Et la référence console montre des confettis
blancs et gris. Ils n'ont jamais eu de défaut.

Les **projecteurs** sont un système de dix particules dont la couleur de sommet
est **blanc opaque**, avec un TEV en `RASC*TEXC` : le blanc est neutre, donc la
couleur affichée est **entièrement celle de la texture**. Console : cônes roses
à dégradé. Port : cônes blancs. **La texture est donc échantillonnée en blanc.**

### Où chercher

`ANIM_BMP_C8 = 3` (`include/game/animdata.h:9`) : c'est une texture
**palettisée 8 bits**, chargée avec une palette RGB5A3 —
`GXInitTlutObj(..., GX_TL_RGB5A3, palNum)` puis `GXLoadTlut(tlut_obj, slot)` et
`GXInitTexObjCI(..., GX_TF_C8, ..., slot)` (`sprput.c:243-251`).

Deux détails rendent ce chemin suspect sur PC, et aucun n'est vérifié :

1. **`HuSprTexLoad` a une implémentation dédiée sous `OPTIMIZED_TEXTURE_LOADING`,
   drapeau posé par le port** (`CMakeLists.txt`). Elle met en cache le `GXTexObj`
   et le `GXTlutObj` par bitmap et par slot (`tex_initialized`,
   `tlut_initialized`), et ne les réinitialise jamais ensuite.
2. **L'indice de TLUT est le numéro de slot de texture** — `0` pour les
   particules. Toute autre texture palettisée chargée dans le même slot écrase
   la palette, et l'ordre de dessin d'un port n'est pas celui de la console.

### Ce qui est acquis, et ce qui ne l'est pas

Acquis : le défaut est dans la **texture** des projecteurs, pas dans la couleur
de sommet, pas dans les confettis, pas dans le texgen — tout cela est mesuré.
C8 palettisé est le format en cause.

Non acquis : **pourquoi** elle sort blanche. Les deux pistes ci-dessus sont des
lectures, pas des mesures, et cette page a assez d'exemples d'hypothèses
plausibles réfutées par la première mesure venue.
