# Plan de tests — couvrir les 9 plateaux et les 59 mini-jeux

Écrit le 2026-09-12 au soir, puis corrigé une heure plus tard par ce que les deux
campagnes lancées ce soir ont effectivement mesuré. Les corrections sont
signalées : elles disent ce que je croyais et ce qui est vrai.

## 1. La carte des écrans, mesurée ce soir

Tout le reste du plan repose là-dessus. Ces numéros ne sont pas des suppositions :
ils viennent du journal des deux campagnes en cours, recoupés avec
`include/ovl_table.h`.

| contexte | module | rôle |
|---|---|---|
| 1 | `bootDll` | démarrage |
| 3 | `instDll` | explication d'un mini-jeu |
| 16 | `m408Dll` | un mini-jeu |
| 70 | `mentDll` | **réglages de la partie, et choix du plateau** |
| 74 | `modeseldll` | **choix du mode** (Fête / Histoire / Mini-jeux) |
| 84 | `resultDll` | résultats du mini-jeu |
| 89 | `w01Dll` | **le plateau** Toad's Midway Madness |

Les deux runs de ce soir, côte à côte :

| étape | partie-complete (sonde intégrée) | nuit-b0 (fichier généré) |
|---|---|---|
| `bootDll` | 116 | 114 |
| `modeseldll` | 1059 | 841 |
| `mentDll` | 2264 | 1658 |
| **plateau `w01Dll`** | **5387** | **4643** |
| `instDll` | 13783 | 10541 |
| mini-jeu | 14135 | 11014 |
| résultats | 20034 | 16914 |
| retour plateau | 20370 | 17249 |

**Les deux jouent vraiment.** Même structure, même enchaînement : plateau,
explication, mini-jeu, résultats, retour au plateau, tour suivant.

## 2. Le défaut que cette mesure a révélé, et que je n'avais pas vu

`modeseldll` dispatche sur un curseur : **0 → mode Fête**, 1 → Histoire,
2 → Mini-jeux (`src/REL/modeseldll/main.c:210-243`).

Le rythme du marcheur envoie un coup de stick horizontal toutes les 240 frames,
en aveugle. Le nombre de crans qu'il donne dans ce menu ne dépend donc que de la
**durée pendant laquelle le menu reste ouvert** — et cette durée bouge de
plusieurs centaines de frames selon ce qui précède. Mesuré ce soir :

- `partie-complete` : 1102 frames dans `modeseldll` → curseur resté à **0** →
  mode Fête → `max_turn = 20` (`mentDll/main.c:840`).
- `nuit-b0` : 817 frames dans `modeseldll` → curseur à **1** → mode Histoire →
  `max_turn = 15` (`BoardStoryConfigSet`, `src/game/board/main.c:361`).

Les traces `modesel_loop` le disent directement : `cursor=0` quatre fois d'un
côté, `0, 0, 1` de l'autre.

**Conséquence : mes six runs de la nuit sont en mode Histoire, pas en mode
Fête.** Ils jouent, ils avancent, mais ce n'est pas le mode qui nous intéresse, et
les crans que j'avais placés pour choisir le plateau tombaient dans le menu des
modes, pas dans la liste des plateaux.

C'est exactement le point de contrôle que j'avais écrit dans la première version
de ce plan — « vérifier que le deuxième run atteint un plateau différent » — sauf
que la réponse est arrivée par la lecture du code, pas en attendant 22h10.

Inscrit comme **D17** au registre.

## 3. Le principe qui gouverne toutes les entrées automatiques

**Un écran charge de mille à quinze cents frames avant de lire la manette, et
cette durée bouge de plusieurs centaines de frames selon ce qui précède.** Viser
une frame fixe ne marche pas ; envoyer un rythme périodique en aveugle ne marche
pas non plus, pour la même raison — c'est ce que D17 démontre.

La seule chose stable est **le contexte courant**. Il fait partie de l'état
canonique, et le protocole vérifie à chaque tick que les deux pairs sont dans le
même (`context_skew=0` dans tous les journaux). Un marcheur qui réagit au contexte
est donc déterministe par construction.

Trois corollaires déjà mesurés, qui restent vrais :

- **A souvent, START rarement.** START tue la séquence d'un mini-jeu
  (`objsysobj.c:86`, `MGSeqPauseKill()`) et ouvre la pause sur un plateau.
- **Le stick derrière un START quitte le plateau.** La boîte « quitter » s'ouvre
  sur *non* (`pause.c:736`) ; seul un déplacement de curseur choisit *oui*.
- **Les crans horizontaux ne partent que du siège 0.** Les deux sièges sont
  décalés de 15 frames ; s'ils poussent tous les deux, un curseur partagé bouge
  deux fois par cran voulu.

## 4. Le correctif en cours : `--netplay-walk-plan=<crans>`

Écrit ce soir, dans `src/port/netplay_runtime.cpp`. Le marcheur garde son rythme
partout, sauf à deux endroits :

- dans `modeseldll` : **aucun horizontal**, A seulement → le curseur ne peut plus
  dériver, le mode Fête est choisi à tous les coups ;
- dans `mentDll` : exactement N crans à droite espacés de 60 frames (le verrou de
  répétition de `PadADConv` dure 20 frames, donc un cran par impulsion), puis A.

Il se construit dans un arbre séparé, `build/walkplan`, pour ne pas toucher au
binaire des deux campagnes qui tournent.

**Ce que je ne prétends pas savoir** : quel plateau correspond à quel nombre de
crans. La table N → plateau sera **relevée sur les runs**, pas devinée. C'est le
seul point du plan qui attend une mesure au lieu de l'annoncer.

## 5. Les phases

### Phase A — cette nuit, et le mur qu'elle a trouvé en dix minutes

Les deux premiers runs sont finis, et **aucun n'a atteint son budget**. Ils ne
sont pas partis en vrille : ils ont **désynchronisé, dans un mini-jeu, au tour 2**.

| run | budget | durée réelle | fin | où |
|---|---|---|---|---|
| `partie-complete-0` | 4 h 30 | **9 min 39** | `DESYNC:HEAPS` frame 32170 | `m415Dll` PYONPYON STAMP |
| `nuit-b0` | 2 h 05 | **11 min 23** | `DESYNC:ANIMATION` frame 38665 | `m431Dll` GURUGURU BOX |

Les deux champs sont nommés :

- PYONPYON STAMP : `HuMemUsedMallocBlockGet(HEAP_SYSTEM)` 67 contre 74, et
  `HuMemUsedMallocSizeGet` 15 360 octets d'écart. **2 champs sur 2300.**
- GURUGURU BOX : `model->attr` bit 0, `HU3D_ATTR_DISPOFF`, sur **13 modèles** —
  treize objets visibles chez un joueur et cachés chez l'autre. **13 champs sur
  5030.**

**C'est le vrai obstacle à une partie de vingt tours, et ce ne sont pas les
menus.** Un budget de 4 h 30 ne sert à rien tant qu'une partie meurt au bout de
dix minutes : ce qu'il faut corriger d'abord, ce sont les mini-jeux qui
divergent. Le budget long reste juste — il ne coûte rien et il servira dès que
les divergences seront traitées — mais il ne fabrique pas à lui seul une partie
complète, et je ne le présenterai pas comme tel.

Cela réordonne le plan : **la phase D passe devant la phase B.**

Ce qui tourne encore cette nuit :

- `partie-complete-1` : même sonde, même binaire. Sa seule question est
  **« la divergence de PYONPYON STAMP est-elle déterministe ? »** Si elle revient
  au même endroit, le défaut est reproductible et donc corrigeable ; sinon il est
  intermittent, et il faudra le traiter comme tel.
- `nuit-b1` : mode Histoire, comme D17 le prédit. Elle cède la place à la
  campagne de relevé dès que le binaire `--netplay-walk-plan` est construit.

### La nuit, telle qu'elle se deroule reellement

Un run meurt en dix a douze minutes sur une divergence. Cela change ce qu'une
nuit peut acheter : pas *une* partie de vingt tours, mais **une recolte** — une
divergence nommee par run, cinq runs par heure et par campagne, sur douze heures.

C'est un meilleur emploi de la nuit que ce que j'avais prevu, et ce n'est pas un
lot de consolation : chaque run produit un champ nomme, et c'est le champ nomme
qui permet la correction.

Ce qui tourne, et ce qui suit :

| campagne | binaire | ce qu'elle donne | fin prevue |
|---|---|---|---|
| `partie-complete` | `build/aexp` | parties en mode Fete, 20 tours, sonde libre | ~20 h 40 |
| `nuit` | `build/aexp` | mode Histoire (D17), divergences du mode Histoire | ~21 h 15 |
| `carte` | `build/walkplan` | **la table crans -> plateau**, 9 runs de 5 min | ~21 h 10 |
| a suivre | `build/hashv3` | balayage des plateaux + test de D19 | la nuit |

`build/hashv3` porte le correctif de D19 : les quatre identifiants de motion
entrent dans le hachage canonique, et `kStateHashVersion` passe de 2 a 3. Il est
a la fois le correctif d'un trou reel et la sonde de D19 et D20.

Trois arbres de construction separes, parce qu'un binaire en cours d'execution
est verrouille sous Windows : recompiler par-dessus ferait echouer l'edition de
liens, et surtout melangerait deux binaires dans une meme campagne.

### Phase B — les plateaux, et comment on atteint chacun

Ce n'est plus une supposition : la liste est lue dans le code.

`mentDll/main.c:1236` declare la liste des plateaux et la position de depart du
curseur :

```c
s32 sp8[6] = { 1, 2, 0, 3, 4, 5 };
var_r30 = 2;
```

Le curseur demarre donc sur l'index 2, qui vaut 0, c'est-a-dire **w01** — ce que
`carte-0` a confirme, 0 cran donnant bien `w01Dll`. Un cran a droite ne donne
pas w02 mais **w04**.

| crans | curseur | plateau | overlay |
|---|---|---|---|
| -2 | 0 | `w02Dll` | 90 |
| -1 | 1 | `w03Dll` | 91 |
| **0** | **2** | **`w01Dll`** | **89** |
| +1 | 3 | `w04Dll` | 92 |
| +2 | 4 | `w05Dll` | 93 |
| +3 | 5 | `w06Dll` | 94 |

Le curseur est borne a 4 tant que `GWGameStat.open_w06` vaut 0
(`mentDll/main.c:1294`), donc **w06 n'est pas atteignable avec un profil neuf**.
Le port a un reglage pour cela, `game.unlockBowsersGnarlyParty`, faux par
defaut, qu'il faudra poser **sur les deux pairs** — et ce champ est bien hache
(`netplay_canonical.hpp:73`), donc un desaccord entre les deux se verrait tout
de suite au lieu de changer la liste en silence.

Les trois plateaux restants ne passent pas par cette liste :

- **`w10Dll`** (didacticiel) est `spC[6]`, atteint par `omOvlCallEx(spC[6], ...)`
  quand `mentDll` est appele avec `arg0 == 2` ;
- **`w20Dll`** et **`w21Dll`** ne sont pas dans `spC` du tout : ils viennent des
  entrees 3 et 4 du menu des modes (`modeseldll/main.c:234-240`), soit deux
  crans de plus dans la liste des modes.

`--netplay-walk-plan=<mode>:<crans>` couvre les deux axes : le premier nombre
choisit l'entree du menu des modes, le second le plateau ou la ligne de la liste.

### Pourquoi la premiere campagne de relevé n'a rien releve

`carte-0` et `carte-1` ont donne **le meme plateau**, w01. La raison est lue, pas
devinee : la liste n'accepte un deplacement que lorsque ses six panneaux se sont
stabilises **et** que son compteur interne a atteint 0x15
(`if (i == 6 && var_r28 >= 0x15)`). Mes crans partaient 180 frames apres l'entree
dans `mentDll`, bien avant que cet ecran existe. Ils sont tombes dans le vide.

C'est la troisieme fois de la journee qu'une fenetre calculee en frames echoue
pour la meme raison. La correction abandonne les frames : **le menu previent
lui-meme quand il ecoute** (`PartyBoard_NetplayWalkMenu`), et le marcheur ne
depense un cran qu'a ce moment-la. Les deux pairs executent le meme code de menu
au meme tick, donc le signal est identique des deux cotes par construction.


Une fois le marcheur corrige, un run long par plateau. Un plateau qui resterait
inatteignable est marque `BLOCKED` avec sa raison — pas `UNTESTED`, et surtout
pas oublie.

### Phase C — les mini-jeux

34 sur 59 jamais atteints. Deux voies :

- par le **mode mini-jeu** (`modeseldll` curseur 2 → `mgmodedll`), en accès
  direct : la route existe et la correspondance ligne → mini-jeu est vérifiée sur
  trois points ;
- par les **parties de plateau**, qui en tirent un par tour — c'est ce qui a fait
  jouer SKY DIVE, PYONPYON STAMP, AIR DOSSUN et THE ROCK CLIME entiers, écran de
  résultats compris.

La seconde est la seule qui prouve qu'un mini-jeu se joue *dans son contexte* :
entrée depuis le plateau, résultats, distribution des pièces, retour.

### Phase D — les défauts ouverts

Dans l'ordre de netteté, le plus reproductible d'abord :

1. **KINOPIO HAMMER** — pointeur nul à l'adresse 0, les deux pairs ensemble, 3
   occurrences sur 3 graines. Déterministe.
2. **PYONPYON STAMP** — 28 blocs de tas d'écart, le plus gros mesuré.
3. **AIR DOSSUN** — `object->trans.y`, 12,44 unités d'écart : une trajectoire
   entière diverge.
4. **CRAY SHOT** — 8 blocs, deux fois à l'identique.
5. **D16 / BATTANDOMINO** — 4 blocs de 160 octets.
6. **D14** — fin de vidéo non déterministe.

Chacun suit la même boucle : comparaison des deux rapports → champ nommé →
instrumentation si la lecture ne suffit pas → test vu rouge → correctif dans un
commit séparé → rejeu du même scénario pour exiger la disparition.

### Phase E — ce que seul Valentin peut faire

- une partie **complète à deux machines**, jusqu'au classement ;
- le comportement sur une **coupure de lien** en pleine partie ;
- les **défauts de rendu** — les neuf de `defauts_graphiques.md` sont hors
  d'atteinte du hachage canonique, qui exclut la présentation par construction.

## 6. Ce qui existe dans le code et qui pourrait tout accélérer

`src/REL/selmenuDll/main.c` est un **menu de développement** qui lance n'importe
quel plateau et n'importe quel mini-jeu par son index — la table commence à la
ligne 111 : `***:BOARD W01`, `W02`, … `W21`, plus les mini-jeux. Il pose lui-même
`GWSystem.turn = 1` et `GWSystem.max_turn = 20`.

**Rien ne l'appelle dans cette version.** Aucun chemin de jeu n'y mène.

L'ouvrir depuis le port donnerait un accès direct et déterministe à chaque plateau
et chaque mini-jeu. Mais cela contournerait les vrais menus, et la règle du
marcheur est explicite : *« never advance an overlay directly »*. Je le signale
comme une option à décider, pas comme quelque chose que je fais de mon propre chef.

## 7. Les plafonds, qui ne bougeront pas

Tout ce que produit une campagne est `SCRIPTED` et plafonne à `SCRIPTED-PARTIAL`.
Une partie de vingt tours terminée automatiquement ne vaudra jamais un `PASS` :
elle prouvera que le jeu tient vingt tours, pas qu'un humain peut y jouer
normalement jusqu'à décider lui-même de s'arrêter.

Les deux lignes qui comptent pour le critère d'acceptation — *joué par un humain*
et *validé entre deux machines* — sont à **0 sur 59** et **0 sur 9**, et aucune
nuit-machine ne les fera bouger.

Ce que le travail automatique achète, c'est que les sessions à deux machines ne
soient plus dépensées à découvrir des défauts qui n'avaient pas besoin de deux
machines — comme celle de ce matin, arrêtée en quatre minutes trente par un champ
de structure non initialisé.
