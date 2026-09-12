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
