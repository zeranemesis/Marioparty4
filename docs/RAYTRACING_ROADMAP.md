# Ray tracing : ce qu'il faudrait pour une qualité professionnelle

État au 11 septembre 2026, révisé le soir même après le lot composition /
ombres / définition. Le ray tracing marche, coûte 1,1 à 2,1 ms au niveau par
défaut, et n'a produit aucune erreur sur vingt scènes. Ce document liste ce qui
sépare ça d'une implémentation qu'un studio expédierait.

Il est ordonné par ce que je corrigerais d'abord, pas par difficulté. Chaque
point dit ce qui est su, ce qui ne l'est pas, et ce que ça coûterait.

---

## Palier 0 — Ce qu'on ne sait pas encore, et qu'il faut savoir avant tout

Ces points ne sont pas du travail de rendu. Ce sont des trous dans la
connaissance de ce qui a déjà été construit, et tant qu'ils sont ouverts, aucune
affirmation sur la qualité ne tient.

### 0.1 Le coût réel par frame n'a jamais été mesuré

Toutes les mesures portent sur **la passe de tracé isolée**, via des horodatages
GPU sur sa propre file. Le jeu tourne avec `video.enableVsync: true` et
`targetFrameRate: 60` : les « 60 FPS » affichés pendant toute la campagne de
mesure sont le plafond, pas de la marge.

Donc on ignore : ce que le ray tracing coûte **de bout en bout**, contention avec
Dawn comprise, et si le jeu tiendrait 60 Hz sans plafond sur une machine plus
modeste. La passe privée tourne sur une file séparée d'un périphérique séparé ;
son temps ne s'additionne pas simplement à celui du rendu.

**À faire** : désactiver vsync, mesurer le temps de frame moyen et le 99e
centile, ray tracing allumé et éteint, sur plusieurs scènes. C'est une soirée de
travail et ça conditionne tout le reste.

### 0.2 Un seul GPU

Tout a été mesuré sur une RTX 5090. Rien ne dit comment ça se comporte sur une
carte d'entrée de gamme, sur AMD (RDNA2+ fait DXR 1.1), ou sur Intel Arc. Le
chemin de repli quand DXR manque est testé ; celui où DXR existe mais est lent ne
l'est pas.

**À faire** : au minimum une carte milieu de gamme et une AMD. Sans ça, les
quatre niveaux de qualité sont calibrés sur un seul point.

### 0.3 Couverture des scènes

Treize scènes sur soixante-et-un mini-jeux et neuf plateaux. Les mini-jeux sont
tous atteignables (`-MinigameIndex`) ; les plateaux au-delà du troisième
demandent de débloquer la sauvegarde.

**À faire** : balayer les soixante-et-un mini-jeux — c'est mécanique, l'outil
existe. Débloquer une sauvegarde complète pour les neuf plateaux.

### 0.4 Rien ne tourne en intégration continue

`tools/test_raytracing.ps1` et `tools/sweep_raytracing.ps1` existent et
fonctionnent, mais aucun workflow ne les lance. Une régression de rendu ne serait
vue par personne.

**À faire** : un job qui lance le balayage et échoue si le journal contient une
erreur `aurora::rt`, si la couverture du masque sort de 5–60 %, ou si le temps de
tracé dépasse un budget. Il faut un runner Windows avec un GPU DXR, ce que la CI
actuelle n'a pas.

---

## Palier 1 — Qualité d'image : ce qui se voit

### 1.1 Vecteurs de mouvement

**Corrigé au passage (11 septembre)** : l'affirmation ci-dessous était fausse
des deux côtés. Le test par tampon guide accepte **99,1 %** des pixels sur un
plateau posé, mesuré (`AURORA_RT_DEBUG_MODE=11`) — il n'échoue pas « à chaque
pixel en mouvement ». Et l'accumulation ne convergeait rien du tout, caméra
immobile comprise, parce que le motif d'échantillonnage était identique à chaque
frame : `lerp(x, x, a)` vaut `x`. Le motif bouge maintenant.

Ce qui reste vrai : sans vecteurs de mouvement, un pixel accepté dont la surface
a bougé garde un historique qui ne la décrit plus.

Le blocage est réel : la géométrie est cuite relativement à la matrice de chaque
draw et placée par des transformations d'instance renouvelées chaque frame, donc
il n'y a pas d'espace stable où reprojeter, et la découpe en groupes peut changer
d'une frame à l'autre.

**À faire** : conserver les transformations d'instance de la frame précédente et
une correspondance de groupe stable (`g_layoutHash` dit déjà quand la découpe
change). Pour un impact, reprojeter la position objet par la transformation
précédente puis par la projection précédente. Rejeter sur désoccultation.
C'est le plus gros morceau de ce document.

### 1.2 Débruiteur guidé par la variance — **à moitié fait** (11 septembre)

Un poids d'accord de signal, mis à l'échelle de la dispersion locale, a été
ajouté : c'est la solution de repli documentée de SVGF quand il n'y a pas de
variance accumulée. Il garde 97,6 % de la netteté de bord contre 92,7 % sans lui,
au même grain.

Reste le vrai SVGF : une variance **accumulée temporellement**, qui sait séparer
le bruit d'échantillonnage d'un vrai bord, là où une dispersion spatiale voit les
deux pareil. Il faut un canal pour la porter — l'alpha de `gOutput` est libre.

### 1.3 Bruit variable par frame — ~~à faire~~ **fait** (11 septembre)

Fait, et ça n'aurait pas dû attendre les vecteurs de mouvement : sans motif
mobile l'accumulation ne servait à rien du tout. Poids ramené de 0,15 à 0,05,
seul réglage qui égale l'ancien grain en étant plus stable.

Ce qui reste : la graine mobile est un hachage blanc, pas du bruit bleu variant
par frame. Un vrai bruit bleu temporel convergerait plus vite encore.

### 1.4 Anti-ghosting

Le seul garde-fou est le test normale + distance. Un studio y ajoute un cadrage
par le voisinage — borner l'historique par la plage des valeurs voisines de la
frame courante — qui rattrape les cas où la géométrie passe le test mais
l'éclairage a changé.

### 1.5 Le masque 2D déborde — ~~à faire~~ **fait** (11 septembre)

Le terme est composé **avant** le calque 2D, à la dernière transition 3D → 2D de
la passe, et le masque est éteint. Voir `docs/RAYTRACING.md`. La tentative
antérieure plantait parce qu'elle créait ses objets Dawn dans la passe, côté
worker ; ils sont maintenant créés au scellement de la passe, du côté qui
enregistre les commandes.

Reste de cette famille : les quads de sprites découpés, dont le rayon primaire
traverse désormais les plus translucides, mais pas tous (voir 2.1).

---

## Palier 2 — Fidélité des matériaux et de l'éclairage

### 2.1 Test alpha réel

La géométrie découpée est tracée comme opaque, avec une pondération statistique
par l'alpha moyen de sa texture. Ça évite qu'une canopée occulte comme un mur,
mais la lumière qui passe est uniforme au lieu d'avoir la forme des trous.

Un studio utilise des any-hit shaders ou des *opacity micromaps*. C'est
significatif ici : la découpe atteint 6 à 35 % des draws en mini-jeu.

**Avancé le 11 septembre** : le rayon primaire franchit désormais jusqu'à quatre
surfaces dont l'alpha moyen est sous 0,6, ce qui a supprimé l'essentiel des
rectangles sombres autour des sprites. Pas tous — c'est le dernier tiers qui
demande le vrai test alpha.

### 2.2 Réflexions

Mesuré sur deux populations : 1 à 4 % des pixels sur un plateau, 0 à 1,3 % des
draws en mini-jeu. Le terme ne fait presque rien, et **la couverture est la
limite, pas la couleur** — c'est pour ça que la table bindless a été écartée.

Si on veut de vraies réflexions, il faut décider quelles surfaces devraient
réfléchir plutôt que de suivre l'environment mapping du jeu, ajouter un modèle de
rugosité (elles sont aujourd'hui miroir + Fresnel), et alors seulement les vraies
textures deviennent utiles. C'est un choix artistique avant d'être technique.

### 2.3 Une seule lumière

Les ombres suivent la lumière la plus brillante de la frame. Les scènes en ont
plusieurs. Et la lumière de MP4 est une fausse lumière infinie posée pour
l'éclairage par sommet, pas une source physique — d'où la portée d'ombre bridée à
0,10 fois la diagonale pour éviter qu'elle assombrisse tout.

Faire mieux demande de décider ce que les lumières du jeu *signifient*, ce qui
est encore un arbitrage artistique.

### 2.4 Rebond unique, albédo moyen

L'éclairage indirect rebondit une fois, sur la couleur moyenne de la texture
touchée. Correct pour du basse fréquence, loin d'une vraie illumination globale.

---

## Palier 3 — Finitions d'ingénierie

| | |
|---|---|
| Tampons d'indices | Trois sommets uniques par triangle alors que la source GX est souvent déjà indexée. Tampons plus petits, constructions plus rapides. Non mesuré. |
| Compactage des BLAS | 1,6 à 3 Mo aujourd'hui : l'enjeu n'existe pas tant que les scènes restent de cette taille. |
| Refit plutôt que reconstruction | Déjà tenté et annulé plus tôt dans le projet. |
| Budget adaptatif | Baisser le niveau de qualité automatiquement si le temps de frame dérape, une fois 0.1 mesuré. |
| Décomposition par passe | Les horodatages couvrent tracé + débruiteur ensemble. On sait par soustraction que le débruiteur est gratuit, mais deux paires de requêtes le diraient directement. |

---

## Ce que « professionnel » coûte, honnêtement

Le palier 0 est une semaine et change tout ce qu'on peut affirmer. Le palier 1
est le gros du travail d'image : les vecteurs de mouvement seuls valent plusieurs
jours, la recomposition avant le calque 2D autant, avec un risque réel puisqu'une
tentative a déjà planté.

Les paliers 2 et 3 sont optionnels au sens strict : ils rapprochent d'un rendu
physique, mais le jeu est un Mario Party sur GameCube. La question « jusqu'où
faut-il aller » est artistique, et elle n'a pas été posée.

Mon ordre : **0.1 d'abord** — sans le coût réel par frame, on optimise à l'aveugle
— puis 0.4, puis 1.1.

---

## Ajouté le 11 septembre : un banc déterministe

Presque toutes les mesures de ce projet comparent deux exécutions scriptées qui
ne tombent pas sur la même image — les personnages s'animent, l'ordre des tours
est tiré au sort. Quand l'écart est large et monotone ça tient ; quand il est de
l'ordre de la variation d'une image à l'autre, la mesure ne conclut pas, et
plusieurs questions de cette session sont restées ouvertes pour cette seule
raison.

**À faire** : un seul processus qui rend la même image deux fois en basculant le
réglage entre les deux, et écrit la différence. Ça rendrait inutiles la moitié
des réserves de `docs/RAYTRACING.md`, et c'est moins de travail que la plupart
des points ci-dessus.
