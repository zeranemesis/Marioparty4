# Ce qu'il reste à faire — PartyBoard, 2026-09-13 (révision du soir)

Cette version **remplace** le plan écrit ce matin, parce que son étape numéro un
n'existe plus : la fin de partie a été observée cet après-midi et elle ne bloque
pas. Un plan dont la première marche est fausse envoie tout le reste au mauvais
endroit, donc il est réécrit plutôt que rapiécé.

---

## 1. Le chiffre : toujours environ 34 %

Mêmes dimensions et mêmes poids qu'au 11 septembre, pour que la comparaison ait
un sens.

| dimension | poids | 11 sept | 13 sept | |
|---|---|---|---|---|
| Causes racines de déterminisme | 12 % | ~90 % | ~75 % | ▼ |
| Infrastructure crash et campagne | 8 % | ~90 % | ~95 % | ▲ |
| Machinerie de validation | 10 % | ~60 % | ~75 % | ▲ |
| Couverture plateaux | 22 % | ~6 % | **~6 %** | = |
| Couverture mini-jeux | 13 % | ~7 % | ~20 % | ▲ |
| Réseau réel, deux machines | 15 % | 0 % | ~30 % | ▲▲ |
| Sessions longues, fin de partie, boutiques, étoiles | 10 % | 0 % | **0 %** | = |
| Rollback validé | 10 % | ~15 % | ~15 % | = |

**Total pondéré ≈ 34 %.**

Le chiffre n'a pas bougé depuis ce matin, et c'est normal : la journée a servi à
corriger des défauts et à construire des instruments, pas à couvrir du contenu.
Les deux lignes les plus lourdes — plateaux (22 %) et fin de partie / sessions
longues (10 %) — pèsent ensemble près d'un tiers du total et restent à 6 % et
0 %. **Aucune partie de plateau n'a jamais été jouée à deux machines.** Ni
boutique, ni étoile, ni Boo, ni objet, ni bloc.

Le critère d'acceptation — *pouvoir jouer normalement jusqu'à décider soi-même de
quitter* — n'a toujours jamais été approché. Tout ce qui suit est ordonné par
cette phrase.

---

## 2. Ce qui a changé depuis le plan de ce matin

**L'étape n°1 tombe.** Le registre affirmait que la fin de partie se bloque sur
une attente sans sortie dans `fn_1_2420`, sur la foi de neuf campagnes
« bloquées au dernier tour ». Une partie de trois tours a été menée jusqu'à sa
cérémonie et **photographiée trente fois sur dix minutes** : les trente images
diffèrent, la page progresse d'un écran de statistiques à l'autre, `mismatch=0`
jusqu'à la frame 89 280, et la trace `endwait` n'a **jamais** été émise — la
fonction accusée n'est même pas atteinte.

Ce que les campagnes enregistraient comme un blocage du jeu est, selon toute
vraisemblance, **le bot incapable de quitter ces écrans**. Ils affichent
« B Previous Screen » et deux flèches ; un marcheur scripté qui n'envoie que A y
tourne indéfiniment. Un défaut de l'outil de test, présenté pendant des jours
comme un défaut du jeu.

**Ce qui reste entier**, en revanche : l'observation de Valentin en session
humaine — *« la fin est buggée avec le jeu qui continue malgré être le
gagnant »*. Un humain franchit ces écrans ; son observation décrit donc autre
chose, et elle sera tranchée par une vraie partie à deux, pas par un run scripté.

---

## 3. Ce que je peux faire seul, dans l'ordre

### 3.1 Apprendre au bot à sortir des écrans de fin de partie

C'est devenu la première étape parce que c'est elle qui débloque la ligne la plus
lourde encore à zéro. Tant que le bot reste coincé sur les statistiques, aucune
campagne ne peut exercer la fin de partie, la sauvegarde, ni le retour au menu —
et ces chemins traversent `GWGameStat.create_time`, un champ **exclu du hachage
canonique** et relu par emplacement (`filesel.c:1994`). Personne n'y est jamais
allé en ligne.

- Le geste : que le marcheur envoie B et les directions, pas seulement A, et
  qu'il reconnaisse un écran dont il ne sort pas.
- Une métrique de progression, sinon un bot coincé ressemble à un run réussi :
  `GWSystem.turn` publié, et `ABNORMAL_EXIT` pour un run qui atteint ses frames
  sans atteindre ses tours.
- *Vérifié quand :* une campagne courte mène une partie à son classement final
  **et en sort** d'elle-même.

### 3.2 Mesurer D23 au lieu de le supposer

La chaîne est **lue** dans le code : `ParManHook` fait vieillir les particules
dans le chemin de dessin, et sa garde (`prevCounter != GlobalCounter`) n'est mise
à jour **que si le modèle est dessiné**. Une machine qui saute une image rendue
perd un tick, libère un emplacement de moins et tire cinq nombres de moins.
Mesuré le matin : +5 tirages, exactement.

Elle n'est pas **mesurée**. Trois hypothèses « plausibles » se sont révélées
fausses dans la même journée ; celle-ci ne sera pas la quatrième.

- Le geste : un indicateur de test qui fait sauter des images rendues à **un seul
  pair** — exactement la technique qui a démontré le recensement des tas du
  premier coup.
- *Attention :* la porte `PARTYBOARD_HOOK_TICK_GATE` déjà livrée **n'est pas le
  correctif**. Elle saute le hook en entier, donc supprime aussi le dessin. Le
  correctif juste fait **rattraper** les ticks manqués.

### 3.3 L'instrument qui nomme les tirages de hasard

Deux des quatre divergences ouvertes sont de catégorie RNG (D23 : +5 tirages,
D31 : +156). Dans les deux cas on sait *combien*, jamais *qui*. Le recensement
des blocs a nommé le champ exact de D30 ; il n'existe pas d'équivalent côté
hasard.

Le tampon circulaire des appelants existe déjà dans `frand.c` ; il n'a jamais
rencontré une vraie divergence. Il se démontre comme le recensement : avec une
divergence provoquée.

### 3.4 Les défauts graphiques, en commençant par la référence Dolphin

Neuf défauts inscrits, un corrigé et confirmé (**G10**, l'écran blanc du choix du
mode). Le hachage canonique exclut la présentation par construction : **aucune
campagne, jamais, n'aurait pu en trouver un seul.** Ils ne se voient qu'à l'œil.

Pour Avalanche! (G2), la question est simple et sans réponse pour l'instant :
l'avalanche facettée et les particules multicolores sont-elles un défaut du
portage, ou est-ce que le vrai jeu fait pareil ? D'où la comparaison avec
Dolphin.

**État exact de ce chantier ce soir** : voir §6.

Règle de tri déjà écrite : **rejouer le mini-jeu hors ligne** avant d'inscrire un
défaut. Sept réglages changent entre solo et en ligne, dont la résolution interne
qui passe de 12× à 1×.

---

## 4. Ce qui demande deux machines

| quoi | pourquoi ça ne peut pas se faire seul |
|---|---|
| **Une partie de plateau, 5 tours** | c'est le seul geste qui vise le critère d'acceptation |
| D29 (`m410`, ANIMATION) | il faut une nouvelle occurrence pour lire le champ |
| D30 (`m415`, HEAPS) | idem — mais **l'instrument est prêt et démontré** |
| La fin de partie « qui continue » | l'observation humaine de Valentin, à confirmer |

La partie de plateau passe par `record_board_session.ps1`, jamais en lançant le
jeu à la main : une session enregistrée laisse un artefact rejouable, sans lui
deux heures de jeu ne laissent qu'une anecdote.

Rappel qui ne souffre pas d'exception : **un run scripté ne peut jamais porter
une cellule au-delà de `PARTIAL`.** Mille runs verts ne démontrent pas qu'une
personne peut jouer normalement jusqu'à décider elle-même de s'arrêter.

---

## 5. Ce qui attend une décision, pas du travail

**Linux et macOS.** Périmètre fixé par Valentin : le jeu, pas le mode en ligne.
Quatre audits, tous propres — aucune API Windows hors garde dans `src/`, aucun
chemin à antislash, aucune erreur de casse dans `res/`, suffixes `.so`/`.dylib`
déjà en place. Le CI possède déjà `build-linux` et `build-apple`. **La lecture du
source a donné tout ce qu'elle pouvait** : le reste ne se découvre qu'en
compilant. Une poussée, et le CI tranche — mais une poussée peut déclencher une
publication, donc c'est une décision, pas une tâche.

**Conformité de distribution.** Bloquante quel que soit le pourcentage, et
indépendante de tout le reste : la revue de licence que `DOLPHIN_AX_NOTICE.md`
réclame lui-même, l'absence de fichier LICENSE, et une décision écrite sur
`dsp_coef.bin` et sur les fontes commerciales.

---

## 6. Le point précis sur la comparaison Dolphin

Demandée pour trancher G2 (Avalanche!). État au moment d'écrire :

- Le vrai jeu est piloté **de bout en bout depuis l'écran-titre** : fichier de
  sauvegarde, menu des modes, MINI-GAME MODE, Free Play, quatre joueurs, fiche
  de mini-jeu, lancement, partie complète, retour au menu.
- Ce qui débloquait tout : sur x64 la structure `INPUT` de Windows fait **40
  octets** (c'est une union contenant `MOUSEINPUT`). N'en déclarer que la partie
  clavier donne 32, et `SendInput` **rejette chaque appel en renvoyant 0**, sans
  erreur. Corrigé, et le rejet lève désormais une exception au lieu de passer
  inaperçu.
- Ce qui reste : la fiche d'un mini-jeu n'est pas une liste ; la liste est en
  amont (`mgmodedll/free_play.c`, défilement haut/bas au stick ou à la croix).
  Le chemin de retour est maintenant connu — jouer le mini-jeu ressort sur le
  menu MINI-GAME MODE — donc il reste à redescendre dans Free Play et à faire
  défiler jusqu'à Avalanche!. Quelques minutes.
- Gagné en chemin : `405:MEDREY RACE` s'appelle **« Manta Rings »** à l'écran.
  Une ligne de moins à deviner dans la table des mini-jeux.

---

## 7. L'ordre, et ce que chaque étape débloque

| # | étape | qui | débloque |
|---|---|---|---|
| 1 | Sortir le bot des écrans de fin (3.1) | moi | la ligne à 0 %, et le chemin de sauvegarde |
| 2 | Mesurer puis corriger D23 (3.2) | moi | une cause de divergence réelle |
| 3 | Nommer les tirages (3.3) | moi | D31, et toutes les RNG suivantes |
| 4 | Défauts graphiques (3.4) | moi | la liste des neuf, au fil de l'eau |
| 5 | **Partie de plateau à deux** | à deux | le critère d'acceptation, enfin visé |
| 6 | Exécution du CI | Valentin | dit si Linux et macOS compilent |
| 7 | Conformité | Valentin | conditionne toute publication |

Les quatre premières ne demandent qu'une machine et personne d'autre. Elles
existent pour que la cinquième — la seule qui vise réellement le but — ne soit
pas gâchée sur un défaut qu'on aurait pu trouver seul.

---

## 8. Ce qui ferait mentir ce plan

- **Une partie de plateau à deux machines révélera des défauts que rien n'a
  encore pu voir.** Boutiques, étoiles, objets, blocs, sauvegardes : aucun n'a
  jamais tourné en ligne une seule fois. Il faut s'attendre à ce que la ligne
  « déterminisme » redescende encore, et ce sera encore un progrès.
- **Le pourcentage est un résumé, pas une promesse.** Il repose sur des poids
  fixés avant de mesurer, ce qui est la bonne manière de faire, mais aucun
  pourcentage ne remplace la phrase : jouer normalement jusqu'à décider soi-même
  de quitter. Cela n'est jamais arrivé.
