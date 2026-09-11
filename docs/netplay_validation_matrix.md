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

La table contient **66 overlays de mini-jeu**, de `m300Dll`
(overlay 4) a `m463Dll` (overlay 69).
Quatre d'entre eux ont ete traverses par le replay enregistre ; les autres
n'ont jamais tourne en ligne.

| overlay | dll | statut |
|---|---|---|
| 4 | `m300Dll` | `UNTESTED` |
| 5 | `m302Dll` | `UNTESTED` |
| 6 | `m303Dll` | `UNTESTED` |
| 7 | `m330Dll` | `UNTESTED` |
| 8 | `m333Dll` | `UNTESTED` |
| 9 | `m401Dll` | `UNTESTED` |
| 10 | `m402Dll` | `UNTESTED` |
| 11 | `m403Dll` | `UNTESTED` |
| 12 | `m404Dll` | `UNTESTED` |
| 13 | `m405Dll` | `UNTESTED` |
| 14 | `m406Dll` | `UNTESTED` |
| 15 | `m407dll` | `UNTESTED` |
| 16 | `m408Dll` | `PARTIAL` |
| 17 | `m409Dll` | `UNTESTED` |
| 18 | `m410Dll` | `UNTESTED` |
| 19 | `m411Dll` | `UNTESTED` |
| 20 | `m412Dll` | `UNTESTED` |
| 21 | `m413Dll` | `UNTESTED` |
| 22 | `m414Dll` | `UNTESTED` |
| 23 | `m415Dll` | `UNTESTED` |
| 24 | `m416Dll` | `PARTIAL` |
| 25 | `m417Dll` | `PARTIAL` |
| 26 | `m418Dll` | `UNTESTED` |
| 27 | `m419Dll` | `UNTESTED` |
| 28 | `m420dll` | `UNTESTED` |
| 29 | `m421Dll` | `UNTESTED` |
| 30 | `m422Dll` | `UNTESTED` |
| 31 | `m423Dll` | `UNTESTED` |
| 32 | `m424Dll` | `UNTESTED` |
| 33 | `m425Dll` | `UNTESTED` |
| 34 | `m426Dll` | `UNTESTED` |
| 35 | `m427Dll` | `UNTESTED` |
| 36 | `m428Dll` | `UNTESTED` |
| 37 | `m429Dll` | `UNTESTED` |
| 38 | `m430Dll` | `UNTESTED` |
| 39 | `m431Dll` | `UNTESTED` |
| 40 | `m432Dll` | `UNTESTED` |
| 41 | `m433Dll` | `UNTESTED` |
| 42 | `m434Dll` | `UNTESTED` |
| 43 | `m435Dll` | `UNTESTED` |
| 44 | `m436Dll` | `UNTESTED` |
| 45 | `m437Dll` | `UNTESTED` |
| 46 | `m438Dll` | `UNTESTED` |
| 47 | `m439Dll` | `UNTESTED` |
| 48 | `m440Dll` | `UNTESTED` |
| 49 | `m441Dll` | `UNTESTED` |
| 50 | `m442Dll` | `UNTESTED` |
| 51 | `m443Dll` | `PARTIAL` |
| 52 | `m444dll` | `UNTESTED` |
| 53 | `m445Dll` | `UNTESTED` |
| 54 | `m446Dll` | `UNTESTED` |
| 55 | `m447dll` | `UNTESTED` |
| 56 | `m448Dll` | `UNTESTED` |
| 57 | `m449Dll` | `UNTESTED` |
| 58 | `m450Dll` | `UNTESTED` |
| 59 | `m451Dll` | `UNTESTED` |
| 60 | `m453Dll` | `UNTESTED` |
| 61 | `m455Dll` | `UNTESTED` |
| 62 | `m456Dll` | `UNTESTED` |
| 63 | `m457Dll` | `UNTESTED` |
| 64 | `m458Dll` | `UNTESTED` |
| 65 | `m459dll` | `UNTESTED` |
| 66 | `m460Dll` | `UNTESTED` |
| 67 | `m461Dll` | `UNTESTED` |
| 68 | `m462Dll` | `UNTESTED` |
| 69 | `m463Dll` | `UNTESTED` |

Les quatre `PARTIAL` le sont parce qu'ils ont ete joues une fois, dans un
seul mode, par un seul chemin d'entree. Aucun n'a encore ete exerce en 1v3,
en 2v2, en battle ni en duel.

## Ce qui manque encore pour que cette matrice veuille dire quelque chose

- Un enregistrement par plateau. Aujourd'hui il en existe un seul.
- Un moyen de nommer les modes de mini-jeu (4 joueurs, 1v3, 2v2, battle,
  duel) depuis les donnees plutot que depuis une liste ecrite a la main.
- Les tests `SAVE`/`RESTORE`/`REPLAY` locaux, qui sont une dimension
  supplementaire de cette matrice et pas une ligne de plus.
