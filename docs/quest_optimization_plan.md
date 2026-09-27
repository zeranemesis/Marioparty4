# Plan d’optimisation Quest

## Objectif

Améliorer la fluidité, la netteté et la stabilité mémoire de Party Board sur
Quest 3 sans dégrader le rendu du plateau, des mini-jeux, du HUD ou de la
réalité mixte. Chaque changement doit répondre à une mesure reproductible.

## Étape 0 — établir une version de référence fiable

1. Rebuilder et installer la build qui désactive temporairement le pipeline
   stéréo instancié, puis confirmer sur casque le lancement, l’entrée sur un
   plateau, un tour complet et un mini-jeu.
2. Capturer des sessions de référence à 72, 90 et 120 Hz, avec le même plateau,
   la même position de table et le même parcours. Répéter avec un mini-jeu
   léger, un mini-jeu chargé et une transition plateau → mini-jeu → plateau.
3. Conserver pour chaque session la version du commit, la fréquence, la
   résolution, le facteur de résolution adaptative, la durée de chargement,
   les images tardives/perdues, `ringFull`, `copyGpu`, PSS et mémoire GPU.
   Enregistrer aussi les journaux Quest et des captures d’image aux mêmes
   moments.
4. Utiliser `tools/collect_quest_performance.ps1` et
   `tools/analyze_quest_performance.py` quand leur entrée correspond à la
   session mesurée. Activer les timers GPU stéréo uniquement sur une session
   de diagnostic, car ils peuvent ajouter un coût de mesure.

**Décision de sortie :** avoir une mesure stable par fréquence et par scène,
avec les mêmes parcours et réglages. Ne pas comparer des scènes ou des
fréquences différentes comme si elles formaient une seule référence.

## Étape 1 — plafonds et pics de mémoire

1. Relever l’empreinte des buffers stéréo à partir de leurs dimensions réelles
   (`width × height × 4 × 3`) et la rapprocher des compteurs GPU du casque.
2. Distinguer mémoire persistante et pic de chargement en relevant PSS/GPU
   avant le plateau, après son chargement, au début du mini-jeu, puis après le
   retour au plateau. Chercher les pics corrélés à une création de texture ou
   de pipeline.
3. Mesurer le surcoût des mipmaps générées par `aurora-quest-quality.patch`
   sur un échantillon représentatif de textures et sur un chargement complet.
   Le surcoût théorique de la chaîne mip complète approche un tiers du niveau
   principal, auquel peuvent s’ajouter des copies temporaires lors de la
   conversion.
4. Si les buffers dominent, tester séparément une réduction de dimensions ou
   une séparation des régions monde/HUD. Si les textures dominent, tester
   uniquement des changements ciblés sur les formats et scènes qui dépassent
   le budget. Conserver le rendu mipmap dans les scènes obliques comme garde de
   qualité visuelle.

**Garde-fous :** aucun redimensionnement ne doit rogner l’image utile ni rendre
le HUD flou; aucun changement de texture ne doit créer de scintillement,
d’artefacts alpha ou de pointe mémoire supérieure au comportement de base.

## Étape 2 — temps GPU et coût du rendu stéréo

1. Mesurer séparément le temps de copie vers les swapchains OpenXR, le rendu du
   monde, le HUD et les périodes sans nouveau rendu. Comparer les valeurs avec
   les temps d’image disponibles à 72/90/120 Hz.
2. Garder le rendu par œil comme référence de stabilité jusqu’à ce que la cause
   de l’échec Adreno de `GX Stereo Pipeline` soit isolée. Le journal build 85
   rapporte `CreateGraphicsPipelines failed with VK_ERROR_UNKNOWN`; la mémoire
   GPU était également presque entièrement allouée. Ces indices ne suffisent
   pas à conclure si le pilote refuse `clip_distances` ou si la pression
   mémoire provoque l’échec.
3. Si un nouvel essai du rendu instancié est envisagé, le faire derrière une
   capacité/test de compatibilité explicite et dans une branche de test. Le
   tester avec les scènes qui ont échoué et inspecter le journal du pilote; ne
   pas réactiver globalement à partir d’un simple rendu de laboratoire.
4. N’optimiser les copies ou le nombre de passes que si `copyGpu`, `ringFull`
   ou les images tardives montrent que cette partie domine effectivement.

## Étape 3 — calibrer la qualité adaptative

1. Vérifier la qualité adaptative sur les scènes les plus coûteuses, pas
   seulement sur le menu ou le démarrage du plateau. Consigner fréquence,
   late-percent, temps GPU/CPU, facteur choisi et temps passé à chaque facteur.
2. Identifier si les baisses suivent une pression GPU, une limite CPU ou une
   saturation des images en vol. Ne pas baisser la résolution pour masquer une
   attente CPU ou une congestion causée par les buffers.
3. Ajuster un seul seuil ou palier à la fois. Valider avec
   `tools/test_quest_quality.ps1`, puis comparer les mêmes sessions casque à la
   référence.
4. Fixer le meilleur facteur stable par fréquence en privilégiant l’absence
   d’images tardives et une lisibilité acceptable du plateau et du texte.

## Étape 4 — chargements et mini-jeux

1. Mesurer durée et mémoire des transitions pour chaque famille de mini-jeux,
   puis prioriser ceux qui présentent une pointe, une lenteur ou un cadrage
   problématique.
2. Étendre le balayage de Free Play aux sept catégories standards et traiter
   séparément les routes d’accès qui ne figurent pas dans ces catégories.
   Enregistrer l’overlay effectivement atteint, pas seulement la ligne choisie.
3. Pour chaque mini-jeu atteignable, vérifier une partie complète : consignes,
   contrôles, caméra, cadrage MR, fin, résultat et retour au plateau. Classer
   séparément les mini-jeux sélectionnables mais non testés et ceux sans route
   d’accès identifiée.
4. Mesurer l’allocation des ressources pendant ces parcours avant de modifier
   la création ou la conservation des textures/pipelines.

## Critères d’acceptation

- Le lancement, le plateau et les mini-jeux ne produisent ni crash ni erreur
  WebGPU fatale.
- Les sessions répétées à chaque fréquence gardent leur qualité cible sans
  images tardives récurrentes ni `ringFull` prolongé.
- Le pic de mémoire reste sous le budget observé avec une marge mesurable, y
  compris pendant les transitions.
- Le plateau, les consignes et le HUD restent lisibles; pas de nouveau
  scintillement, clipping stéréo ou artefact alpha.
- Chaque optimisation est isolée, documentée par ses mesures avant/après et
  accompagnée des tests pertinents (`test_quest_quality.ps1`,
  `test_quest_stereo_render.ps1` et `test_submodule_patches.ps1` selon le
  composant touché).

## Ordre de travail recommandé

1. Stabiliser et vérifier le contournement du pipeline stéréo instancié.
2. Établir les références de performance et de mémoire sur Quest.
3. Traiter d’abord le poste dominant mesuré : pics mémoire, copies stéréo ou
   facteur de résolution.
4. Étendre la couverture des mini-jeux et refaire les mesures sur les scènes
   corrigées.
5. Réévaluer le rendu instancié uniquement après diagnostic Adreno et avec un
   test de compatibilité explicite.
