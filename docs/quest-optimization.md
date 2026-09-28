# Optimisation Quest : implementation et validation

## Lot implemente

- La frequence XR reste la plus haute annoncee par le runtime. La simulation reste a 60 Hz ; le rendu hors ligne peut interpoler les images intermediaires. Le rendu en ligne conserve son rythme de securite.
- La resolution 3D suit les compteurs OpenXR CPU/GPU et le budget de la frequence effective. Baisse par pas de 5 %, remontage apres cinq mesures calmes, plancher 65 %. Une saturation CPU connue seule ne provoque pas de baisse de resolution. Sans compteurs GPU, la saturation du ring sert de secours borne.
- Y selectionne une limite de qualite : economie 80 %, equilibre 100 %, nettete 125 % de la recommandation OpenXR par oeil. Le HUD utilise respectivement 1280, 1600 et 1920 pixels de largeur. Les resolutions classiques sont conservees pour l'ecran du jeu.
- Le redimensionnement attend la liberation des images. Les buffers et le HUD sont remplaces ensemble seulement apres allocation reussie ; un echec conserve les buffers precedents.
- Les textures RGBA/RGB compatibles sans mipmaps recoivent une chaine calculee a leur chargement ou mise a jour lorsqu'elles servent au monde 3D. Moyenne en lumiere lineaire, ponderation alpha, tailles impaires et couverture des decoupes sont gerees. Le niveau zero reste exact. Les copies EFB, textures de remplacement, textures incompatibles et chaines existantes gardent leur traitement.
- Le filtrage anisotrope force a 8x concerne la scene 3D. Les messages et menus conservent leur selection du niveau original. MSAA 4x reste actif. Le mode de super-resolution accentue est reserve aux images sous la resolution recommandee ; au-dessus, le mode normal evite une accentuation excessive.
- Les copies des yeux et du HUD partagent un flush et une barriere de fin. Les mesures separent attente CPU d'acquisition, transfert CPU, copie GPU facultative, cadence XR et nouvelles images du monde.

La super-resolution du compositeur est spatiale. Ce lot n'ajoute pas de modele IA ni de reconstruction temporelle.

## Mesurer sur Quest 2 et 3

Installer l'APK Quest, conserver le meme plateau, placement et profil, puis laisser chauffer le jeu. Mesurer au moins deux minutes sur plateau, transitions et chaque minijeu concerne. Comparer les memes parcours avant/apres ; ajouter une session longue pour la chauffe.

```powershell
./tools/collect_quest_performance.ps1 -DurationSeconds 120
# Mesure GPU facultative : redemarre le jeu pour activer les requetes GL.
./tools/collect_quest_performance.ps1 -DurationSeconds 120 -GpuTiming -RestartGame
```

Le script conserve logcat, summary.json et world-windows.csv. Il ne vide pas les logs. Il restaure la propriete de diagnostic apres capture ; redemarrer ensuite le jeu desactive aussi les requetes dans le processus existant. Les statistiques sont des fenetres de mesure, pas des percentiles de frames individuelles. Une frequence XR de 120 Hz ne prouve pas 120 nouvelles images 3D/s. Les compteurs absents restent inconnus.

Dans le menu de placement : frequence XR, resolution effective par oeil, nouvelles images 3D/s, CPU/GPU et images en retard. OVR Metrics Tool et RenderDoc restent necessaires pour localiser les passes couteuses et mesurer la consommation memoire.

### Comparaisons A/B sur casque

Deux proprietes de diagnostic, absentes par defaut (comportement inchange), servent a trancher entre les postes probables. Le script les pose, note les reglages dans `settings.json` et restaure les valeurs precedentes.

- `debug.partyboard.render_hz` plafonne les nouvelles images du jeu (lu toutes les 2 s). L'affichage XR garde sa frequence et remontre la derniere image avec ses poses. Hypothese a verifier : le thread du jeu, qui simule et enregistre les deux yeux, vise 120 images/s et plafonne sous 60 sur le plateau Toad.
- `debug.partyboard.instanced_stereo` (lu au demarrage) choisit le dessin des yeux. `0` : un draw par oeil. `1` : un draw pour les deux yeux, coupe par des clip distances. C'est ce pipeline qu'Adreno refuse (VK_ERROR_UNKNOWN), et Dawn en fait une perte d'appareil. `2`, la valeur par defaut : un draw pour les deux yeux, coupe par un test en fragments. La distance signee au bord de l'oeil passe en varying, sans fonctionnalite Vulkan particuliere. Sur PC, les deux variantes donnent la meme image que le rendu par oeil (`tools/test_quest_stereo_render.ps1`, 0 pixel different). Le cout du `discard` (LRZ d'Adreno desactive pour ces draws) face au gain sur le nombre de draws reste a mesurer.

```powershell
# Meme parcours, trois fois chacun, apres chauffe :
./tools/collect_quest_performance.ps1 -DurationSeconds 120               # reference
./tools/collect_quest_performance.ps1 -DurationSeconds 120 -RenderHz 72  # cadence 72
./tools/collect_quest_performance.ps1 -DurationSeconds 120 -RenderHz 60  # cadence 60
./tools/collect_quest_performance.ps1 -DurationSeconds 120 -RestartGame -InstancedStereo 0  # un draw par oeil
./tools/collect_quest_performance.ps1 -DurationSeconds 120 -RestartGame -InstancedStereo 2  # un draw pour les deux
```

- `debug.partyboard.stereo_msaa` (lu au demarrage) regle le multisampling des yeux : `1` (aucun) ou 4 par defaut. WebGPU n'accepte que 1 ou 4 echantillons ; le 2x faisait planter le jeu a la premiere image des yeux. Sur l'Adreno 740, qui rend par tuiles, le 4x prend quatre fois plus de memoire de tuile par pixel : les tuiles sont plus petites et chaque draw est retraite dans chaque tuile qu'il touche. Option du script : `-StereoMsaa 1 -RestartGame`.
- Les notifications de performance du casque (`XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT` : composition, rendu, thermique ; normal, warning, impaired) sont journalisees (`Perf settings:`) et reprises dans `summary.json` (`perf_events`).

### Mesures du 28/09 apres-midi (build 100, Quest 3, 120 Hz, plateau Toad)

- Le GPU est occupe a 85-93 % (jusqu'a 98 %), le plus souvent au niveau 2. Les coeurs CPU sont a 30-54 % et le thread du jeu a environ 4,5 ms par image. Environ 13-15 ms de GPU par image, presque independantes de la resolution. Le compositeur prend environ 2 ms par image affichee, soit pres d'un quart du GPU a 120 Hz.
- Avec une cible a 120 i/s : 62-92 i/s et 50-80 images en retard par 5 s, la resolution bloquee a 65 % (controleur corrige depuis). Plafonne a 60 i/s : 60 stables, 0-2 images en retard, 75-80 % de resolution.
- Dans la vue la plus chargee (430-500 draws par oeil), a 60 i/s : un draw pour les deux yeux 57,2 i/s, un draw par oeil 55,0 i/s. L'essentiel du gain depuis le matin (44 i/s a 414 draws, build 89) vient d'ailleurs, probablement de la passe de l'ecran plat 4K qui n'est plus dessinee.

Depuis ces mesures :

- `debug.partyboard.render_hz` non defini : la moitie de la frequence de l'ecran a partir de 110 Hz (60 i/s a 120 Hz, la cadence de la simulation), la frequence de l'ecran en dessous. `0` : la frequence de l'ecran.
- `debug.partyboard.gpu_level=boost` (lu au debut de la session) demande le niveau boost pour le GPU, au lieu de « soutenu eleve ». A surveiller : `Perf settings:` (thermique) et le niveau GPU dans les lignes VrApi.
- `debug.partyboard.layer_filter` (`normal` ou `none`, lu au premier affichage) allege le filtre du compositeur sur le calque des yeux, pour mesurer son cout (`compositor/gpu_frametime`).
- `debug.partyboard.display_hz` (72, 80, 90 ou 120, lu au debut de la session XR) choisit la frequence de l'ecran parmi celles du casque ; non defini : la plus rapide. En dessous de 110 Hz, le jeu rend a cette frequence et chaque image affichee est nouvelle. A 120 Hz, chaque image du jeu est montree deux fois : le compositeur corrige la rotation de la tete mais pas sa translation, et un plateau proche saccade quand la tete se deplace. A 72 Hz le compositeur fait aussi moitie moins de travail. Ligne du journal : `Display rate`. Option du script : `-DisplayHz 72 -RestartGame`.
- `-GpuMetrics` enregistre les compteurs Adreno d'`ovrgpuprofiler` (`gpu-metrics.txt`, une valeur par seconde) pendant la capture.

`summary.json` ajoute `game` et `game_by_scene` (images/s du thread du jeu, temps d'enregistrement, saccades), `draws` (draws du monde par oeil, part instanciee) et `memory` (memoire residente, memoire disponible du systeme, croissance sur la capture ; ligne `Memory:` du journal toutes les 5 s). La memoire residente n'est pas le PSS : elle suit la croissance, `dumpsys meminfo` reste la reference ponctuelle.

Chaque baisse de resolution est desormais un essai. La fenetre suivante, qui recree les images, est ignoree. La fenetre d'apres doit livrer au moins 5 % d'images en plus (plus une), sinon la resolution precedente revient et reste 30 s. Le journal l'indique (`restored: fewer pixels brought no more images`). Mesure du 28/09 (build 89, Quest 3, 72 Hz, plateau Toad) : a 414 draws par oeil, la cadence restait a 43-45 images/s de 80 % a 70 %, avec un GPU a 88-92 % et un thread du jeu a 5-6 ms. La limite venait du nombre de draws, pas des pixels. Les compteurs `app/gpu_frametime` et `compositor/gpu_frametime` ne voient pas le rendu Vulkan du jeu.

Un changement de resolution ne recree plus forcement les images des yeux. Les recreer vide d'abord l'anneau : le casque remontre la derniere image pendant quelques images (une saccade du plateau), puis le jeu refait ses cibles de rendu, dont les tampons MSAA 4x. Une taille plus petite est dessinee tout de suite dans une partie des images existantes (a gauche de chaque moitie, centree verticalement, comme Aurora le faisait deja). Un essai de baisse annule ne coute donc aucune nouvelle image. Les images ne retrecissent qu'une fois la taille tenue 20 s, et seulement si plus de 15 % de leurs pixels restent inutilises : le GPU efface et ecrit les images entieres. Une taille plus grande demande de nouvelles images tout de suite. Journal : `images now ... (were ...), N resizes since start` (regle dans `ImageSizePolicy`, `adaptive_quality.hpp`).

Sur casque, les images du jeu demarrent maintenant au rythme de l'ecran. Avant, le jeu suivait sa propre horloge a 60 Hz, qui glisse par rapport aux 120 Hz du casque. Une image terminee juste au moment ou le thread XR en cherche une restait affichee une trame, la suivante trois. Les poses des yeux, prevues pour le delai moyen, se trompaient alors d'une trame pour les deux images : le plateau saccadait, surtout quand la tete bouge. Desormais, chaque image demarre a un temps fixe apres un de ces passages du thread XR. Ce temps est choisi d'apres l'heure reelle de fin du GPU (horodatage de la fence) : 98 % des images finissent 1,5 ms avant leur passage prevu. Aucune image n'est montree avant ce passage. Mesure du build 112 (Quest 3, plateau Toad, 60 s chacun), ou les images etaient encore montrees des qu'elles etaient finies : sans calage, 17,3 % des images hors rythme et 27,6 ms de latence ; avec calage, 11,3 % et 25,0 ms. Du debut de l'image a la fin du GPU, il s'ecoulait de 18 a 26 ms selon la seconde (95e centile). Ce temps varie de plus d'une trame : une image finie tot passait une trame trop tot. Une simulation (`tools/test_quest_quality.ps1`) reproduit les deux cas. Sans calage contre un ecran a 119,88 Hz, ou avec calage mais sans attendre le passage prevu, des images restent 1 ou 3 trames. En attendant ce passage, toutes en restent 2. Pas en netplay. Pour un A/B sans redemarrer : `adb shell setprop debug.partyboard.xr_pacing 0` (lu toutes les 2 s). Dans le journal `Stereo perf` : `holds=a/b/c/d` (images restees 1, 2, 3, 4 trames ou plus), `latencyMin`/`latencyMax`, `paced`, `paceWork` (95e centile du debut de l'image a la fin du GPU), `paceLooks` et `pacePhase`. `summary.json` en tire `off_cadence_percent`, la part des images hors du rythme habituel de la fenetre.

### Profondeur pour le compositeur (piste E) : conception, pas encore faite

A 120 Hz, chaque image du jeu est montree deux fois. La seconde fois, le compositeur ne corrige que la rotation de la tete. Il faudrait la profondeur des yeux pour corriger aussi la translation (`XR_KHR_composition_layer_depth`), et en plus des vecteurs de mouvement pour Application SpaceWarp (`XR_FB_space_warp`). Le journal indique au demarrage ce que le casque propose (`Reprojection: depth submission ..., space warp ...`).

Ce que ca demanderait :

1. Aujourd'hui, la profondeur des yeux (Depth32Float, MSAA 4x) est transitoire : elle ne sort jamais de la memoire de tuile. L'ecrire en memoire couterait environ 95 Mo par image, et WebGPU ne sait pas resoudre une profondeur. La voie raisonnable : une seconde sortie couleur R16Float dans les shaders GX des yeux (profondeur lineaire, resolue comme la couleur), soit 2 octets par pixel en plus. Il faut toucher `lib/gx/shader.cpp`, les pipelines stereo (deux cibles) et leur cle de cache.
2. Une seconde image partagee par emplacement de l'anneau (AHardwareBuffer R16F), importee par Aurora comme les images couleur.
3. Cote XR, une swapchain de profondeur (`GL_DEPTH_COMPONENT16`) et une passe GL qui ecrit `gl_FragDepth` depuis l'image R16F. Une copie directe est impossible entre couleur et profondeur. Cout estime : 0,2 a 0,4 ms de GPU par image.
4. `XrCompositionLayerDepthInfoKHR` chaine a chaque vue, avec `nearZ`/`farZ` en metres (`kNear`/`kFar` de `stereo_view.cpp`, l'espace des yeux etant en metres de la piece). Les pixels sans monde (la piece en transparence) restent au plus loin.

Critere de decision : d'abord mesurer `-DisplayHz 72` (chaque image montree une seule fois, donc aucune translation a corriger) avec le calage sur l'ecran. On ne lance E que si 72 i/s ne tiennent pas sur le plateau et qu'a 120 Hz la saccade en translation reste visible avec des `holds` reguliers.

La generation des mipmaps du monde (thread du jeu, a chaque chargement ou mise a jour de texture) n'appelle plus `pow` par texel : table construite avec le `pow` de la plateforme, et chemin 2x2 pour les tailles paires. Les octets sont identiques, ce que verifient `tools/test_quest_quality.ps1` (encodage compare autour de chaque seuil, textures comparees a `tools/tests/rgba_mips_reference.hpp`) et une compilation clang avec FMA. Sur PC, une texture 512x512 passe d'environ 5,5 ms a 2 ms ; le gain sur Quest reste a mesurer.

## Verification locale

```powershell
./tools/test_quest_quality.ps1
py -3 tools/tests/test_quest_performance.py
./tools/test_quest_stereo.ps1 -CompileOnly
# Avec casque : execute aussi les tests de cycle de vie.
./tools/test_quest_stereo.ps1
```

Le moteur Android ARM64 compile, l'APK Quest assemble et les tests JVM passent. Les decisions GPU/CPU, bornes, fraicheur des mesures et mipmaps passent les tests locaux. La pile de patches Aurora est appliquee dans l'ordre par CI, avec aurora-quest-quality.patch apres aurora-android-surface-generation.patch. Les modifications du sous-module sont distribuees par ces patches.

Les mesures sur casque sont dans les sections plus haut (builds 100 et 112, Quest 3). La liste suivante dit ce qui reste a verifier ou a faire.

## Ce qui reste a faire

Etat au 28/09/2026, apres le build 112 (branche `quest/lrz-uncut`). Chaque point se mesure seul, sur le meme parcours (plateau Toad, scene 89), avec `tools/collect_quest_performance.ps1`.

### 1. A valider sur casque

1. **Calage sur l'ecran avec passage prevu** (build 113, 6921e395). A/B avec `debug.partyboard.xr_pacing` sur le plateau. Objectif : `off_cadence_percent` sous 3 % (11,3 % au build 112, 17,3 % sans calage), une latence stable et, au ressenti, plus de saccade quand la tete bouge.
2. **Stabilite** :
   - retour au jeu apres une longue pause (le build 109 n'a ete teste que sur cinq retours courts) ;
   - plantage de m440 (SIGSEGV, build 88), a reproduire ; son rapport n'est pas lisible sans acces root ;
   - parcours plateau, m428, retour au plateau, en suivant la memoire (`Memory:` : 611 a 634 Mo residents sur le plateau au build 112).
3. **Controles visuels**, dont ceux de 81bbc211 (fusionne au build 114) :
   - bords des yeux avec le mode sans coupe (`StereoUncut`) ;
   - plateau fixe : le recentrage sur le joueur actif est retire, il donnait la nausee ;
   - HUD sur un ecran de stade au-dessus du plateau ;
   - mini-jeux : une maquette inclinee a la place de l'ecran, vue depuis leur camera (le grip gauche bascule ce mode) ;
   - table du scan de la piece : le 28/09, la piece n'avait aucune table scannee (`Room scan: 0 table(s)`). A refaire apres en avoir ajoute une dans Space Setup.
4. **Hauteur des mini-jeux par rapport a la table**, seulement quand ils sont poses sur la table (trop hauts ou trop bas). Le sol est mesure sur la geometrie des 20 premieres images (`PartyBoard_StereoObserveBounds`). Piste : la hauteur des pieds des personnages. Ils sont dans `charWork[]`, statique dans `src/game/chrman.c` ; il faudrait un accesseur sous `TARGET_PC`.

### 2. Alleger le GPU, le poste dominant

Ce qu'on sait :
- 92 a 93 % du temps GPU sont passes sur les fragments, avec 6 a 7 fragments par pixel.
- Le GPU reste au niveau 2 (456 a 640 MHz) ; le niveau boost ne change rien.
- A 72 Hz, le plateau tombe a 49-72 i/s, et baisser la resolution a 90 % n'y fait rien.

5. **Trace par etape de rendu et par draw**, avant de choisir entre A, B et C. Commandes : `ovrgpuprofiler -t 0.25 --renderstage-metrics=...`, puis `-x`, en mode detaille (`ovrgpuprofiler -e`, a desactiver ensuite avec `-d`). La session du 28/09 a ete coupee par une deconnexion du casque.
6. **A : demi-precision (f16)** dans les shaders GX, pour les calculs de couleur. Il faut la fonctionnalite `ShaderF16` de Dawn, que `lib/webgpu/gpu.cpp` ne demande pas aujourd'hui. Il faut aussi comparer l'image au rendu actuel (`tools/test_quest_stereo_render.ps1`, avec une tolerance).
7. **B : textures compressees**. Sur casque, toutes les textures couleur, CMPR compris, sont decodees en RGBA8 (`lib/gfx/texture.cpp`), soit huit fois la taille du CMPR. Adreno accepte l'ASTC (`g_astcTexturesSupported`). Piste : transcoder au chargement, sur le CPU ou par un calcul GPU. A mesurer : bande passante (`ovrgpuprofiler`), memoire et temps de chargement.
8. **C : surdessin**. Trier les draws opaques de l'avant vers l'arriere quand l'ordre du jeu le permet. La coupe des yeux par `discard` desactive le LRZ d'Adreno ; le mode sans coupe a deja retire environ 6 % des fragments par pixel. Il faudrait l'etendre aux objets qui touchent les deux yeux sans passer par un test en fragments.
9. **Compositeur** : 1,5 a 2,3 ms de GPU par image affichee a 120 Hz (`compositor/gpu_frametime`). L'A/B `debug.partyboard.layer_filter` (`normal`, `none`) reste a mesurer.
10. **Attente du compositeur derriere le jeu**. La prediction du runtime (`Prd` dans les lignes VrApi) passe de 18 ms dans les menus a 34 ms des que le plateau est dessine : les copies GL et le compositeur attendent le rendu du jeu. Pistes : decouper le rendu des yeux en plusieurs soumissions, ou une priorite de file Vulkan si Dawn l'expose.

### 3. Ensuite

11. **72 Hz** (`debug.partyboard.display_hz 72`), a remesurer apres les points 6 a 8. Il faut 72 i/s tenus sur le plateau. Chaque image n'est alors montree qu'une fois : il n'y a plus de translation de la tete a corriger.
12. **E, la profondeur pour le compositeur** : conception plus haut. A lancer seulement si son critere est rempli.
13. **Mini-jeux au cas par cas**, avec des captures reproductibles. Ne pas modifier le temps de simulation pour obtenir une frequence d'affichage plus haute.
14. **Fusion de `quest/lrz-uncut` dans `audio-local`**, par une PR, sur decision de l'utilisateur. `origin/audio-local` s'arrete a a1e1ada8 (PR #7 fusionnee en partie).

### 4. Long terme : changement de backend

- Une integration Dawn/Vulkan qui cible directement les images OpenXR, a la place des buffers Android partages et des copies GL. La bibliotheque Dawn precompilee actuelle ne fournit pas le chemin d'import/export necessaire.
- Le vrai multiview et la foveation dans ce backend, avec detection des capacites et repli Quest 2/3. La foveation doit porter sur la passe 3D couteuse, pas sur une copie de presentation.
- Une reconstruction temporelle, seulement avec des vecteurs de mouvement et un historique coherents (GX n'en fournit pas). Pas d'upscaling IA promis sans implementation ni mesure de cout.

## Sources Meta du plan

- [OVR Metrics Tool](https://developers.meta.com/horizon/documentation/native/android/ts-ovrmetricstool/)
- [RenderDoc et Render Stage](https://developers.meta.com/horizon/documentation/native/android/ts-renderdoc-renderstage/)
- [Analyse MSAA](https://developers.meta.com/horizon/documentation/native/android/mobile-msaa-analysis/)
- [Super-resolution et qualite](https://developers.meta.com/horizon/blog/vr-image-quality-meta-quest-super-resolution/)
- [Multiview](https://developers.meta.com/horizon/documentation/unity/enable-multiview/)
- [Foveation fixe native](https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/)
- [Projection symetrique](https://developers.meta.com/horizon/documentation/native/android/os-symmetric-projection/)
