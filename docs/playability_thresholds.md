# Seuils de jouabilité réseau — fixés avant la première session à deux machines

**Date : 2026-09-11. Aucune session à deux machines n'a encore eu lieu. Aucun
chiffre de jouabilité réel n'a été observé au moment où ce document est écrit,
et c'est délibéré.**

## Pourquoi ce document existe

Le critère d'acceptation du projet est « pouvoir jouer normalement jusqu'à
décider soi-même de quitter ». Les quatre verdicts qu'une session produisait
jusqu'ici — `DETERMINISM`, `STABILITY`, `USER_TERMINATED`, `OVERALL` — peuvent
tous être verts sur une partie qu'aucun être humain n'accepterait de terminer.

Le modèle livré est le **lockstep** : un tick n'avance que lorsque l'entrée
distante de cette frame est arrivée. En boucle locale, cette attente est nulle —
`stalled_ticks=0` sur toutes les campagnes menées jusqu'ici. Entre deux machines
sur deux réseaux, elle ne le sera pas. Une partie peut donc être parfaitement
déterministe, parfaitement stable, et injouable.

Et si les seuils étaient fixés **après** avoir vu les premiers chiffres, la
première session deviendrait une négociation avec soi-même : on regarderait
« 900 attentes par minute » et on se demanderait si ce n'est pas acceptable après
tout. C'est exactement le mécanisme par lequel un projet se convainc que son
résultat est bon. Les seuils sont donc décidés ici, à l'avance, à partir du
raisonnement seul.

## Ce qui est mesuré

Le moteur compte déjà les deux grandeurs, dans `src/port/netplay_runtime.cpp` :

| compteur | sens |
|---|---|
| `stalledTicks` | nombre total de ticks où la simulation **n'a pas avancé** faute d'entrée distante |
| `maximumStalledTicks` | la plus longue série **consécutive** de tels ticks |

Les deux sont écrits dans le fichier d'état vivant à chaque battement de cœur,
ligne `packet_age_ms=… stalled_ticks=… longest_stall=…`, et étaient jusqu'ici
lus par personne.

Un tick bloqué fait retourner `false` à la fonction de tick : la frame ne
s'exécute pas. À 60 Hz, **un tick bloqué est donc 16,67 ms de temps réel perdu**,
et la conversion est directe :

- 3 600 attentes en une minute = une minute entière perdue,
- 1 800 = le jeu tourne à moitié vitesse,
-   180 = le jeu tourne à 95 % de sa vitesse.

Les deux grandeurs ne disent pas la même chose, et il faut les deux :

- le **taux par minute** dit à quelle vitesse la partie avance ;
- la **plus longue attente consécutive** dit si le jeu se fige. Un ralentissement
  continu de 10 % se supporte pendant deux heures ; un gel de cinq secondes au
  milieu d'un tour fait quitter la partie. Une moyenne peut être excellente et
  masquer un gel.

## Les seuils

| grandeur | `PASS` | `MARGINAL` | `FAIL` |
|---|---|---|---|
| attentes par minute | ≤ **180** | ≤ **720** | > 720 |
| plus longue attente consécutive | ≤ **30** frames | ≤ **180** frames | > 180 frames |

Le verdict retenu est **le pire des deux pairs sur les deux grandeurs** : une
partie n'est pas jouable parce qu'elle l'était pour un des deux joueurs.

### Justification de chaque nombre

**180 attentes/minute pour `PASS`** — 3 secondes perdues par minute, soit 5 %.
Le jeu tourne à 95 % de sa vitesse nominale. Une partie de 2 h 00 en prend 2 h 06.
C'est en dessous du seuil où un joueur attribue le ralentissement au réseau
plutôt qu'au jeu.

**720 attentes/minute pour la limite de `FAIL`** — 12 secondes par minute, soit
20 %. Le jeu tourne à 80 % de sa vitesse. Une partie de 2 h 00 en prend 2 h 30.
C'est visiblement lent, c'est désagréable, et c'est encore **terminable** — ce
qui est précisément la définition du critère d'acceptation. Au-delà, la partie
dure une demi-heure de plus par heure de jeu et le critère « jouer normalement »
ne tient plus.

**30 frames pour `PASS`** — une demi-seconde. C'est l'ordre de grandeur d'un
à-coup que l'on remarque sans le comprendre. En dessous, l'interpolation et le
rythme du jeu de plateau l'absorbent.

**180 frames pour la limite de `FAIL`** — trois secondes. Au-delà de trois
secondes sans image, un joueur ne pense plus « ça rame », il pense « c'est
planté » et va chercher la souris. Le verdict doit basculer avant ce moment, pas
après.

## Ce que ces seuils ne disent pas

Ils ne disent rien de la latence ressentie sur les entrées, qui est gouvernée par
`--netplay-delay` (3 frames par défaut, soit 50 ms de marge avant blocage) et qui
est une **autre** propriété. Un lien à 20 ms de latence stable donnera zéro
attente et une réponse aux boutons retardée de 50 ms ; un lien à 80 ms donnera
des attentes constantes. Les deux se mesurent séparément, et seul le second est
couvert ici.

Ils ne disent rien non plus du comportement sur **coupure** de lien, qui est un
critère distinct du plan (section 5E) et qui se teste en débranchant, pas en
mesurant.

## Si les seuils se révèlent mal choisis

Ils peuvent l'être : ils sont dérivés d'un raisonnement, pas d'une mesure. Mais
la règle est qu'on ne les change pas en regardant un résultat qui déplaît. On les
change en écrivant **pourquoi le raisonnement ci-dessus était faux**, avec la
date, dans ce fichier, au-dessus de l'ancienne version. Un seuil déplacé sans
justification écrite est un seuil qui n'existe pas.
