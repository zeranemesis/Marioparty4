# Ray tracing : ce qu'il faudrait pour une qualité professionnelle

État au 11 septembre 2026. Le ray tracing marche, coûte 0,89 ms au niveau par
défaut, et n'a produit aucune erreur sur treize scènes. Ce document liste ce qui
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

L'accumulation temporelle converge **caméra immobile**. En mouvement, le test par
tampon guide échoue à chaque pixel et on retombe sur 8 rayons par pixel, ce qui
est plus bruité que les 48 d'avant. C'est le compromis assumé, et c'est la
principale faiblesse d'image qui reste.

Le blocage est réel : la géométrie est cuite relativement à la matrice de chaque
draw et placée par des transformations d'instance renouvelées chaque frame, donc
il n'y a pas d'espace stable où reprojeter, et la découpe en groupes peut changer
d'une frame à l'autre.

**À faire** : conserver les transformations d'instance de la frame précédente et
une correspondance de groupe stable (`g_layoutHash` dit déjà quand la découpe
change). Pour un impact, reprojeter la position objet par la transformation
précédente puis par la projection précédente. Rejeter sur désoccultation.
C'est le plus gros morceau de ce document.

### 1.2 Débruiteur guidé par la variance

Le filtre à-trous utilise des poids fixes : exposant de normale constant à 12,
tolérance de profondeur constante. Un débruiteur professionnel estime la variance
locale et filtre fort là où c'est bruité, peu là où c'est convergé (SVGF).

Sans ça, une zone déjà propre est floutée autant qu'une zone bruitée. Coût
supplémentaire faible — le débruiteur actuel est mesuré à ~0 ms.

### 1.3 Bruit variable par frame

La graine est purement spatiale, donc le motif est verrouillé à l'écran et glisse
sous une caméra qui bouge. C'était le bon choix **sans** accumulation ; avec elle,
un bruit bleu variant par frame convergerait plus vite et supprimerait l'artefact
de porte de douche.

À faire **après** les vecteurs de mouvement, pas avant : faire varier la graine
sans convergence en mouvement ne ferait qu'ajouter du scintillement.

### 1.4 Anti-ghosting

Le seul garde-fou est le test normale + distance. Un studio y ajoute un cadrage
par le voisinage — borner l'historique par la plage des valeurs voisines de la
frame courante — qui rattrape les cas où la géométrie passe le test mais
l'éclairage a changé.

### 1.5 Le masque 2D déborde

Mesuré : chaque rectangle protégé est plus grand que le panneau qu'il contient,
parce que le quad d'un panneau est plus grand que la partie opaque de sa texture
et qu'un masque construit sur la géométrie ne voit pas l'alpha. Le resserrement
de grille a récupéré 6 % ; le reste est structurel.

**Le vrai correctif** est architectural : composer le terme de ray tracing
**avant** que le calque 2D ne soit dessiné, au lieu de multiplier l'image finie.
Le masque disparaît alors entièrement. Une tentative antérieure a provoqué un
`0xC0000005` dans le worker de rendu ; c'est à reprendre proprement.

---

## Palier 2 — Fidélité des matériaux et de l'éclairage

### 2.1 Test alpha réel

La géométrie découpée est tracée comme opaque, avec une pondération statistique
par la fraction solide de sa texture. Ça évite qu'une canopée occulte comme un
mur, mais la lumière qui passe est uniforme au lieu d'avoir la forme des trous.

Un studio utilise des any-hit shaders ou des *opacity micromaps*. C'est
significatif ici : la découpe atteint 17 à 53 % des draws en mini-jeu.

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
— puis 0.4, puis 1.5, puis 1.1.
