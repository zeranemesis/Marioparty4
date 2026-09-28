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
- `-GpuMetrics` enregistre les compteurs Adreno d'`ovrgpuprofiler` (`gpu-metrics.txt`, une valeur par seconde) pendant la capture.

`summary.json` ajoute `game` et `game_by_scene` (images/s du thread du jeu, temps d'enregistrement, saccades), `draws` (draws du monde par oeil, part instanciee) et `memory` (memoire residente, memoire disponible du systeme, croissance sur la capture ; ligne `Memory:` du journal toutes les 5 s). La memoire residente n'est pas le PSS : elle suit la croissance, `dumpsys meminfo` reste la reference ponctuelle.

Chaque baisse de resolution est desormais un essai. La fenetre suivante, qui recree les images, est ignoree. La fenetre d'apres doit livrer au moins 5 % d'images en plus (plus une), sinon la resolution precedente revient et reste 30 s. Le journal l'indique (`restored: fewer pixels brought no more images`). Mesure du 28/09 (build 89, Quest 3, 72 Hz, plateau Toad) : a 414 draws par oeil, la cadence restait a 43-45 images/s de 80 % a 70 %, avec un GPU a 88-92 % et un thread du jeu a 5-6 ms. La limite venait du nombre de draws, pas des pixels. Les compteurs `app/gpu_frametime` et `compositor/gpu_frametime` ne voient pas le rendu Vulkan du jeu.

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

Aucun casque n'etait connecte pour cette validation : fluidite, absence de scintillement, consommation et comportement des redimensionnements doivent encore etre verifies sur appareil. Aucune amelioration chiffree n'est revendiquee.

## Etapes restantes du plan

1. Capturer les profils Quest 2/3, identifier CPU, GPU, bande passante et cout des copies, puis regler les marges avec ces mesures.
2. Valider la stereo instanciee presente dans le moteur et ses shaders sur appareil, y compris fallback. Ce chemin ne constitue pas le multiview natif OpenXR.
3. Construire une integration Dawn/Vulkan capable de cibler les images OpenXR directement, puis remplacer la liaison par buffers Android partages et les copies GL. La bibliotheque Dawn precompilee actuelle ne fournit pas le chemin d'import/export requis pour cette integration. C'est un changement de backend, pas un reglage.
4. Integrer le vrai multiview et la foveation dans ce backend, avec detection des capacites et fallback Quest 2/3. La foveation doit affecter la passe 3D couteuse, pas seulement une copie de presentation.
5. Etudier resolution variable par zone, projection symetrique et profondeur du compositeur seulement apres les profils. Tester transparences, particules, interfaces et geometrie proche.
6. Traiter les minijeux au cas par cas avec captures reproductibles ; eviter de modifier le temps de simulation pour obtenir une frequence d'affichage plus haute.
7. Evaluer une reconstruction temporelle uniquement avec vecteurs de mouvement et historique coherents. Pas de promesse d'upscaling IA sans implementation et mesures de cout.

## Sources Meta du plan

- [OVR Metrics Tool](https://developers.meta.com/horizon/documentation/native/android/ts-ovrmetricstool/)
- [RenderDoc et Render Stage](https://developers.meta.com/horizon/documentation/native/android/ts-renderdoc-renderstage/)
- [Analyse MSAA](https://developers.meta.com/horizon/documentation/native/android/mobile-msaa-analysis/)
- [Super-resolution et qualite](https://developers.meta.com/horizon/blog/vr-image-quality-meta-quest-super-resolution/)
- [Multiview](https://developers.meta.com/horizon/documentation/unity/enable-multiview/)
- [Foveation fixe native](https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/)
- [Projection symetrique](https://developers.meta.com/horizon/documentation/native/android/os-symmetric-projection/)
