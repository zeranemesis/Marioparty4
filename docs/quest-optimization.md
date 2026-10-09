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

### Reprojection de la translation (piste E) : conception, pas encore faite

A 120 Hz, chaque image du jeu est montree deux fois. La seconde fois, le compositeur ne corrige que la rotation de la tete : le plateau saccade quand la tete se deplace.

**Correction du 29/09.** Ce document disait que soumettre la profondeur des yeux (`XR_KHR_composition_layer_depth`) suffisait a corriger aussi la translation. La documentation de Meta dit le contraire : sur Quest, la correction de position (Positional TimeWarp) n'existe qu'avec Application SpaceWarp (AppSW), qui demande la profondeur et des vecteurs de mouvement (`XR_FB_space_warp`). La profondeur seule ne change rien. En echange, AppSW fait rendre le jeu a la moitie de la frequence de l'ecran et synthetise les images intermediaires, ce qui est deja notre cas a 120 Hz (60 i/s) et ferait passer le 72 Hz a 36 i/s. Limites annoncees par Meta : transparences et particules ambigues, distorsions aux rotations rapides, un seul calque pris en charge (le HUD reste hors AppSW), latence des manettes.

Le journal indique au demarrage ce que le casque propose (`Reprojection: depth submission ..., space warp ...`). Seule la ligne `space warp` compte.

Ce que ca demanderait :

1. Une seconde et une troisieme sortie dans les shaders GX des yeux : la profondeur lineaire et le vecteur de mouvement de chaque pixel. Aujourd'hui la profondeur (Depth32Float, MSAA 4x) est transitoire, elle ne sort jamais de la memoire de tuile, et WebGPU ne sait pas resoudre une profondeur. Il faut toucher `lib/gx/shader.cpp`, les pipelines stereo (plusieurs cibles) et leur cle de cache.
2. Le vecteur de mouvement vient de la position de chaque sommet a l'image precedente : le jeu garde deja les transformations precedente et courante de ses objets pour l'interpolation au-dela de 60 Hz (`PartyBoard_FrameInterpolation`). Les objets animes par squelette ou par sommets, et les particules, demandent un traitement a part.
3. Des images partagees pour ces sorties (AHardwareBuffer) par emplacement de l'anneau, importees par Aurora comme les images couleur.
4. Cote XR, les swapchains de profondeur et de mouvement, la copie GL vers elles, et `XrCompositionLayerSpaceWarpInfoFB` chaine a chaque vue. Le calque a besoin de ses poses et de son champ de vision pour la reprojection.

Critere de decision : d'abord mesurer `-DisplayHz 72` sur le confort. On ne lance E que si le 72 Hz ne tient pas ou ne suffit pas, et si le journal annonce `space warp offered`. Effort : de l'ordre de plusieurs jours, avec des risques visuels (artefacts) qu'on ne juge que sur casque.

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
## Plan d'optimisation (29/09/2026, apres le build 119)

Quest 3, plateau Toad (scene 89), sauf mention. Les gains **mesures** viennent des captures sur casque. Les gains **estimes** sont des ordres de grandeur a verifier : chaque ligne dit sur quoi repose l'estimation.

### 1. Mesures cles

| Build | Mesure | Resultat |
| --- | --- | --- |
| 88 | 120 Hz | 47-57 i/s a 65 % de resolution (objectif 60) |
| 89 | 72 Hz, 414 draws par oeil | 43-45 i/s, GPU 88-92 % ; de 80 a 70 % de resolution, rien ne change |
| 100 | 120 Hz, jeu plafonne a 60 i/s | 60 i/s stables, 0-2 images en retard par 5 s, 75-80 % de resolution ; 13-15 ms de GPU par image, presque independantes de la resolution ; compositeur environ 2 ms par image affichee |
| 100 | GPU | 92-93 % du temps sur les fragments, 6 a 7 fragments par pixel ; niveau 2, 640 MHz au plus |
| 110 | Mode sans coupe des yeux | -6 % de fragments par pixel |
| 112 | Calage sur l'ecran | 17,3 % puis 11,3 % d'images hors rythme ; latence des yeux 27,6 puis 25,0 ms |
| 112 | Changements de resolution | 13 changements, 0 recreation des images |
| 112 | 72 Hz | 49-72 i/s, 27 % d'images hors rythme, GPU 456-640 MHz |
| 117-119 | Lot 1 (tri avant vers arriere, objets a cheval par oeil, TEV reduit), yeux a 95 % | 78 M puis 50 M de fragments par image, GPU 81 puis 71 %, 12 puis 0 saccade, latence 66 puis 60 ms |
| 117-119 | Resolution 80 % | 37 M de fragments, GPU 51 % |
| 117-119 | MSAA 1x | -20 % de fragments, crenelage visible (refuse) |
| 117-119 | Sans effet | niveau GPU boost (reste a 640 MHz), priorite haute de la copie, melange coupe, 110 % de resolution |
| 117-119 | Variation selon la scene | meme reglage : 50 M puis 71 M de fragments (+40 %) |

### 2. Ce que les mesures disent

1. Le GPU limite, et dans le GPU, les fragments. Le lot 1 l'a confirme : -36 % de fragments ont donne -10 points de GPU et plus aucune saccade.
2. Le cout ne depend pas que du nombre de pixels : 13-15 ms par image presque independantes de la resolution (build 100), rien de gagne de 80 a 70 % (build 89). Le reste vient probablement du nombre de draws (414 a 500 par oeil, chacun retraite dans chaque tuile touchee) ou de la lecture des textures. La trace GPU doit trancher.
3. Le GPU ne montera pas plus haut en frequence : le boost ne change rien. Tout gain vient de travail en moins.
4. La charge varie de 40 % selon la scene : une mesure seule ne vaut rien, il faut des phases courtes alternees (A/B/A/B de 20 s), resolution figee (`debug.partyboard.eye_scale`), meme vue.
5. La latence suit la charge du GPU : 17 ms en menus, 53 a 68 ms sur le plateau (campagne du 28/09).
6. Le compositeur coute 1,5 a 2,3 ms par image affichee : environ un quart du GPU a 120 Hz, environ 14 % a 72 Hz.

### 3. Objectif chiffre

- **72 Hz** : 13,9 ms par trame, compositeur compris. Le jeu doit tenir sous environ 10 ms de GPU par image pour garder de la marge.
- Estimation, en supposant que les 71 % du build 119 comprennent le compositeur (24 % a 120 Hz) : environ 7,8 ms par image du jeu sur la scene legere (50 M), soit un GPU vers 70 % a 72 Hz, qui tiendrait. Sur la scene lourde (71 M), environ 11 ms, soit un GPU vers 94 %, qui ne tiendrait pas sans environ 12 % de gain ou une resolution a 80 %.
- **120 Hz avec 60 i/s** : deja tenu. Reste a rendre le rythme regulier, moins de 3 % d'images hors rythme.

### 4. Gains mesures, par optimisation

| Optimisation | Build | Gain mesure |
| --- | --- | --- |
| Ecran plat 4K plus dessine pendant la vue des yeux (cause probable) | 89 -> 100 | 44 -> 57 i/s dans la vue la plus chargee, environ +30 % |
| Jeu plafonne a 60 i/s a 120 Hz | 100 | images en retard 50-80 -> 0-2 par 5 s (environ -97 %), resolution 65 -> 75-80 % |
| Un draw pour les deux yeux au lieu d'un par oeil | 100 | 55,0 -> 57,2 i/s, environ +4 % |
| Mode sans coupe pour les objets d'un seul oeil | 110 | -6 % de fragments par pixel |
| Calage des images sur l'ecran | 112 | images hors rythme -35 % (17,3 -> 11,3 %), latence -9 % (27,6 -> 25,0 ms) |
| Pas de recreation des images a chaque changement de resolution | 112 | 0 recreation sur 13 changements (-100 % des gels lies) |
| Lot 1 : tri avant vers arriere, objets a cheval par oeil, TEV reduit | 117-119 | fragments -36 %, GPU -12 % (81 -> 71 %), saccades -100 % (12 -> 0), latence -9 % (66 -> 60 ms) |
| Resolution 80 % au lieu de 95 % (reglage, pas une optimisation) | 117-119 | fragments -26 %, GPU -28 % (71 -> 51 %), au prix de la nettete |
| Mipmaps du monde par table au lieu de `pow` | PC | texture 512x512 : 5,5 -> 2 ms (-64 %) au chargement ; non mesure sur casque |

### 5. Gains estimes, par optimisation restante

Pourcentages du temps GPU par image du jeu au build 119 (yeux a 95 %), sauf mention. A confirmer par la trace GPU puis par une mesure A/B.

| Optimisation | Gain estime | Sur quoi repose l'estimation | Effort | Etat |
| --- | --- | --- | --- | --- |
| HUD dans une image a lui, hors de la passe MSAA des yeux | passe des yeux : 9,95 -> 5,91 M pixels (-41 %) ; pixels effaces et ecrits en tout : 9,95 -> 7,83 M par image (-21 %, -8,5 Mo par image) ; memoire des images partagees : 119 -> 94 Mo (-21 %). Environ 2 a 8 % de GPU | calcul des tailles (3360x2960 contre 3360x1760 et 1600x1200) ; le gain en temps depend du cout d'une tuile vide, a mesurer | fait | a mesurer sur casque |
| HUD dessine une image sur deux (30 Hz a 60 i/s, sa frequence de copie) | pixels effaces et ecrits par image : 7,83 -> 6,87 M en moyenne (-12 %), la moitie des draws du HUD ; environ 1 a 4 % de GPU | le HUD fait 1,92 M pixels a 4x, un quart des pixels restants | fait (`debug.partyboard.hud_rate full` pour le dessiner a chaque image) | a mesurer (phase `hud-every` de la campagne) |
| Pipelines des yeux gardes dans le cache | 0 % de GPU ; beaucoup moins d'objets absents a la premiere vue du plateau, demarrage plus long | les dessins sans pipeline sont abandonnes (`bind_pipeline`) | fait | bbe26248, a mesurer (`Pipelines:`) |
| Filtre du compositeur allege (`layer_filter normal`) | 3 a 10 % du GPU a 120 Hz, 2 a 6 % a 72 Hz | 1,5-2,3 ms par image affichee, dont une partie pour le filtre | fait (interrupteur) | a mesurer |
| Regroupement des draws | 5 a 20 % | 414 a 500 draws par oeil, cout presque independant de la resolution ; tres incertain. Aurora fusionne deja les draws consecutifs de meme etat (`command_processor.cpp`) : il faudrait regrouper des draws aux uniformes differents (uniformes par instance), un gros changement | fort (Aurora) | seulement si la trace le montre |
| Demi-precision (f16) des calculs du TEV (`debug.partyboard.shader_f16 1`, au demarrage) | 5 a 15 % | calcul a double debit en f16 sur Adreno, sur la part calcul des fragments seulement | fait (interrupteur, desactive par defaut) | verifie sur PC (section 8) ; a mesurer (phase `shader-f16` de la campagne, avec redemarrage) |
| Filtrage anisotrope 8x -> 4x (`debug.partyboard.anisotropy`, relu toutes les 2 s) | 0 a 8 % de GPU selon l'attente des textures, textures en biais un peu plus floues | le plateau est vu en biais, la ou le 8x lit le plus de texels ; 1x donne le cout maximal des textures | fait (interrupteur) | a mesurer (phases `aniso-4`, `aniso-1`) ; decide aussi de l'ASTC |
| Textures ASTC au lieu de RGBA8 | 0 a 15 % de GPU selon l'attente des textures ; memoire des textures -75 % (ASTC 4x4) | RGBA8 32 bits par texel contre 8 en ASTC 4x4 | moyen a fort, legere perte de nettete | seulement si la trace montre les textures |
| Decoupage du rendu des yeux en plusieurs soumissions | 0 % d'images/s ; latence -10 a -30 % | la latence suit la file du GPU | moyen | apres les images/s |
| 72 Hz au lieu de 120 Hz / 60 i/s | +20 % d'images neuves par seconde (60 -> 72), trame -17 % (16,7 -> 13,9 ms), compositeur -40 % (120 -> 72 compositions/s) ; GPU du jeu +20 % | arithmetique des frequences | fait (interrupteur) | a mesurer, et le confort a juger |
| AppSW (profondeur et vecteurs de mouvement) | GPU du jeu environ -50 % (rendu a la moitie de la frequence) ; Meta annonce jusqu'a +70 % de performance au mieux | documentation Meta | fort (plusieurs jours), artefacts possibles | seulement si le 72 Hz ne suffit pas |
| Resolution 95 -> 100 % (nettete) | cout : +11 % de pixels, environ +8 a 10 % de GPU | proportion pixels / fragments | reglage | a faire avec la marge gagnee |

Scenario, si fusion des draws, f16 et filtre donnent chacun le bas de leur fourchette (5 + 5 + 3 %, environ -13 % en tout) : la scene lourde passerait d'environ 94 % a environ 83 % de GPU a 72 Hz, a la limite. Avec le milieu des fourchettes (environ -25 %), vers 73 %, avec de la marge. Ce n'est qu'un ordre de grandeur.

### 6. Etapes

**Etape 1 - Session casque de validation (45 min, build 120).**

| Mesure | Objectif | Si c'est rate |
| --- | --- | --- |
| Deux lancements de suite, dessins abandonnes (`Pipelines:`) | beaucoup moins au second | revoir les pipelines conserves |
| Calage avec et sans (`debug.partyboard.xr_pacing`) | moins de 3 % d'images hors rythme | augmenter la marge du calage |
| 72 Hz sur la scene legere, 95 % puis 80 % | 72 i/s tenus 60 s | etape 2 |
| 72 Hz sur la scene lourde, 95 % puis 80 % | 72 i/s tenus 60 s | 72 Hz pour les scenes legeres seulement |
| HUD apart et une image sur deux (builds 121 et suivants) : HUD visible, `hudNew` vers 30, GPU contre `hud-every` | HUD identique, GPU en baisse | `debug.partyboard.hud_rate full`, puis revoir |
| `tools/quest_campaign.ps1` : phases `filter-normal`, `aniso-4`, `aniso-1`, `hud-every`, `pacing-off` contre la reference | chaque gain chiffre, sans perte de nettete visible | garder le reglage actuel |
| Ressenti du joueur a 72 Hz | plus de mal des transports | etape 4, AppSW |

**Etape 2 - Trace GPU (15 min de casque).** `tools/quest_gpu_trace.ps1` sur la meilleure combinaison de l'etape 1. Elle dit ou vont les millisecondes (calcul des pixels, sommets, textures, draws) et fixe l'ordre de l'etape 3.

**Etape 3 - Leviers, un seul a la fois**, dans l'ordre de la trace : fusion des draws, f16, ASTC, decoupage du rendu. Chacun mesure en A/B/A/B de 20 s, resolution figee, meme vue.

**Etape 4 - Decision sur la frequence.**

| Resultat | Suite |
| --- | --- |
| 72 Hz tient partout | 72 Hz par defaut, puis remonter la resolution |
| 72 Hz tient sur les scenes legeres | 72 Hz pour les mini-jeux et les scenes legeres, 120 Hz avec 60 i/s sur le plateau lourd, changement au changement de scene |
| 72 Hz ne tient pas apres l'etape 3 | 72 Hz a 80 %, ou rester a 120 Hz avec 60 i/s |
| 72 Hz ne regle pas le confort | AppSW (piste E) |

**Etape 5 - Finitions.** Nettete (resolution au-dessus de 95 % avec la marge gagnee), puis la stabilite et le contenu (section 10).

### 7. HUD hors de la passe des yeux (29/09), a mesurer

Les yeux et le HUD partageaient une image par emplacement de l'anneau : les yeux cote a cote, le HUD dessous, soit 3360x2960 pixels pour 3360x1760 d'yeux et 1600x1200 de HUD. La passe MSAA 4x des yeux effacait, resolvait et ecrivait toute l'image : 41 % de ses pixels etaient le HUD ou rien du tout (la zone a droite du HUD, 1760x1200). Le HUD a maintenant sa propre image partagee et sa propre passe, apres celle des yeux dans la meme soumission ; les deux fences sont fusionnees en une seule pour le casque (`SYNC_IOC_MERGE`). La passe des yeux ne couvre plus que les yeux. Le journal le dit : `Stereo: 3 images of ...x..., HUD ...x... apart`.

Le casque ne copie le HUD qu'a 30 Hz. Il est donc dessine une image sur deux : le thread XR le demande a une image sur deux qu'il prete au jeu (`drawHud`, `AuroraStereoSetHud`), et ne copie le HUD que depuis ces images. Une image qui ne le dessine pas ne touche pas son image de HUD. `adb shell setprop debug.partyboard.hud_rate full` le fait dessiner a chaque image (relu toutes les 2 s) ; `hudNew` dans `Stereo perf` donne les copies du HUD par seconde (environ 30 attendues). `tools/quest_campaign.ps1` a trois phases de plus : `hud-every`, `pacing-off` (calage coupe) et `filter-normal` (filtre du compositeur allege).

A verifier sur casque : le HUD s'affiche comme avant, a 30 Hz, sans image en retard, et le GPU baisse.

### 8. TEV en demi-precision (29/09), a mesurer

`adb shell setprop debug.partyboard.shader_f16 1`, avant de lancer le jeu, fait generer les shaders GX avec le TEV en f16 (`ShaderConfig::halfPrecision`, `lib/gx/shader.cpp`), si le GPU a la fonctionnalite `shader-f16` de WebGPU. Desactive par defaut. Ce qui passe en f16 : les registres du TEV (`prev`, `tevreg0-2`, copies `hprev`, `htevreg0-2`), ses operations (melange, biais, echelle, bornes). Ce qui reste en f32 : textures, couleurs rasterisees et constantes (converties a l'entree du TEV), coordonnees, brouillard, test alpha, profondeur, l'emulation du debordement 8 bits (appliquee en f32 autour des registres) et les comparaisons sur 16 et 24 bits, qui depassent la plage du f16 (65 504). Adreno calcule en f16 a double debit ; les valeurs du TEV sont des couleurs 8 bits entre -4 et 4, ou le f16 garde des pas de 1/1024 a 1, un quart de pas 8 bits.

Verifie sur PC (`tools/test_quest_stereo_render.ps1`, GPU Intel, 29/09) : les 15 variantes f16 generees sont acceptees par Dawn. Contre le f32, une scene a un etage : 450 pixels sur 19 200 (2,3 %) a un pas de 8 bits, aucun au-dela. Une scene a trois etages (constante, biais et echelle x2 sans borne donc debordement du registre relu, soustraction, comparaison 8 bits) : 521 pixels a un pas, 2 (0,01 %) de l'autre cote du seuil de la comparaison. Les pipelines f16 du cache disque sont ignores sur un appareil sans f16 (leur validation echouerait, ce qui est fatal). A mesurer sur casque, phase `shader-f16` de `tools/quest_campaign.ps1 -RestartPhases` : le gain en GPU, et a l'oeil, les degrades et les effets de couleur.

### 9. Pipelines des yeux (29/09), a mesurer

Un dessin dont le pipeline n'est pas encore construit est abandonne (`bind_pipeline`), pas retarde : les objets manquent de l'image jusqu'a la fin de la construction, et une construction sur Adreno peut prendre des dizaines de millisecondes. Les pipelines des yeux a un seul dessin pour les deux yeux (`StereoDiscard`, `StereoUncut`) n'etaient pas conserves dans le cache disque (seul `StereoOff` l'etait) : ils etaient donc tous construits a la premiere vue, a chaque lancement, sur le plateau. Ils sont conserves desormais (sauf `StereoClipDistance`, que le pilote ne sait pas construire) et reconstruits au demarrage avec les autres. Le journal ecrit, toutes les 5 s quand il y en a, `Pipelines: N draws dropped in the last 5 s ...` ; `summary.json` en tire `pipeline_drops`. A verifier sur casque, deux lancements de suite : la premiere partie remplit le cache, la seconde doit montrer beaucoup moins de dessins abandonnes sur le plateau, mais un demarrage plus long.

### 9 bis. Masque des lentilles et scene figee (30/09), a mesurer

Masque des lentilles : le casque donne le contour de ce que chaque lentille laisse voir (`XR_KHR_visibility_mask`, contour visible). `stereo_view.cpp` le convertit, avec le champ de vision des yeux, en rectangles de l'image de l'oeil entierement hors du contour : une grille de 48x48 cases, retenue seulement si aucun bord du contour ne traverse la case et si son centre est dehors, puis regroupee en rectangles par ligne. Le decoupage est donc prudent : aucun pixel visible n'est couvert. Au debut de la passe des yeux, Aurora remplit ces rectangles avec la profondeur la plus proche (le shader d'effacement, couleur coupee, limite a chaque rectangle). Tout dessin teste en profondeur y est rejete par le test de profondeur precoce, avant d'etre ombre. Le journal ecrit `Hidden area: left N rectangles (x % of the eye), right ...`. Il suffit de `adb shell setprop debug.partyboard.visibility_mask 0` (relu toutes les 2 s) pour ne plus remplir le masque. Gain estime : 10 a 20 % des fragments, selon la part cachee annoncee dans le journal. A verifier : aucun bord noir ou decoupe visible dans le casque, et la phase `mask-off` de la campagne.

Scene figee : `adb shell setprop debug.partyboard.freeze 1` arrete la simulation (les ticks sont consommes, aucun n'est joue) sans arreter le rendu. Chaque phase dessine donc la meme image, et le journal ecrit `Simulation frozen`. `tools/quest_campaign.ps1` fige desormais le jeu pendant ses phases (`-NoFreeze` pour laisser jouer). Il intercale aussi une phase de reference `ref-<nom>` avant chaque phase testee (`-NoAlternate` pour ne pas le faire) et donne l'ecart de chaque phase a sa reference (`delta_frag_pct`, `delta_gpu_pct`, `delta_m2p_pct`), ce qui neutralise la derive de la chauffe et des frequences. Les phases en direct durent donc environ 16 minutes. Avant, les ecarts entre deux phases identiques allaient jusqu'a 40 %.

### 9 ter. Mesures du 02/10 au soir : ou va le GPU (builds 160 a 164)

Le masque des lentilles est sans effet sur le Quest 3 : le runtime rend un masque vide (`Hidden area, eye 0: outline 0 points (result 0), hidden mesh 0 triangles`), meme une fois la session FOCUSED.

Le gel (`debug.partyboard.freeze 1`) fige aussi les vues des yeux (stereo_view.cpp) : sans cela, la tete qui bouge changeait la scene dessinee (80 a 611 dessins par oeil d'une phase a l'autre). Pendant le gel, le plateau suit la tete : c'est inconfortable, donc a garder court.

120 Hz demande (`render_hz 0`), plateau de Toad, scene figee :

| | images/s | anneau plein / 2 s | GPU | MHz | latence |
|---|---|---|---|---|---|
| anneau de 3, 95 % | 61 | 117 | 82 % | 611 | 66 ms |
| anneau de 3, 75 % | 63 | 110 | 81 % | 610 | 66 ms |
| anneau de 4, 95 % | 80 | 73 | 90 % | 640 | 65 ms |
| anneau de 4, 75 % | 88 | 51 | 89 % | 640 | 64 ms |

Avec 3 images, l'anneau limitait (images pretes ou en copie, GPU pas plein) : l'anneau passe a 4 par defaut (`debug.partyboard.ring`, 2 a 6, au demarrage). Avec 4, le GPU est le goulot.

Sondes ajoutees (journal) : `Stereo GPU` (horodatages GPU de la passe des yeux et du HUD), `Stereo pipeline` (enregistrement, GPU, attente, total par image ; etat de l'anneau quand il est plein), `Render worker` (encodage, soumission, occupation, attentes du jeu), `Game loop` (attente, simulation, dessin, fin d'image, limiteur), `Stereo kinds` et `debug.partyboard.skip_draws` (cout par categorie de dessins), `debug.partyboard.alpha_test opaque-off`.

Ce que coute une image (plateau entier, 430 dessins par oeil) : passe des yeux 8,7 ms a 640 MHz (10,8 ms vers 620 MHz en 60 Hz), HUD 0,8 ms, compositeur 1,1 a 2,6 ms par affichage. La trace par etape (`tools/quest_gpu_trace.ps1`) donne pour la passe des yeux : binning 2,2 ms (78 000 polygones), rendu 7,1 ms dans 63 bins de 384x256 (MSAA 4x et profondeur 32 bits : 32 octets par pixel en memoire de tuile), 20 millions de fragments pour 5,3 millions de pixels. Compteurs en direct : shaders occupes 49 %, 46 a 50 instructions ALU par fragment, textures occupees 47 %, attente texture 12-15 %, anisotrope 5 % des lectures.

A/B courts (scene figee, ecart a la reference juste avant) : anisotrope 4x -1,5 % de passe des yeux ; test alpha retire des dessins opaques -3 a -7 % (le jeu met GEQUAL 1 partout) : ni l'un ni l'autre n'est le goulot.

Conclusion : deux moities. Les bins (63 par image, chacun rejouant les dessins visibles) et le binning, sans shader ; et le cout du TEV emule (46 instructions par fragment). Prochaines etapes : profondeur 16 bits pour les yeux (bins plus grands), TEV en demi-precision et TEV genere plus court, plus de dessins pour les deux yeux a la fois.

### 9 quater. Build 166 : par defaut, a verifier sur casque

Mesures en direct (scene figee, 421 dessins par oeil, en cycles : ms x MHz) : profondeur 16 bits -5 %, TEV en demi-precision -8 %, MSAA 1x -22 % (crenelage), lot "test alpha coupe + anisotrope 4x" -3 %. Les pixels et les shaders ne sont pas le coeur du cout : 44 % de pixels en moins ne donnaient que -9 %. Ce qui coute, ce sont les dessins rejoues dans chacune des 63 tuiles (le MSAA 1x en divise le nombre par 4).

Actifs par defaut depuis le build 166, chacun avec son interrupteur :

- profondeur 16 bits pour les yeux (`debug.partyboard.eye_depth 32` pour revenir), avec le plan proche a 10 cm au lieu de 5 (pas de profondeur de 0,3 mm a 1,4 m) ;
- TEV en demi-precision pour les yeux, si le GPU a `shader-f16` (`debug.partyboard.eye_f16 0`) ;
- affichage a 90 Hz, chaque image neuve (`debug.partyboard.display_hz 120` pour revenir a 120 Hz et 60 images neuves) ;
- anneau de 4 images (`debug.partyboard.ring 3`).
- build 167 : un objet qui touche les deux yeux est un seul dessin instancie (`debug.partyboard.stereo_crossing 1` pour le dessiner une fois par oeil). Par oeil, il y avait 11 % de fragments en moins (build 117), mais deux fois ses dessins dans chaque tuile ; le 02/10, en un seul dessin : fragments +9 %, GPU -11 %. Tous les dessins ont deja un `discard` (test alpha GEQUAL 1 du jeu).

A verifier demain : pas de scintillement des cases et chemins (profondeur 16 bits), couleurs et degrades (demi-precision), 90 images/s tenues sans saccade (`Stereo perf` worldNew, `Game perf` stutters, `Stereo GPU`), et la fluidite ressentie. La premiere partie compile les nouveaux pipelines : quelques secondes d'objets manquants a la premiere vue du plateau.

Prochaine piste, la plus grosse : moins de dessins par tuile (objets qui touchent les deux yeux dessines une seule fois, `stereo_crossing 0` a mesurer proprement ; fusion des dessins consecutifs de meme materiau).

### 9 quinquies. Technologies Meta : profondeur pour le compositeur (build 168), a essayer

Le runtime propose `XR_KHR_composition_layer_depth` et `XR_FB_space_warp`. Premiere etape, sans toucher a Aurora : le fil XR dessine lui-meme, en GL, la profondeur du plan du plateau (la table sous la scene, 1,1 fois la scene ajustee) pour chaque oeil de chaque image neuve, dans une swapchain de profondeur, avec une projection GL classique (0 au plan proche de 10 cm, 1 au lointain de 100 m), et la joint a la couche des yeux (`XrCompositionLayerDepthInfoKHR`). Pour une maquette posee sur une table, la plupart des pixels sont a quelques centimetres de ce plan. Un mini-jeu flottant (tourne vers la tete) n'en a pas.

Desactive par defaut : `adb shell setprop debug.partyboard.depth_layer 1` (relu toutes les 2 s). Le journal ecrit `Depth layer: ...x..., format ..., program ready` au demarrage et `Depth layer: submitted` quand il est envoye. A voir sur casque : si le plateau tient mieux en place quand la tete se deplace (60 images sur 120 Hz : `display_hz 120`), et que rien ne se deforme. Suite : Application SpaceWarp (vecteurs de mouvement nuls pour le plateau fixe, cette profondeur), puis le test de profondeur du compositeur pour le HUD (`XR_FB_composition_layer_depth_test`).

Application SpaceWarp (build 169), desactive par defaut : `adb shell setprop debug.partyboard.space_warp 1` (relu toutes les 2 s), avec `debug.partyboard.display_hz 120` (au demarrage) pour que le jeu dessine 60 images neuves et que le compositeur fabrique les 60 autres. Le runtime donne la taille recommandee des vecteurs de mouvement (`XrSystemSpaceWarpPropertiesFB`) ; le fil XR remplit pour chaque image neuve une image de vecteurs de mouvement a zero (le plateau est immobile dans la piece, le compositeur connait le mouvement de la tete) et la profondeur du plan du plateau a cette taille, et joint `XrCompositionLayerSpaceWarpInfoFB` a chaque vue (pose d'espace inchangee). Les personnages animes n'ont pas de vecteurs : ils gardent leur cadence de 60 images. Journal : `Space warp: motion vectors WxH per eye, depth ..., ready` au demarrage, `Space warp: on` quand il sert. A voir sur casque : le plateau tient en place quand la tete se deplace, 120 Hz fluide, pas de deformation au bord des objets.
Deux mesures ajoutees a la campagne : `filter-none` (filtre du compositeur coupe sur la couche des yeux) et `gpu-boost` (niveau GPU boost).
### 10. Stabilite et contenu

1. Retour au jeu apres une longue pause (le build 109 n'a ete teste que sur cinq retours courts), et reprise apres Space Setup : ligne `Layers:` et garde « table sous le sol ».
2. Plantage de m440 (SIGSEGV, build 88), a reproduire ; son rapport n'est pas lisible sans acces root.
3. Parcours plateau, m428, retour au plateau, en suivant la memoire (`Memory:` : 611 a 634 Mo residents sur le plateau au build 112).
4. Controles visuels : bords des yeux avec le mode sans coupe, plateau fixe, HUD en ecran de stade, mini-jeux en maquette inclinee (le grip gauche bascule ce mode), table du scan de la piece (aucune table scannee le 28/09).
5. Hauteur des mini-jeux quand ils sont poses sur la table. Le sol est mesure sur la geometrie des 20 premieres images (`PartyBoard_StereoObserveBounds`). Piste : la hauteur des pieds des personnages, dans `charWork[]` (`src/game/chrman.c`), qui demande un accesseur sous `TARGET_PC`.
6. Mini-jeux au cas par cas, avec des captures reproductibles. Ne pas modifier le temps de simulation pour obtenir une frequence d'affichage plus haute.

### 11. Long terme : changement de backend

- Une integration Dawn/Vulkan qui cible directement les images OpenXR, a la place des buffers Android partages et des copies GL. La bibliotheque Dawn precompilee actuelle ne fournit pas le chemin d'import/export necessaire.
- Le vrai multiview et la foveation dans ce backend, avec detection des capacites et repli Quest 2/3. La foveation doit porter sur la passe 3D couteuse, pas sur une copie de presentation.
- AppSW, la reprojection de la translation par profondeur et vecteurs de mouvement (piste E, conception plus haut), si le 72 Hz ne suffit pas.
- Une reconstruction temporelle, seulement avec des vecteurs de mouvement et un historique coherents (GX n'en fournit pas). Pas d'upscaling IA promis sans implementation ni mesure de cout.

## Plan d'optimisation du 03/10 : goulots mesures et ordre des travaux

### Carte des goulots (plateau de Toad, Quest 3, sondes du build 163 et suivants)

| Poste | Mesure | Verdict |
|---|---|---|
| Passe des yeux (GPU) | 8,2 a 8,7 ms a 640 MHz, pleine resolution, 420 dessins par oeil | **Goulot principal** |
| dont binning (sommets) | ~23 % : 78 000 polygones, sommets ombres deux fois | important |
| dont rendu dans 63 bins | ~75 % : chaque bin rejoue les dessins visibles ; shaders occupes 49 %, 46-50 instructions ALU par fragment | **le coeur** |
| Passe du HUD (GPU) | 0,8 a 1,2 ms, une image sur deux (MSAA 4x, 1600x1200) | cree une alternance de cout d'une image a l'autre |
| Compositeur | 1,1 a 2,6 ms par affichage (4 ms avec la profondeur a 90 Hz) | 13 a 30 % du GPU a 90-120 Hz |
| Pointes GPU | max 16-18 ms par image | a-coups a 90 Hz |
| CPU du jeu | enregistrement 4-5 ms, fil de rendu 26 % occupe, simulation < 0,2 ms | pas un frein |
| Chaine d'images | 4 images : plus pleine a 72/90 Hz | regle |
| Latence / cadence | 90 Hz : 75-83 images neuves, age 11-45 ms ; 72 Hz + retenue : 72/72, age 13,9 ms, mouvement->image 38 ms | 72 Hz regle la cadence |
| Instabilite du plateau | disparait quand le suivi de position est perdu (capteurs caches) | **translation de la tete non corrigee** : il faut SpaceWarp |

Leviers deja mesures sans grand effet : resolution (-44 % de pixels -> -9 %), anisotrope 4x (-1,5 %), test alpha coupe (-3 a -7 %), masque des lentilles (aucun sur Quest 3), profondeur seule pour le compositeur (pas de difference ressentie).

### Phase 1 : confort (la nausee), en cours

1. **SpaceWarp cale** (build 175) : 120 Hz affiches, 60 images calculees, la translation de la tete corrigee par le compositeur. Critere : 60 images neuves sur 60, age constant, plateau stable en se deplacant. Sinon, repli : 72 Hz avec retenue (`display_hz 72`, `xr_pacing 1`).
2. **Choisir le mode par defaut** selon le ressenti, et le noter ici.

### Phase 2 : lisser le cout de chaque image (pointes, alternance)

3. **HUD sans MSAA** (une interface plate, du texte) ou a 2x, et peut-etre a chaque image : la passe du HUD passerait d'environ 1 ms a environ 0,3 ms, sans l'alternance qui fait deborder une image sur deux a 90 Hz. Gain : -5 a -10 % les images avec HUD.
4. **Pipelines compiles d'avance** pour les variantes demi-precision et profondeur 16 bits (le cache disque les garde deja apres la premiere partie) : plus d'objets absents ni de pointes a la premiere vue d'un plateau.
5. **Sonde des pointes** : les images de plus de 12 ms avec leur contenu (dessins, HUD, compilation) pour en trouver la cause.

### Phase 3 : moins de travail par bin (le goulot GPU)

6. **Fusion des dessins** : lire `samePipeTex` (part des dessins au meme pipeline et aux memes textures que le precedent). Si elle depasse 20 %, fusionner a travers les etats GX redondants (meme pipeline, memes textures, uniformes identiques -> un seul dessin). Gain attendu proportionnel.
7. **Culling par oeil** : un objet dont les bornes sortent d'un oeil n'y est pas dessine (les objets du jeu ont des bornes : `ObjDraw`).
8. **Liaisons dedupliquees** (fait, build 170) : a mesurer.

### Phase 4 : moins de travail par fragment

9. **TEV genere plus court** : sans l'emulation des debordements quand ils sont impossibles, constantes precalculees, etages fusionnes. Objectif : de 46 a moins de 30 instructions par fragment. Gain : -10 a -15 %.
10. Demi-precision et profondeur 16 bits (faits, -8 et -5 %).

### Phase 5 : compositeur

11. **Filtre de la couche des yeux** : mesurer `layer_filter none` (phase `filter-none`). Gain possible : -0,3 a -1 ms par affichage.
12. Profondeur pour le compositeur : la garder seulement avec SpaceWarp (elle coutait +2 ms par affichage a 90 Hz sans effet ressenti).

### Phase 6 : architecture (plusieurs jours), pour 120 images calculees

13. **Rendu Vulkan direct dans les swapchains OpenXR** (plus de copie GL), qui ouvre le **multiview** (les deux yeux en une passe : moins de rejeu par bin) et le **rendu foveal** (densite de fragments).
14. **Boucle du jeu cadencee par le fil XR** (pour SpaceWarp et la latence minimale).

Objectif chiffre pour 120 images calculees : environ 7 ms de GPU par image, compositeur compris, contre ~9 a 10 aujourd'hui : les phases 2 a 5 visent -30 %, la phase 6 le reste.

### Methode de mesure

Chaque etape : la campagne (`tools/quest_campaign.ps1 -Only ...`, scene et vues figees, reference avant chaque phase) et les sondes du journal (`Stereo GPU`, `Stereo pipeline`, `Stereo kinds`, `Render worker`, `Game loop`, `holds=`). Puis le ressenti au casque, qui decide.
### Mesures du 03/10 en fin de matinee (builds 176 a 179)

- **HUD sans MSAA** (build 177, `debug.partyboard.hud_msaa 4` pour revenir) : la passe du HUD reste a 0,7-0,8 ms (55 dessins) ; le cout etait les dessins, pas le MSAA.
- **Fusion des dessins a etat GX reecrit mais identique** (essayee puis retiree) : 1 054 dessins sur 152 739 (0,7 %).
- **Sonde des changements d'etat entre deux dessins** (essayee puis retiree, elle copiait l'etat a chaque dessin) : 0 % d'etats identiques ; matrices 73 %, tableaux de sommets 74 %, textures 65 %, canaux de couleur 29-39 %, etages TEV 26 %, eclairage 11 %. Les ~360 dessins par oeil sont de vrais morceaux differents : les fusionner demande un regroupement multi-dessins (donnees par dessin dans un stockage lu par le shader), au mieux -27 % de dessins (samePipeTex).
- **Cases du plateau qui scintillent et tremblent quand le personnage avance**, en profondeur 16 comme en 32 bits : piste, le tri des objets de l'avant vers l'arriere, qui change l'ordre des cases (dessinees apres le sol, a la meme profondeur) quand la camera du jeu bouge. A verifier avec `debug.partyboard.sort_opaque 0`.
- Charge actuelle a 72 Hz, plateau entier : passe des yeux 9,5-10 ms a 640 MHz (360 dessins par oeil), 59-61 images neuves sur 72, resolution dynamique 90 %.
### Bilan du 03/10 apres-midi (builds 180 a 187)

Mesure ou essaye sur le plateau de Toad :

- **Cases du plateau qui tremblaient** quand un joueur avancait : `DrawSpaces` (board/space.c) recalculait sa vue avec la camera du plateau du dernier tick de simulation, alors que la scene est dessinee avec la camera interpolee. Corrige (build 183), valide au casque. Les cases sont aussi ecartees selon les yeux, et toutes dessinees en un seul dessin (build 184).
- **Regroupement des dessins** (build 182, `batch_draws`) : ~83 dessins par image joints en ~30 ; passe des yeux 10,4-11,2 ms sans, 10,9-11,2 avec : neutre. Garde.
- **Objets hors des yeux** (build 181, `cull_objects`) : sans effet sur ce plateau, ses modeles passent deja par le test de visibilite du jeu, qui utilise les yeux.
- **SpaceWarp** (builds 180, 175) : 51 a 53 images neuves sur 60, prediction 92 ms : abandonne, il faudrait le jeu cadence par la boucle XR et une passe plus legere.
- **MSAA 1x** : -15 % de passe des yeux a vue egale (9,8 -> 8,4 ms), mais nettement plus laid : 4x garde.
- **Filtre du compositeur coupe** (build 186, par defaut) : compositeur de 2,2 a 1,7 ms par affichage ; le plateau semble bouger moins (l'accentuation faisait grouiller les bords). Un peu moins net, accepte.
- **Images des yeux reduites plus vite** (build 186) : elles restaient a leur taille maximale (4208x2200) pour 70 a 95 % dessines ; reduites apres 8 s a plus de 15 % inutilises.
- **Resolution a l'entree d'un plateau** (build 185) : celle que le plateau tenait la derniere fois (90 % la premiere), et non celle des menus.
- **Emulation du debordement 8 bits du TEV** (build 187) : sur les 764 shaders des yeux regeneres depuis le cache de pipelines du casque, 1 337 enveloppements, dont la plupart d'operandes deja bornes a [0, 1] : 141 restent. Exact (0 pixel different).
- **Rendu Vulkan direct, multiview, rendu foveal** : impossible sans forker Dawn (il n'expose ni son peripherique Vulkan ni la creation sur un peripherique fourni, et WebGPU n'a ni multiview ni densite de fragments) : ecarte.

Reglages par defaut a la fin : 72 Hz, images retenues pour leur affichage, GPU en boost, MSAA 4x, TEV en demi-precision, profondeur 16 bits, filtre du compositeur coupe, HUD sans MSAA ni ecran sombre, anneau de 4 images. A mesurer au prochain essai : la passe des yeux avec le TEV allege (build 187), plateau entier dans le champ.
## Bilan du 04/10 (builds 188 a 201)

### Regularite et latence : la copie anticipee

- Sonde des regards (`Pacing looks`) : 31 % des images attendues etaient encore au GPU au passage du casque. Sonde de la copie (`Copy GPU`) : la copie vers les images OpenXR attendait 5 a 6 ms (max 14) derriere la passe des yeux de l'image suivante ; le casque, voyant ses images finir si tard, preparait l'affichage 46 ms a l'avance au lieu de 18 (Prd VrApi 78-80 ms). Preuve : sans le decor (`skip_draws`), copie 0,46 ms, avance 18 ms, 72/72.
- Build 194 : apres `xrEndFrame`, le fil XR attend l'image due a l'affichage suivant, la copie pendant que le GPU est libre et la rend aussitot (`StereoView::early_copy`). Resultat : 61 -> 72 images neuves sur 72, ~15 -> ~0 images ratees par seconde, avance 46 -> 21 ms, Prd 80 -> 51 ms, delai constant 27,8 ms.
- Build 195 : le pacer grossit son plan des que 2 images sur 8 depassent (scene plus lourde), et vise la fin de la copie anticipee (2 ms avant le regard) : ~100 % des images copiees en avance.
- Builds 190-191 : le pacer suit les heures d'affichage OpenXR (exactes) et non l'heure des regards (+-7 ms) ; les images ratees sont comptees et donnees au controleur de resolution (`missedLate`, `missedDropped`).
- Build 199 : late latch, la pose des yeux relue une fois l'image enregistree, chaque projection corrigee (`AuroraStereoSetLateLatch`). Correction mesuree 0,1-0,5 mm en moyenne, jusqu'a 6 mm ; le Prd du systeme ne la voit pas.

### Le GPU suit la charge

- La frequence du GPU suit la charge (611 MHz a 100 %, 469 MHz a 65 %) : le temps de la passe des yeux reste ~10 ms quoi qu'on retire. Le vrai cout se lit en occupation x frequence (campagne a image figee).
- Image figee, plateau Toad, 100 % : 65 % de resolution -42 % de travail ; objets opaques 73 % du travail ; transparents 19 % ; TEV a 3 etages et plus 17 % ; test alpha coupe 0 % ; tri d'avant en arriere +7 % de travail ; **tri par pipeline et textures -10 %** (defaut depuis le build 200, rien de visible).
- Resolution du plateau : 100 % par defaut (build 196), plancher 90 % sur un plateau (baisser ne faisait que flouter), pas de decision pendant les 3 s qui suivent l'arrivee d'une scene (build 197).
- 90 Hz : vue legere 90/90 et 39 ms mouvement->photon, plateau entier sature (68-85 images, GPU 92-96 %) ; 90 Hz a 80 % tient 90/90 (GPU 85-88 %, 41-45 ms). 72 Hz a 100 % reste le defaut.

### Pistes du soir (build 201, non mesure)

- Shaders : le debordement 8 bits final n'est plus applique qu'aux composantes qui peuvent sortir de [0, 1] (un lerp de valeurs dans [0, 1] y reste) : sur les 1083 shaders des yeux du cache du jour, 138 enveloppaient tout `prev`, 0 maintenant (105 une composante, 33 rien).
- Textures du decor en ASTC 4x4 (8 bits par texel au lieu de 32), encodees en arriere-plan par astc-encoder (ARM, Apache-2.0) et gardees en cache (`astc4x4/`), `debug.partyboard.astc 0` pour couper.
- Foveation fixe : demande un Dawn modifie (`VK_EXT_fragment_density_map`, carte de densite attachee a la passe des yeux) ; Dawn se compile depuis ses sources pour Android (`C:\mp4w\dawn-src`).

## Bilan du 05/10 (builds 205 a 210)

Base au plateau Toad, 72 Hz, 100 % (build 210) : 72/72 images neuves, 0 ratee, 0 saccade, delai constant 27,8 ms, toutes les images copiees en avance, GPU 65-85 % selon la frequence choisie par le systeme, passe des yeux ~9,4 ms.

- Cache de pipelines du Quest (build 205) : le cache livre (PC) etait en version 13, ignore par le jeu (version 14) ; le Quest livre maintenant celui du casque, 2840 pipelines charges au demarrage (53 que le casque ne peut construire sont ecartes).
- Foveation : Dawn choisissait le dynamic rendering, que la carte de densite ne suit pas ; avec les render passes (build 206) la carte est appliquee (`foveation test` : tout pixelise ; `split` : un oeil net, l'autre pixelise), mais les niveaux 1 a 3 ne changent ni les fragments (-9,5, +2,2, -1,5 %) ni la passe des yeux (9,2-9,6 ms), et l'image plait moins. **Coupee par defaut** (build 210), Dawn garde son dynamic rendering sauf si `debug.partyboard.foveation` est donne au demarrage.
- ASTC : -5 % de memoire lue (4,36 contre 4,57 Go/s), passe des yeux 9,22 contre 9,49 ms, travail GPU dans le bruit. Garde (rien de visible), sans gain de vitesse.
- Filtre automatique de Meta (`layer_filter auto`) : aucune difference visible, compositeur 2,0 -> 3,7 ms par image affichee. Non retenu.
- 90 Hz a 110 % (en jeu) : 79-82 images neuves sur 90, ~5 ratees par seconde, delai 35-38 ms (25 a 52), mouvement->photon 64 ms contre 48, GPU 95 %. Ressenti : latence et manque de fluidite. **72 Hz a 100 % reste le defaut.**

### Pistes du soir (build 211, non mesure)

- Interface redessinee seulement quand elle change (`hud_keep`, actif par defaut) : Aurora hache chaque dessin de l'interface (pipeline, sommets, indices, uniformes sans leurs decalages de tampon, tableaux indexes, textures et leurs ecritures) ; meme hache que la derniere dessinee : pas de passe, le casque garde l'image d'avant. Une interface dessinee mais jamais copiee (image abandonnee) force la suivante. A mesurer : `Stereo interface: kept N of M`, phase `hud-keep-off`.
- Sondes coupees par defaut (`debug.partyboard.probes 1` pour les remettre) : horodatages GPU des passes (`gpu_timing 1` aussi, au demarrage), barriere de fin de copie (`Copy GPU`), statistiques par type de dessin (`Stereo kinds`).
- Decodage GX sur un fil a lui (`debug.partyboard.gx_thread 1` au demarrage, **coupe par defaut**) : le fil du jeu ne fait qu'ecrire ses commandes, un autre coeur les decode dans l'ordre, comme le GPU de la GameCube lisait sa FIFO ; ce qui change l'etat hors FIFO (copies EFB, crochets stereo, TLUT) passe dans le meme ordre (`fifo::ordered`), et la fin de l'image ou `GXDrawDone` attendent le decodeur. La ligne `GX decode: X ms a frame` dit ce que le decodage coute au fil du jeu (aujourd'hui) ou au sien, et l'attente du jeu : c'est le gain possible. Phase `gx-thread` (redemarrage).

## Bilan du 09/10 (builds 229 a 231)

Mesures par scene : `python tools/quest_scene_report.py build/quest-session-*/logcat-*.txt` (images/s hors chargement, saccades, resolution, dessins, enregistrement, temps GPU, frequence et charge du GPU).

- Mini-jeux (sessions du 06 et du 09, vue camera) : une fois lances, presque tous tiennent 70-72 images/s pour 72 ; les chiffres a 43-44 venaient de fenetres de 5 s coupees par le chargement. GPU charge a 70-90 %, souvent a 456 MHz (niveau 2) malgre la demande de boost : le systeme choisit la frequence. Revers-a-Bomb : ~175 dessins par oeil mais 9-11 ms de GPU : les pixels coutent dans un mini-jeu vu en grand, pas les dessins comme au plateau.
- Pipelines : 4772 precompiles au demarrage (11 s), le reste se construit a la premiere rencontre et le cache les garde. Les pics de dessins abandonnes (jusqu'a 15 000 en 5 s, objets absents une fraction de seconde) venaient des scenes nouvelles en 3D (menus, build 229) ; ils disparaissent une fois vus. Attendre le pipeline figerait l'image : non retenu.
- Copie de l'image (Three Throw, Mario Medley) : la passe d'apres la derniere copie est deja sautee quand l'ecran plat est cache ; reste le monde dessine en 3840x2160 pour etre copie en 640x480. Piste : une image plate plus petite quand l'ecran est cache (recreer la cible de rendu, non fait).
- Tout sur la table par defaut (build 230, cle `scene_view`) : menus, plateaux et mini-jeux ; le tri des objets hors champ revient aux mini-jeux.
- Yeux cadres sur la scene (build 231, `debug.partyboard.crop 0` pour couper) : le jeu envoie chaque image la boite (piece) des objets qu'il propose au dessin, comptes avant le tri hors champ ; le casque reduit le champ de chaque oeil a cette boite avec une marge, a densite de pixels egale. Ligne `Stereo crop` : part des pixels dessines. A verifier : objets coupes au bord du cadre (particules, grands decors ; objets de plus de 1,5 m ignores).
- Fils nommes au systeme (build 231, `XR_KHR_android_thread_settings`) : fil XR en rendu principal, fil du jeu en application principale.

## Sources Meta du plan

- [OVR Metrics Tool](https://developers.meta.com/horizon/documentation/native/android/ts-ovrmetricstool/)
- [RenderDoc et Render Stage](https://developers.meta.com/horizon/documentation/native/android/ts-renderdoc-renderstage/)
- [Analyse MSAA](https://developers.meta.com/horizon/documentation/native/android/mobile-msaa-analysis/)
- [Super-resolution et qualite](https://developers.meta.com/horizon/blog/vr-image-quality-meta-quest-super-resolution/)
- [Multiview](https://developers.meta.com/horizon/documentation/unity/enable-multiview/)
- [Application SpaceWarp](https://developers.meta.com/horizon/documentation/unity/os-app-spacewarp/)
- [Foveation fixe native](https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/)
- [Projection symetrique](https://developers.meta.com/horizon/documentation/native/android/os-symmetric-projection/)
