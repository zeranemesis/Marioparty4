# Rollback : avancement du moteur

Le jeu en ligne reste en lockstep. Le noyau de rollback sait prédire, restaurer et rejouer un état fourni par des callbacks, mais les snapshots du moteur ne couvrent pas encore une partie complète. Ne pas interpréter une sonde de capture réussie comme une validation du rollback en jeu.

## Incrément actuel

- Noyau : les erreurs ou exceptions de sauvegarde, restauration et simulation rendent la session terminale. Une sauvegarde partiellement écrite ne peut pas être restaurée. Le signal de resimulation couvre aussi la restauration, est restauré automatiquement à la sortie et isolé par thread.
- Manettes : snapshot explicite de 30 régions, dont boutons précédents, répétitions, anciennes directions, sticks, gâchettes, erreurs, compteurs et état logique des vibrations. Les handles SDL et callbacks de périphériques sont exclus. La restauration n'émet aucune commande matérielle. Elle doit être effectuée à une frontière de tick sans polling concurrent.
- Aléatoire et compteur : capture/restauration des graines frand et rand8 et de GlobalCounter dans une structure versionnée distincte.
- Sonde native : ajoute le PAD et les graines/compteur aux captures existantes des heaps, coroutines et overlay. Le message précise que le snapshot reste partiel et le rollback complet désactivé.

## Travail indispensable avant activation

1. Couvrir explicitement les globals du moteur : objets, séquences de mini-jeu, modèles, motions, caméras et sprites. Ne pas copier en bloc le processus ou les sections du moteur contenant SDL, audio, fichiers et sockets.
2. Isoler un tick déterministe complet. HuPrcCall et MGSeqMain ne suffisent pas : Hu3DExec avance aussi Hu3DMotionNext, HuSprFinish et Hu3DAnimExec hors de la boucle de simulation actuelle. — **Mesuré et corrigé pour le tick rejoué** (défaut D5) ; reste ouvert pour le cadencement hors rejeu (défaut D6).
3. Gérer les durées de vie des allocations, coroutines et modules. Une adresse réutilisée ne prouve pas que l'objet est le même ; la prévalidation d'un composant ne rend pas l'ensemble des snapshots atomique.
4. Gérer les effets externes : audio, vibrations, sauvegardes et succès. Le signal de resimulation est disponible mais il n'est pas encore consommé par ces sous-systèmes.
5. ~~Vérifier une vraie restauration puis resimulation sur le moteur et comparer l'état obtenu à une exécution sans prédiction~~ — **fait**, par `PARTYBOARD_FORCE_ROLLBACK`. **Neuf comparaisons réussies, zéro échec, zéro gâchis** sur un replay de plateau complet, après que la politique d'armement a été corrigée à partir d'un relevé par frame. Les sections 2, 2bis et 2ter ci-dessous racontent la mesure, l'erreur de lecture qu'elle corrige, et le goulot qui s'est déplacé en cours de route.

Les tests de noyau utilisent un état synthétique. Les tests PAD/horloge vérifient de vrais globals du jeu en mode headless, sans partie graphique. Aucune de ces validations ne prouve à elle seule le déterminisme du jeu complet.

## Ce que le harnais local a mesuré — 2026-09-11

`PARTYBOARD_FORCE_ROLLBACK` fait ce que le point 5 ci-dessus réclamait : une
vraie restauration suivie d'une resimulation, comparée à l'exécution sans
prédiction. Il n'est plus à faire, il est fait, et il a produit trois résultats.

### 1. Le point 2 était exact, et il est maintenant corrigé pour le rejeu

« `Hu3DExec` avance aussi `Hu3DMotionNext`, `HuSprFinish` et `Hu3DAnimExec` hors
de la boucle de simulation » — c'était écrit comme une prédiction. Le probe l'a
mesuré : un retour arrière d'**une seule frame** ne se reproduisait pas, quinze
sous-systèmes canoniques revenant identiques et `ANIMATION` non,
`(model->motWork).time` valant 267.0 au lieu de 268.0.

C'est le défaut D5 du registre. `PartyBoard_RollbackRunGameLogicTick` appelle
maintenant `PartyBoard_AnimationAdvance()` avant la logique, dans l'ordre qu'une
image rendue impose, et les distances 1, 2 et 4 se reproduisent exactement.

### 2. La porte de capture a refusé 155 des 159 instants interrogés, et on sait laquelle

> **Correction, 2026-09-11.** Cette section s'intitulait « la porte de capture
> refuse 97 % du temps » et concluait que « le moteur en autorise quatre sur
> 47 921 frames ». **Les deux formulations dépassent la mesure**, et l'erreur est
> de ma part.
>
> `src/port/netplay_runtime.cpp` n'interroge la porte qu'aux multiples de la
> période :
>
> ```cpp
> if (frame == 0 || (frame % probe.period) != 0) return;
> ```
>
> et, en cas de refus, abandonne l'occasion jusqu'au multiple suivant plutôt que
> de réessayer à la frame d'après. Les 159 « occasions » sont donc **159 instants
> choisis par ma politique d'échantillonnage**, espacés de 300 frames. « 155 refus
> sur 159 » signifie « la porte était fermée à ces 159 instants précis » et
> **non** « la porte est fermée 97 % du temps ». Les 47 762 frames jamais
> interrogées n'ont rien dit, ni dans un sens ni dans l'autre.
>
> Ce que la mesure établit réellement, et qui reste solide : **quelles clauses**
> ferment la porte, dans quelles proportions relatives, et le fait que quatre
> restaurations réelles se sont reproduites exactement. Ce qu'elle n'établit pas :
> la fréquence d'ouverture de la porte dans le temps.
>
> **La mesure a eu lieu le même jour. Voir la section 2bis ci-dessous : elle
> tranche, et elle donne tort à la lecture d'origine.**

Replay de plateau complet, 47 921 frames, période 300, donc 159 instants
interrogés :

| | |
|---|---|
| refusées | **155** |
| comparaisons tentées | 6 |
| réussies | **4** |
| échouées | **0** |
| refusées pendant la comparaison | 2 (limite du probe, pas du jeu) |

Et la porte qui refuse, clause par clause :

| clause | refus | part |
|---|---|---|
| `layer-hook` | **89** | 57 % |
| `model-draw-hook` | 46 | 30 % |
| `sprite-draw-hook` | 10 | 6 % |
| `wipe-active` | 8 | 5 % |

Ni coroutine active, ni E/S disque : **des rappels au moment du dessin**. Un tick
rejoué n'a pas de passe de dessin pour les exécuter, donc `PartyBoard_Rollback
RenderCanReplayWithoutDraw` refuse toute image qui en porte un.

La clause dominante a un coupable unique : `sprput.c:371` installe
`HuSprLayerHook` pour chaque calque utilisé par le système de sprites, c'est-à-
dire presque en permanence pendant le jeu de plateau.

**Conséquence pour la section 19 du cahier des charges** : la demande de milliers
de tests de rollback par replay n'est pas satisfaite en l'état — quatre
comparaisons ont eu lieu sur ce replay. Mais, la correction ci-dessus l'impose :
on ne sait pas encore si c'est une propriété du moteur ou de la politique
d'échantillonnage du probe. Les deux hypothèses restent ouvertes, elles appellent
des remèdes opposés — desserrer les clauses de sûreté dans un cas, changer
seulement le moment où l'on demande dans l'autre — et c'est précisément pourquoi
la mesure par frame doit précéder toute décision.

### 2bis. La porte interrogée à chaque frame — la mesure qui tranche

`PARTYBOARD_ROLLBACK_GATE_SURVEY=1` évalue
`PartyBoard_RollbackCheckpointSize() != 0` **à chaque frame** et histogramme les
séries de frames ouvertes. Sur le même replay de plateau, 51 000 frames
interrogées, **résultat identique au chiffre près sur les deux pairs** :

| | |
|---|---|
| frames interrogées | 51 000 |
| frames ouvertes | **1 718 — 3,37 %** |
| première frame ouverte | **1** |
| plus longue série ouverte | **312 frames**, soit 5,2 secondes |
| séries ouvertes distinctes | **31** |

Et la distribution de ces séries, qui est le vrai résultat :

| longueur de la série | nombre |
|---|---|
| 1 frame | 15 |
| 2–3 frames | 5 |
| 4–31 frames | **0** |
| 32–63 frames | 3 |
| 64 frames et plus | **8** |

Les clauses qui ferment la porte, sur ces 49 282 refus :

| clause | refus | part |
|---|---|---|
| `layer-hook` | 30 203 | 61 % |
| `model-draw-hook` | 13 287 | 27 % |
| `sprite-draw-hook` | 2 991 | 6 % |
| `wipe-active` | 2 780 | 6 % |
| `region-set-refused` | 21 | 0,04 % |

#### Ce que cela veut dire

**Six captures sur 159 sondages aveugles d'une porte ouverte 3,37 % du temps,
c'est exactement ce que le hasard prédit** (159 × 3,37 % ≈ 5,4). Les anciens
chiffres mesuraient donc la foulée d'échantillonnage, et rien d'autre. La
correction inscrite plus haut était juste, et cette mesure la confirme au lieu de
simplement la soupçonner.

Le comportement réel du moteur est **meilleur** que ce que « 97 % de refus »
laissait croire, et d'une manière que le taux global ne dit pas : la porte ne
s'ouvre pas sur des frames isolées, elle s'ouvre sur **des plages utilisables**.
Onze séries d'au moins 32 frames, dont huit d'au moins 64, et une de 312 — cinq
secondes pendant lesquelles des dizaines de tests de rewind tiennent.

La distribution est nettement **bimodale** : soit une ou deux frames, soit 32 et
plus, et strictement rien entre 4 et 31. Ce n'est pas du bruit ; c'est la
structure du jeu de plateau, où les passes de dessin s'installent et se retirent
par blocs.

#### Décision, conforme au point 2 du plan W5

La politique d'armement de la sonde passe de « aux multiples de la période » à
« à la première frame où la porte est ouverte **à partir de** l'échéance ». La
période cadence désormais les tests au lieu d'en choisir les instants.

**Cela ne desserre aucune clause de sûreté.** Cela change *quand on demande*, pas
*ce qu'on accepte* : chaque refus reste compté et rapporté, clause par clause, et
aucune barrière n'est touchée. C'est la différence exacte avec l'expérience du
hook de calque ci-dessous, qui desserrait une barrière et a été retirée.

### 2ter. Trois politiques mesurées, et le goulot qui se déplace

Le même replay, la même période de 300, les mêmes distances 1/2/4/8. Chaque ligne
est un run complet de 900 secondes.

| politique | armés | comparaisons | réussies | abandonnés à la sauvegarde avant |
|---|---|---|---|---|
| aux multiples de la période | 6 | 6 | 4 | 2 |
| à la première frame ouverte | **23** | 5 | 4 | **18** |
| première frame ouverte **après 4 frames d'ouverture** | 9 | **9** | **9** | **0** |

La deuxième ligne est le résultat le plus instructif du lot, et ce n'est pas
celui qui était attendu. Armer à la première frame ouverte a presque quadruplé
les armements — la politique marchait — mais le nombre de **comparaisons
réellement menées** n'a pas bougé. Le goulot s'était simplement déplacé.

**Un test de rollback a besoin que la porte soit ouverte deux fois** : une fois
pour prendre l'instantané, et de nouveau `distance` frames plus tard pour la
sauvegarde avant qui rend l'excursion annulable. Armer à la première frame
ouverte garantit la première et ne dit rien de la seconde. Or 15 des 31
ouvertures mesurées durent **une seule frame** : s'armer sur l'une d'elles, c'est
gâcher le test à coup sûr. Dix-huit fois sur vingt-trois.

D'où la troisième ligne. La distribution des ouvertures est nettement bimodale —
15 de longueur 1, 5 de longueur 2-3, **rien du tout entre 4 et 31**, puis 3 de
32-63 et 8 de 64 et plus. « La porte est-elle déjà ouverte depuis quatre
frames ? » est donc un discriminateur presque parfait : il rejette toutes les
ouvertures courtes et accepte toutes les longues, **sans avoir besoin de voir
l'avenir**. Quatre est le plus petit nombre qui franchit le mode court, et les
distances testées tiennent largement dans une ouverture de 32 frames.

Résultat : **neuf armements, neuf comparaisons, neuf réussites, aucun gâchis.**
Chaque test armé aboutit. Deux fois plus de comparaisons utiles qu'à l'origine,
et surtout un rendement de 100 % au lieu de 22 %.

Neuf est proche du plafond pour ce replay à cette période : le relevé par frame
n'a trouvé que onze ouvertures longues sur 51 000 frames. Pour en obtenir
davantage il faut baisser la période, pas desserrer une clause.

**Toujours aucune clause de sûreté touchée.** Les trois politiques acceptent
exactement les mêmes instants ; elles diffèrent seulement sur ceux qu'elles
prennent la peine de demander.

#### Une erreur d'observabilité, corrigée en route

La deuxième politique a d'abord été mesurée sur un run mutilé sans que rien ne le
signale. Demander à chaque frame au lieu d'une sur 300 transforme un refus
d'**événement** en **état**, et le probe écrivait toujours une ligne d'environ
700 octets par frame refusée : le fichier de diagnostic a atteint son plafond de
2 Mo à la frame 4449, et tout le reste du run a été perdu. Le changement
détruisait l'observabilité de ce qu'il devait améliorer.

Les compteurs par clause vivent désormais dans le probe lui-même et non dans le
comptage des lignes du journal, de sorte que l'histogramme survit à un fichier
tronqué. Le journal reçoit une ligne quand une attente commence, une quand la
clause bloquante change, une toutes les 600 frames d'attente, et l'histogramme
complet à chaque armement — plus `waited=N`, qui est le chiffre que cette
politique existe pour produire. Sous l'ancienne, la réponse à « combien de temps
a-t-il attendu ? » était toujours « il n'a pas attendu, il a renoncé ».

#### Les clauses se recouvrent — expérience faite, et négative

L'hypothèse évidente était que `HuSprLayerHook`, le hook du système de sprites,
est du dessin pur et pourrait être autorisé : il ne fait qu'appeler
`HuSprDispInit` et `HuSprExec`, et l'horloge logique des sprites est avancée
ailleurs, par `HuSprFinish` dans `PartyBoard_AnimationAdvance`, qu'un tick rejoué
exécute bien.

Cela a été essayé, sur le même replay, dans les mêmes conditions :

| clause | avant | après |
|---|---|---|
| `layer-hook` | 89 | **0** |
| `model-draw-hook` | 46 | **135** |
| `sprite-draw-hook` | 10 | 10 |
| `wipe-active` | 8 | 8 |
| **total refusé** | **155** | **155** |
| tentatives / réussites / échecs | 6 / 4 / 0 | 6 / 4 / 0 |

**Le nombre de captures n'a pas bougé d'une unité.** Les 89 images bloquées par
le hook de calque étaient déjà bloquées par un hook de modèle ; les clauses se
recouvrent presque entièrement. Le changement était correct et sans effet, donc
il a été retiré : desserrer une barrière de sûreté pour un gain mesuré à zéro
est exactement le genre de correction spéculative que ce projet s'interdit.

Ce que l'expérience apprend malgré tout, et qui vaut d'être écrit : **le goulot
est `HU3D_ATTR_HOOKFUNC` sur les modèles**, pas les calques. Quiconque s'attaque
à la couverture de rollback doit commencer par là, et saura que le hook de
calque attend juste derrière.

### 3. D6 ne peut pas se produire à 60 images par seconde

Le détecteur ajouté dans `main.c` n'a rien imprimé sur 47 921 frames. C'est
exactement ce que prédit la lecture de `frame_pacer_simulation_tick`, qui rend
exactement 1 tant que `video.targetFrameRate <= 60`. La moitié positive de la
mesure — un pair à 240 — reste à faire, et la campagne a `-TargetFrameRate` pour
ça.

## Validation de cet incrément

Le noyau passe 49 334 contrôles (tools/test_rollback_network.ps1). Les nouveaux tests couvrent les erreurs et exceptions des callbacks ainsi que l'isolation du signal de resimulation entre threads.

Les restaurations de coroutines et d'overlay prévalident désormais toute la structure avant écriture. Elles refusent les topologies différentes, pointeurs non reconnus, doublons, tailles invalides et buffers aliasant la destination. tools/test_snapshot_restore.ps1 passe 25 contrôles en compilant les véritables process.c et objdll.c, avec buffers de coroutine et module PE synthétiques ainsi que des services périphériques remplacés. Ces tests ne valident pas une reprise réelle de coroutine. La validation reste locale à chaque composant, sans transaction atomique globale.

Le test natif --netplay-self-test inclut maintenant les aller-retour PAD et graines/compteur, ainsi que le refus de format/version/taille incorrects avant mutation. Il restaure les valeurs initiales après chaque essai.

Compilation native et compagnon réussie. --netplay-self-test : PASS pour noyau, snapshots de coroutine/mémoire, transport et runtime incluant les nouveaux états PAD/horloge. Les avertissements de compilation concernent des fichiers REL non modifiés par cet incrément.

