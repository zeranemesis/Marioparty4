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
