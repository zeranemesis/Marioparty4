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
5. ~~Vérifier une vraie restauration puis resimulation sur le moteur et comparer l'état obtenu à une exécution sans prédiction~~ — **fait**, par `PARTYBOARD_FORCE_ROLLBACK`. Voir la section de mesure ci-dessous : quatre comparaisons réussies, zéro échec, sur 159 instants interrogés. La proportion de refus porte sur ces 159 instants, pas sur les 47 921 frames du replay ; la nuance est développée dans la section, et elle est importante.

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
> La mesure qui trancherait est décrite en W5 du plan : évaluer
> `PartyBoard_RollbackCheckpointSize() != 0` **à chaque frame** du replay et
> histogrammer les fenêtres ouvertes. Un run de treize minutes. Tant qu'il n'a pas
> eu lieu, aucun chiffre de cette section ne doit être cité comme une propriété du
> moteur.

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

