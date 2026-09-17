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
