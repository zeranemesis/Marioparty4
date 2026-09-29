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

## Lot 1 : alleger les fragments (branche `quest/gpu-lot1`)

Ajoute sans casque, verifie sur PC. Rien n'a encore tourne sur le casque.

- **Moins de calcul par pixel, image identique.** Le generateur GX n'emule plus le debordement 8 bits (`tev_overflow_*`) que sur les operandes lus dans un registre TEV (`prev`, `tevreg0-2`). Textures, couleurs rasterisees et constantes sont deja des valeurs 8 bits dans [0, 1], ou l'emulation ne change rien. L'emulation finale de `prev` est omise quand les derniers etages qui l'ecrivent bornent leur resultat. Une texture lue aux memes coordonnees par un etage precedent n'est pas relue. Retour a l'ancien comportement : `debug.partyboard.tev_overflow=all` (lu au premier shader, donc au demarrage). `tools/test_quest_stereo_render.ps1` compare l'ancien et le nouveau shader sur une scene ou la couleur de sommet passe par l'emulation : 0 pixel different.
- **Tri avant vers arriere des draws opaques** (`debug.partyboard.sort_opaque 1`, relu toutes les 2 s). Chaque objet de `ObjDraw` donne sa distance au milieu des yeux (`AuroraStereoSetSortKey`). Une suite de draws opaques, testes et ecrits en profondeur, est envoyee de l'avant vers l'arriere, pour le rejet precoce d'Adreno (LRZ). Les draws d'un objet restent groupes et dans l'ordre ; les draws melanges, hors objet, et la passe arriere-avant du jeu ne bougent pas. Le tri se fait sur les draws enregistres, dont sommets et uniformes sont deja figes. Risque a verifier a l'oeil : un decor coplanaire dessine par un autre objet (marquage au sol) pourrait passer sous le sol.
- **Objets a cheval sur les deux yeux dessines par oeil** (`debug.partyboard.stereo_crossing 1`, relu toutes les 2 s) au lieu d'un draw instancie coupe par `discard`, qui prive l'Adreno du LRZ.
- **Filtre du compositeur** (`debug.partyboard.layer_filter`) relu toutes les 2 s.
- **Campagne en une session** : `tools/quest_campaign.ps1`, le jeu sur la vue a mesurer, sept phases de 45 s sans redemarrer (reference, tri, cheval, les deux, filtre normal, filtre aucun, reference). Tableau par phase dans `build/quest-campaign/<date>/campaign.csv`.

Session casque proposee :

```powershell
# Le jeu sur le plateau Toad, immobile, meme vue ; environ 6 minutes :
./tools/quest_campaign.ps1
# Puis la trace GPU (5) sur la meilleure combinaison, et l'A/B de l'emulation TEV
# (redemarrage : adb shell setprop debug.partyboard.tev_overflow all).
```

Repousses tant que la trace GPU ne les justifie pas : demi-precision (A) et textures ETC2/ASTC (B). La recompression baisse la nettete de textures deja compressees une fois ; a ne faire que si la trace montre que la bande passante des textures limite.

## Mesures du 28/09 au soir (builds 117-119, plateau Toad, yeux a 95 %)

- Par defaut desormais : tri avant vers arriere des objets opaques, objets a cheval dessines par oeil, emulation TEV reduite. Contre l'ancien comportement : 50 M fragments par image au lieu de 78 M, GPU 71 % au lieu de 81 %, 0 saccade au lieu de 12, 60 ms de latence au lieu de 66.
- Sans effet : priorite haute du contexte de copie (la latence suit la charge GPU : 17 ms en menus, 53 a 68 ms sur le plateau), niveau GPU boost (reste a 640 MHz). Melange coupe : pas de gain net avec le tri, il reste actif par defaut (`debug.partyboard.opaque_blend off` pour le couper).
- La charge varie de 40 % avec la scene (meme reglage : 50 M puis 71 M) : une comparaison fiable demande des phases courtes alternees.
- Resolution 80 % : 37 M fragments, GPU 51 %, 50 ms. 110 % : trop lourd. MSAA 1x : -20 % de fragments, crenelage visible.

Plan suivant :
1. Campagne alternee (A/B/A/B, phases de 20 s) pour chaque reglage restant.
2. 72 Hz natif (`debug.partyboard.display_hz 72`) maintenant que le GPU a de la marge.
3. Plus de nettete : remonter la resolution par defaut si 72 ou 120 Hz tient.
4. Reprise apres Space Setup : verifier la ligne `Layers:` et la garde "table sous le sol".
## Ce qui reste a faire

Etat au 29/09/2026, apres le build 119 (`audio-local`, c436ba53). Chaque point se mesure seul, sur le meme parcours (plateau Toad, scene 89) : `tools/quest_campaign.ps1` pour les interrupteurs, `tools/collect_quest_performance.ps1` pour une capture simple.

### Acquis (mesures des 28/09, builds 100 a 119)

- Plateau Toad, yeux a 95 % : 78 M puis 50 M de fragments par image, GPU 81 % puis 71 %, 12 puis 0 saccade du jeu (tri avant vers arriere, objets a cheval par oeil, TEV reduit).
- Resolution : les changements ne recreent plus les images des yeux (0 recreation sur 13 changements, build 112).
- Calage sur l'ecran : 17,3 % d'images hors rythme sans calage, 11,3 % avec (build 112), avant le passage prevu du build 113.
- Sans effet, ne pas y revenir : niveau GPU boost, priorite haute du contexte de copie, melange coupe, 110 % de resolution.

### 1. Session casque (build 119), dans cet ordre

1. **Calage** : `off_cadence_percent` et latence, avec `debug.partyboard.xr_pacing` et sans. Objectif : moins de 3 % d'images hors rythme. Non mesure depuis le build 113.
2. **72 Hz** (`debug.partyboard.display_hz 72`, avec `-RestartGame`) sur deux scenes : le plateau a 50 M de fragments et une scene plus lourde (71 M). Pour chacune, la resolution a 95 % puis a 80 % (`debug.partyboard.eye_scale`). Relever images/s du jeu, saccades, GPU occupe, latence. Estimation a confirmer : a 50 M le jeu prend environ 8 ms par image, soit un GPU vers 71 % a 72 i/s ; a 71 M, environ 11,4 ms, soit un GPU vers 96 %.
3. **Confort** : la question qui decide. Le mal des transports disparait-il a 72 Hz, meme a 80 % ? Sinon, le 72 Hz ne sert a rien et on passe a la profondeur pour le compositeur (piste E).
4. **Trace GPU** (`tools/quest_gpu_trace.ps1`), sur la meilleure combinaison. Elle dit ou vont les millisecondes : fragments, sommets, bande passante des textures, nombre de draws. Aucun echantillon de sa sortie n'existe encore : l'analyse se fera sur la premiere capture reelle.

Decision apres 2 et 3 :

| Resultat | Suite |
| --- | --- |
| 72 Hz tient sur les deux scenes | reglage par defaut, puis remonter la resolution |
| 72 Hz tient sur les scenes legeres seulement | 72 Hz pour les mini-jeux, 120 Hz avec 60 i/s sur le plateau lourd, changement de frequence au changement de scene |
| 72 Hz ne tient pas | leviers du point 2, selon la trace |
| 72 Hz ne regle pas le confort | piste E |

### 2. Optimisation qui reste, selon la trace

Ce qu'on sait : le GPU passe 92 a 93 % de son temps sur les fragments (build 100). Or 13 a 15 ms par image y etaient presque independantes de la resolution : le cout n'est pas que le nombre de pixels, d'ou la trace avant de choisir.

5. **Nombre de draws** : 414 a 500 par oeil sur le plateau, et le GPU a tuiles retraite chaque draw dans chaque tuile qu'il touche. Piste : fusionner les draws successifs de meme etat (meme pipeline, textures et uniformes).
6. **A : demi-precision (f16)** pour les calculs de couleur. Il faut la fonctionnalite `ShaderF16` de Dawn, que `lib/webgpu/gpu.cpp` ne demande pas, et comparer l'image au rendu actuel (`tools/test_quest_stereo_render.ps1`, avec une tolerance).
7. **B : textures compressees**. Toutes les textures couleur, CMPR compris, sont decodees en RGBA8 sur casque (`lib/gfx/texture.cpp`), soit huit fois la taille du CMPR. Adreno accepte l'ASTC. A ne faire que si la trace montre que la bande passante des textures limite : recompresser des textures deja compressees en baisse la nettete.
8. **Compositeur** : 1,5 a 2,3 ms de GPU par image affichee a 120 Hz (`compositor/gpu_frametime`). L'A/B `debug.partyboard.layer_filter` (`normal`, `none`) est dans `tools/quest_campaign.ps1`.
9. **Latence sur le plateau** : 53 a 68 ms contre 17 ms dans les menus. Elle suit la charge GPU. Piste : decouper le rendu des yeux en plusieurs soumissions, ou une priorite de file Vulkan si Dawn l'expose.
10. **Nettete** : remonter la resolution par defaut (95 % aujourd'hui) si 72 ou 120 Hz tient. Le MSAA 1x retire 20 % de fragments mais le crenelage se voit.

### 3. Stabilite et contenu

11. Retour au jeu apres une longue pause (le build 109 n'a ete teste que sur cinq retours courts), et reprise apres Space Setup : ligne `Layers:` et garde « table sous le sol ».
12. Plantage de m440 (SIGSEGV, build 88), a reproduire ; son rapport n'est pas lisible sans acces root.
13. Parcours plateau, m428, retour au plateau, en suivant la memoire (`Memory:` : 611 a 634 Mo residents sur le plateau au build 112).
14. Controles visuels : bords des yeux avec le mode sans coupe, plateau fixe, HUD en ecran de stade, mini-jeux en maquette inclinee (le grip gauche bascule ce mode), table du scan de la piece (aucune table scannee le 28/09).
15. Hauteur des mini-jeux quand ils sont poses sur la table. Le sol est mesure sur la geometrie des 20 premieres images (`PartyBoard_StereoObserveBounds`). Piste : la hauteur des pieds des personnages, dans `charWork[]` (`src/game/chrman.c`), qui demande un accesseur sous `TARGET_PC`.
16. Mini-jeux au cas par cas, avec des captures reproductibles. Ne pas modifier le temps de simulation pour obtenir une frequence d'affichage plus haute.

### 4. Long terme : changement de backend

- Une integration Dawn/Vulkan qui cible directement les images OpenXR, a la place des buffers Android partages et des copies GL. La bibliotheque Dawn precompilee actuelle ne fournit pas le chemin d'import/export necessaire.
- Le vrai multiview et la foveation dans ce backend, avec detection des capacites et repli Quest 2/3. La foveation doit porter sur la passe 3D couteuse, pas sur une copie de presentation.
- La profondeur pour le compositeur (piste E, conception plus haut), si le 72 Hz ne suffit pas.
- Une reconstruction temporelle, seulement avec des vecteurs de mouvement et un historique coherents (GX n'en fournit pas). Pas d'upscaling IA promis sans implementation ni mesure de cout.

## Sources Meta du plan

- [OVR Metrics Tool](https://developers.meta.com/horizon/documentation/native/android/ts-ovrmetricstool/)
- [RenderDoc et Render Stage](https://developers.meta.com/horizon/documentation/native/android/ts-renderdoc-renderstage/)
- [Analyse MSAA](https://developers.meta.com/horizon/documentation/native/android/mobile-msaa-analysis/)
- [Super-resolution et qualite](https://developers.meta.com/horizon/blog/vr-image-quality-meta-quest-super-resolution/)
- [Multiview](https://developers.meta.com/horizon/documentation/unity/enable-multiview/)
- [Foveation fixe native](https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/)
- [Projection symetrique](https://developers.meta.com/horizon/documentation/native/android/os-symmetric-projection/)
