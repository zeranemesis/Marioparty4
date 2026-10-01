# Plan d’optimisation et de qualité Quest

> État à jour, mesures et reste à faire : `docs/quest-optimization.md`, section « Plan d'optimisation ». Ce plan garde les critères d’origine.

## Objectif et règle de décision

Améliorer la fluidité, la netteté et la stabilité mémoire de Party Board sur
Quest 3 sans dégrader le plateau, les mini-jeux, le HUD ni la réalité mixte.
Chaque changement doit être isolé, relié à une mesure reproductible et
accompagné d’un contrôle visuel. Les chiffres ci-dessous sont des références
observées, pas des garanties ni des objectifs de gain.

## Références actuelles

| Scène / capture | Affichage | Résolution stéréo | Débit observé | Interprétation |
| --- | ---: | ---: | ---: | --- |
| Plateau Toad, scène 89 | 120 Hz | 65 % | 47–57 images/s | Le plateau est actuellement le cas lent. Garder séparée la cadence de rendu mesurée de la cadence d’affichage XR. |
| Mini-jeu m428 | 120 Hz | 65 % | producteur 87–91 Hz; présentation 78–85 Hz | `source` est le débit de nouvelles images prises par le ring; `presented`/`worldNew` est le débit présenté. L’écart et 58–66 `ringFull` par fenêtre de 2 s signalent de la pression sur le ring, sans identifier sa cause. |

**Objectif de rendu :** obtenir d’abord 60 images/s stables sur le plateau Toad
à 65 %, puis augmenter progressivement la résolution vers 80 %, puis 100 % si
le budget GPU reste respecté et si chaque hausse apporte un gain visuel mesuré.
Ne pas viser une résolution plus élevée au prix d’un débit instable.

Pour m428, les fenêtres de la build 88 montrent peu ou pas d’images XR en retard,
mais `copyGpuMax=-1` (`gpuSamples=0`). Certaines fenêtres ont
`acquireCpuMax` proche de `copyMax` (maximum mural par fenêtre, jusqu’à environ
5 ms). Ces données n’établissent ni un goulot GPU, ni que l’acquisition OpenXR
est toujours le goulot : les compteurs sont des maxima et les timers de copie
GPU étaient désactivés. La résolution adaptative était déjà à son plancher de
65 %.

**Protocole de comparaison :** enregistrer le commit, le casque, la fréquence
réellement acceptée, le placement de table, la scène, la résolution, l’état du
passthrough et les réglages. Faire au moins trois parcours identiques de 120 s
après échauffement. Comparer une différence seulement si elle se répète au
moins dans deux parcours sur trois et dépasse la variabilité de la référence.
Ne pas confondre 120 Hz XR avec 120 nouvelles images du jeu par seconde.

### Nouvelle observation de stabilité — build 88

Le journal du 27 septembre montre à 16:50:43 un avertissement système de
mémoire faible, suivi de `onDestroy` et de la fermeture du jeu. À 16:50:44,
le thread `MusyX Audio` provoque un SIGABRT :
`FORTIFY: pthread_mutex_lock called on a destroyed mutex`.
Ce défaut survient pendant la fermeture, après l'arrêt du flux AAudio;
ce journal n'établit pas un nouveau crash WebGPU en jeu ni la cause de
la demande de fermeture. Le correctif local appelle `sndQuit()` si MusyX
est installé, avant la fermeture d'Aurora. Le patch MusyX arrête et rejoint
son thread avant de prendre le mutex de ses callbacks. Les patches passent
le contrôle d'application et le fichier matériel MusyX passe le contrôle
de compilation NDK. La fermeture propre reste à valider dans la nouvelle
APK; la compilation complète du jeu n'a pas été effectuée localement.
Mesurer les pics mémoire en priorité, avant d'augmenter la résolution.

## Étapes priorisées

| Priorité / étape | Fichiers et zone ciblés | Mesure avant changement | Critère pour poursuivre / accepter | Régressions à contrôler |
| --- | --- | --- | --- | --- |
| **P0 — stabilité** | `src/port/quest_stereo.cpp`; `patches/aurora-quest-stereo.patch`; `platforms/android/app/src/main/cpp/stereo_view.cpp` | Sur le build de référence par œil, lancer le jeu, rejoindre Toad scene 89, jouer un tour, rejoindre m428 et revenir. Conserver logcat et les journaux du jeu. | Trois parcours sans crash ni `WebGPU error` fatal; confirmer à nouveau les deux débits de référence sur la même route avant toute optimisation. | Crash au chargement d’un pipeline, images noires, HUD absent, erreurs de synchronisation ou changements de cadence dus à une scène/parcours différent. |
| **P0b — calibrage spatial automatique + fond VR** | `platforms/android/app/src/main/cpp/quest_xr.cpp` (pose, contrôleurs, première scène); `table_anchor.hpp/.cpp` (`TableSettings`, ancre et sauvegarde); `src/port/quest_stereo.cpp` et `src/game/hsfdraw.c` (fond monde/table) | Au premier plateau ou mini-jeu après placement, échantillonner la pose du contrôleur tenu immobile et la pose de l’ancre. Relever variance position/orientation et offset contrôleur-ancre; journaliser décision, rejet d’échantillons et pose finale. Capturer le fond en VR et l’écran plat. | Calibrer une seule fois au premier contenu jouable, après une fenêtre d’échantillons stables; appliquer la même pose/table aux plateaux et mini-jeux; sauvegarder avec l’ancre et restaurer après redémarrage. Offrir le recalibrage manuel. Après confirmation avec X sur la manette gauche, attendre au moins 30 échantillons suivis sur 750 ms, dans une tolérance de 5 mm et 0,08 rad (environ 4,6° de lacet); recommencer la fenêtre si le contrôleur bouge ou perd le suivi. Vérifier sur casque le décalage estimé de 3,5 cm entre la pose de prise du contrôleur et la table; conserver le réglage manuel de hauteur pour ajuster le contact physique. Ne pas limiter arbitrairement le déplacement vers une nouvelle table. Retirer la boîte/fond seulement dans le rendu Quest du monde/table. | Tester contrôleur immobile puis déplacement volontaire, mouvement de tête pendant la calibration, dérive/perte et restauration de l’ancre, recalibrage manuel, entrée directe dans mini-jeu et retour du mini-jeu au plateau. La table doit rester stable dans la pièce malgré le mouvement de tête, correspondre à la hauteur/position physique dans les limites, et ne pas sauter à la transition plateau↔mini-jeu. L’écran plat et ses fonds doivent rester inchangés; aucun ciel/décor légitime ne doit disparaître. |
| **P1 — limites de scène et coût du fond VR** | `src/port/quest_stereo.cpp` (`PartyBoard_StereoBackdrop`, `PartyBoard_StereoSphereVisible`); `src/game/hsfdraw.c` (tests de fond et de visibilité) | Pour scene 89 et m428, relever nombre de draws par œil, temps GPU si disponible, débit source/presented et captures sous plusieurs angles de tête. Vérifier quels objets classés « backdrop » sont réellement retirés quand la stéréo est active. | Essayer une modification seulement si les draws coûteux du fond/skybox sont présents dans le chemin Quest et mesurables. Accepter si les draws ou le temps GPU baissent de manière répétée, sans baisse de worldNew/presented ni défaut visuel. La première suppression de boîte fait partie du lot P0b; tout culling additionnel reste séparé. | Fond/anneau de décor visible à travers la pièce, ciel retiré à tort, clipping en se penchant ou en tournant, comportement écran plat modifié. Le retrait doit rester actif uniquement pendant la stéréo Quest (`sActive`). |
| **P2 — attente OpenXR / ring** | `platforms/android/app/src/main/cpp/stereo_view.cpp` (`acquireImage`, copies et transitions `Drawing/Ready/Copying`); `stereo_view.hpp` | Capturer d’abord sans timer, puis avec `tools/collect_quest_performance.ps1 -GpuTiming -RestartGame`. Distinguer durée de `xrAcquireSwapchainImage`, durée de `xrWaitSwapchainImage`, copies HUD/yeux, attente de fence, `ringFull`, slots, worldNew, presented et latence. Comparer les timers activés/désactivés. | Tester un abandon non bloquant/timeout court seulement si les attentes acquisition/wait sont reproductiblement coûteuses. Accepter si les longues attentes et ringFull baissent, sans baisse répétée de worldNew/presented ni hausse de latence ou d’images tardives. | Images périmées, HUD désynchronisé, perte de débit présenté, files de swapchain bloquées. Ne pas ajouter de slot avant d’avoir mesuré l’occupation : cela ajoute mémoire et peut augmenter la latence sans augmenter le débit. |
| **P3 — bornes mémoire et résolution** | `platforms/android/app/src/main/cpp/stereo_view.cpp` (`allocate_images`, `eye_size`); `stereo_view.hpp`; `adaptive_quality.hpp`; `quest_xr.cpp` | Calculer les buffers AHardwareBuffer réels : `max(2 × eyeWidth, hudWidth) × (eyeHeight + hudHeight) × 4 × 3` octets. Capturer PSS et mémoire GPU avant/après chargement du plateau et pendant une transition via outils casque; le collecteur de performance ne fournit pas ces mesures. Noter les redimensionnements et les pics transitoires. | Ne tester une réduction ou séparation de régions que si les buffers dominent le pic mesuré. Accepter si le pic baisse au-delà du bruit de mesure et reste sous le budget avec marge pendant transitions; image utile et lisibilité conservées. | Mauvaise taille/stride, découpe, corruption de partage EGL/AHardwareBuffer, pic accru pendant remplacement des images, texte HUD flou. La séparation de buffers est à risque élevé côté interop Aurora/Dawn. |
| **P4 — HUD, anticrénelage et filtrage des textures** | `platforms/android/app/src/main/cpp/stereo_view.cpp` (HUD 1280/1600/1920); `patches/aurora-quest-stereo.patch` (MSAA et anisotropie); `patches/aurora-quest-quality.patch` (mipmaps RGBA) | A/B d’un seul réglage à la fois sur scene 89 et m428; capturer gros plan du texte, bords géométriques et surfaces obliques. Relever mémoire texture, temps de chargement, FPS, scintillement et artefacts alpha. | Baisser HUD ou MSAA/anisotropie uniquement si ce coût est mesuré. Maintenir la meilleure combinaison qui respecte la lisibilité et réduit un coût constaté. Conserver des mipmaps tant que leur retrait n’a pas prouvé un bénéfice sans scintillement. | HUD flou ou mal filtré, bords crénelés, scintillement, aliasing, franges alpha, textures obliques dégradées. Les mipmaps générées ajoutent théoriquement environ un tiers du niveau principal, avec copies CPU temporaires possibles. |

### Notes sur les deux priorités les plus probables

**Limites de scène / fond.** Le code reconnaît déjà des objets englobant les yeux
comme des fonds et possède un test de visibilité des sphères. Commencer par
vérifier leur efficacité réelle dans Toad scene 89 et m428; ne pas supposer que
le skybox est le poste dominant. Les compteurs de draws et une capture RenderDoc
peuvent établir si le fond produit encore des passes coûteuses. Toute règle de
culling doit rester spécifique au rendu VR afin de préserver l’image écran.

Deux boîtes de fond sont identifiées dans les assets lus sélectivement depuis
l’ISO. `src/REL/w01Dll/main.c` charge l’entrée 1 de `data/w01.bin` en modèle de
fond; son HSF contient `bigbox` (mesh, index 18, 66 objets), absent de l’entrée
2 de premier plan. `w02Dll/main.c` charge aussi l’entrée 1 en modèle de fond;
son HSF contient `b02wall` (mesh, index 6), un cube dédié de 8 sommets et
6 faces avec des bornes géométriques de -4500 à +4500 sur chaque axe. L’entrée
2 de premier plan ne contient pas ce mesh. Le retrait Quest est limité à ces
noms et overlays W01/W02, pendant la stéréo et la présentation MR du
monde/table (`PartyBoard_StereoActive()` et
`PartyBoard_StereoBoardPresentation()`).

Les entrées 1 de fond W03 et W06 ne contiennent pas de mesh nommé box, wall,
sky ou backdrop; W06 contient une chaîne `s3_w6box01`, mais pas de mesh de ce
nom dans ces modèles. W04 contient `kabem`, `kabem1` et `syomenkabe`, qui sont
des meshes muraux distincts, et W05 contient `waku01`, un cadre étendu sur le
plateau; aucun n’est une boîte englobante confirmée, donc ils restent visibles.
Aucun autre objet n’est retiré sans preuve. Le bénéfice FPS demeure à mesurer
sur casque.

**Attentes OpenXR.** `copyMax` comprend plus que le coût de copie GPU; il mesure
une durée murale de transaction, et `acquireCpuMax` est également un maximum.
Leur proximité sur certaines fenêtres motive une mesure séparée, pas une
conclusion. Garder le rendu courant par œil et les trois slots comme référence
tant que les nouvelles mesures ne démontrent pas quel wait est responsable.

### Mesures mémoire à effectuer avant la hausse de qualité

À 65 %, les yeux mesurent 1096 × 1144. Avec un HUD de 1280 × 960,
les trois images partagées occupent environ 52,8 Mio; les swapchains OpenXR
ajoutent environ 121,5 Mio, soit environ 174 Mio pour ces allocations.
Le HUD à 1920 × 1440 porte cet ensemble vers 200 Mio. Ces chiffres excluent
les textures, pipelines, cibles intermédiaires et chevauchements de remplacement.
Ne pas compter deux fois un même AHardwareBuffer importé par Dawn et EGL.

La session observée passe d'environ 719 Mio de PSS au lancement à un palier
d'environ 1174–1176 Mio après les changements de scène et de HUD.
Cela ne localise pas les allocations responsables. Le `dumpsys meminfo`
du redémarrage donne environ 1149 Mio de PSS et 983,5 Mio de Graphics;
ce relevé appartient à une autre phase et ses compteurs ne doivent pas être
additionnés au compteur GPU du runtime.

1. Mesurer le menu stabilisé, le plateau, puis le mini-jeu sur un lancement
   neuf, à résolution 65 % et HUD 1280 fixe; noter les notifications mémoire.
2. Comparer séparément HUD 1280, 1600 et 1920, avec yeux et parcours identiques;
   relever les pics pendant le remplacement des images, puis le palier final.
3. Inventorier textures/mipmaps et pipelines par scène si le palier continue
   de monter après échauffement. Un nombre de pipelines ne donne pas leur
   coût mémoire en octets.
4. Comparer taille fixe et qualité adaptative pour chercher un chevauchement
   prolongé entre anciennes et nouvelles images. N'augmenter la résolution
   qu'après avoir vérifié la marge pendant les transitions.

## Calibration qualité adaptative

Le contrôleur de `adaptive_quality.hpp` baisse la résolution par pas de 5 %,
remonte après cinq fenêtres calmes et a un plancher de 65 %. Il prend déjà en
compte pression GPU, CPU et congestion des images en vol. Pour les références
actuelles à 65 %, baisser encore un seuil n’est pas un levier disponible. Avant
de modifier l’algorithme, déterminer si Toad scene 89 et m428 sont limités par
le GPU, le CPU, les acquisitions OpenXR ou le ring. Le jalon est Toad stable à
60 images/s à 65 %; ensuite tester 80 % puis 100 %, un palier à la fois. Garder
le palier supérieur seulement si le budget GPU laisse une marge reproductible
et le débit reste stable. Valider avec `tools/test_quest_quality.ps1` et les
mêmes parcours casque.

## Vérification et acceptation globale

- Pas de crash, `FATAL` ni erreur WebGPU non capturée sur les parcours retenus.
- Consigner séparément, pour chaque scène, source, presented/worldNew, late%,
  `ringFull`, slots, durée d’acquisition, `copyGpuMax`, latence et facteur de
  résolution. Les compteurs par fenêtres ne sont pas des percentiles frame.
- Relever séparément PSS et mémoire GPU; conserver assez de marge pour le pic de
  transition, pas uniquement le plateau stable.
- Comparer à fréquence, table, itinéraire, options, échauffement et commit
  identiques. Aucun gain numérique n’est attendu ou promis avant mesure.
- Refaire les contrôles visuels : cadrage stéréo, salle visible, plateau,
  consignes, HUD, transparences, textures obliques et retour au plateau.
- Après changement du contrôleur, exécuter `tools/test_quest_quality.ps1`.
  Après changement rendu/pipeline, utiliser `tools/test_quest_stereo_render.ps1`
  avec Visual Studio et GPU; après modification d’un patch Aurora, exécuter
  `tools/test_submodule_patches.ps1`. Les parcours Quest restent requis pour
  les critères de performance et de qualité visuelle.

## Ordre de travail

1. Confirmer le build stable et refaire les baselines Toad scene 89 et m428.
2. Instrumenter les limites de scène et les waits OpenXR avant de toucher au
   rendu ou d’ajouter des buffers.
3. Traiter le poste dominant mesuré : bornes/fond, attente d’acquisition ou
   budget mémoire; mesurer chaque modification séparément.
4. A/B HUD, MSAA, anisotropie et mipmaps seulement selon le coût constaté.
5. Répéter les mesures sur d’autres scènes et mini-jeux atteignables, dont les
   routes complètes consignes → partie → résultat → retour au plateau.
