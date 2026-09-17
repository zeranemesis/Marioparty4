# Les sept trajets à enregistrer — checklist

**Sept enregistrements, une seule fois, ~5 minutes chacun.** Ils deviennent
ensuite des préfixes réutilisables à l'infini par la campagne automatique.

Deux plateaux sont déjà couverts : leur trajet a été **découpé gratuitement**
dans des enregistrements existants (`w01` dans `walk.txt` à la frame 5385, `w04`
dans `board-replay.txt` à la frame 5961). Il ne reste donc que les sept autres.

---

## Ce que tu n'as pas à faire

**Tu n'as pas à t'arrêter au bon moment.** C'est le point important : la
précision est mon travail, pas le tien.

Joue jusqu'à être sur le plateau, **lance un dé une fois** pour qu'il n'y ait
aucun doute, puis **ferme simplement la fenêtre**. Je relis ensuite le fichier
`.overlays` que l'enregistreur produit, j'y trouve la frame exacte où le plateau
est apparu, et je découpe là. Si tu joues trois tours de plus, ça ne gêne rien —
je coupe au bon endroit.

**Tu n'as pas non plus à réussir quoi que ce soit.** Le trajet sert à atteindre
le plateau, pas à bien jouer.

---

## Avant de commencer : une seule fois

Bowser's Gnarly Party est **verrouillé par défaut** : `GWGameStat.open_w06` vaut
0 tant que le mode histoire ne l'a pas débloqué, et le menu de sélection le lit
(`src/REL/mentDll/main.c:207`). Le port a un réglage pour ça, que la commande
ci-dessous pose automatiquement dans le profil isolé de l'enregistrement :

```
game.unlockBowsersGnarlyParty = true
```

Tu n'as rien à faire, c'est déjà dans la commande du trajet 4. Je le signale
pour que, si le plateau n'apparaît quand même pas dans le menu, tu saches que
c'est un verrou et pas un bug — dis-le-moi et je le note `BLOCKED` avec sa
raison au lieu de te faire chercher.

Même remarque pour **Mega Board Mayhem** et **Mini Board Mad Dash** : ce sont des
plateaux supplémentaires, et je n'ai **pas** vérifié s'ils ont leur propre
verrou. Si tu ne les trouves pas dans le menu, c'est une information utile, pas
un échec — signale-le.

---

## Les sept trajets

| # | plateau | overlay | `GWSystem.board` | module |
|---|---|---|---|---|
| 1 | Goomba's Greedy Gala | 90 | 1 | `w02Dll` |
| 2 | Shy Guy's Jungle Jam | 91 | 2 | `w03Dll` |
| 3 | Koopa's Seaside Soiree | 93 | 4 | `w05Dll` |
| 4 | Bowser's Gnarly Party | 94 | 5 | `w06Dll` |
| 5 | Tutorial Board | 95 | 6 | `w10Dll` |
| 6 | Mega Board Mayhem | 96 | 7 | `w20Dll` |
| 7 | Mini Board Mad Dash | 97 | 8 | `w21Dll` |

### Le trajet, identique pour les sept

C'est celui que les deux enregistrements existants suivent déjà :

```
démarrage → sélection du mode → menu → CHOIX DU PLATEAU → plateau
```

Concrètement, dans les deux fenêtres qui s'ouvrent (une seule manette pilote les
deux sièges par défaut) :

1. passer l'écran de démarrage ;
2. choisir **Party Mode** ;
3. **choisir le plateau de la ligne** ;
4. régler la partie : **2 humains + 2 CPU, 20 tours** ;
5. laisser arriver sur le plateau ;
6. **lancer un dé une fois** ;
7. **fermer la fenêtre.**

Les étapes 4 et 6 sont les seules qui comptent vraiment : 20 tours pour que le
préfixe serve aussi aux parties longues, et un dé lancé pour prouver que le
plateau est bien actif et pas seulement chargé.

### Les commandes

Une par trajet. Copie-colle, joue, ferme.

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w02
```

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w03
```

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w05
```

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w06 -UnlockBowser
```

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w10
```

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w20
```

```bash
tools\record_board_session.ps1 -DiscPath "..\Mario Party 4 (USA) (Rev 1).iso" -Label w21
```

*(Adapte le chemin du disque si le tien diffère.)*

### Ce que ça produit

Chaque commande crée un dossier horodaté sous `work/netplay-recordings/` :

```
work/netplay-recordings/2026-09-12_HHMMSS-local-w02/
    peer-0-input.txt          l'enregistrement
    peer-0-stdout.log         d'où je tire le chemin d'overlays
    session.json              le verdict et les métadonnées
    session-crash-summary.txt le rapport lisible
```

Le nom du dossier contient le `-Label`, donc `w02` est reconnaissable
immédiatement. **C'est tout ce dont j'ai besoin** — je m'occupe du découpage, du
scénario et de l'enregistrement de l'empreinte.

---

## Après

Dis-moi simplement « les sept sont faits » et je fais le reste :

1. je lis le chemin d'overlays de chaque session pour trouver la frame d'entrée ;
2. je découpe le préfixe à cette frame ;
3. j'ajoute sept scénarios générés à `tests/scenarios/generated.json` ;
4. je lance les nuits-machine sur les neuf plateaux.

Si un trajet s'est mal passé — plateau introuvable, plantage, fenêtre fermée trop
tôt — **garde quand même le dossier** et dis-le-moi. Un enregistrement partiel
reste de la couverture, et un plateau introuvable est une information sur un
verrou.

---

## Rappel sur ce que ces trajets prouvent

Rien, en eux-mêmes. Ce sont des **outils**, pas des preuves : ils servent à
amener une campagne automatique jusqu'au plateau. Toute couverture obtenue
ensuite par le singe reste `SCRIPTED` et plafonne à `PARTIAL`.

Les sessions qui comptent comme preuve de jeu — `HUMAN` puis `REAL-NETWORK` —
sont un exercice différent, plus long, et qui vient après.
