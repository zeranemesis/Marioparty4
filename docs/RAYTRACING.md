# Ray tracing matériel (DXR) dans PartyBoard

Étude de faisabilité, 4 septembre 2026. Conclusion : réalisable, mais c'est un
chantier de renderer, pas un réglage.

## Le blocage à contourner

WebGPU n'a aucune API de ray tracing, et Dawn n'expose pas DXR. Tout passage par
`wgpu::Device` est donc une impasse : il faut atteindre Direct3D 12 directement.

Bonne nouvelle, Aurora inclut déjà `dawn::native` — voir
`extern/aurora/lib/webgpu/gpu.cpp:704` (`DawnInstanceDescriptor`). Les en-têtes
natifs sont donc dans le périmètre de build.

## Ce qui rend le projet viable

Trois propriétés du code existant font l'essentiel du travail :

1. **La topologie est déjà résolue.** `gx/command_processor.cpp:1545` (`draw_prim`)
   parse les commandes GX, convertit strips et fans en triangles indexés
   (`prepare_idx_buffer`) et fusionne les draws consécutifs. On n'a pas à
   démêler un flux FIFO brut.

2. **Un point de dérivation unique existe.** `handle_draw_unmerged`
   (`command_processor.cpp:1616`) est la fonction où *tout* converge
   simultanément : tableaux d'attributs, formats de sommets, plage d'indices et
   matrices. C'est là qu'il faut brancher la capture — pas dans `push_verts`.

3. **Les matrices sont accessibles.** La mémoire de matrices de position GX
   (`command_processor.cpp:242`, plage `0x0000-0x0077`) est gérée explicitement.

### Correction : les positions ne sont pas dans le vertex buffer

Erreur d'analyse initiale, corrigée après lecture de `handle_draw_unmerged`.

Pour tout attribut déclaré `GX_INDEX8` / `GX_INDEX16`, les données réelles ne
transitent pas par `vertRange`. Elles vivent dans `g_gxState.arrays[i]`, poussées
comme **storage buffers** via `gfx::push_storage` (`command_processor.cpp:1630`).
Le flux de sommets ne contient alors que des *indices* vers ces tableaux, et
c'est le vertex shader généré qui fait la résolution.

Extraire un triangle en espace monde demande donc de réunir cinq éléments :

| Élément | Source |
|---|---|
| Tableau de positions, format GX brut | `g_gxState.arrays[GX_VA_POS]` |
| Type, nombre de composantes, bits fractionnaires | `vtxFmt.attrs[GX_VA_POS]` |
| Indice de position par sommet | flux `vertRange`, offset variable |
| Indice de matrice par sommet (`GX_VA_PNMTXIDX`) | flux `vertRange` |
| Matrices de position | mémoire de matrices GX |

L'offset de chaque attribut dans un sommet dépend des attributs qui le précèdent
et de leurs descripteurs — la logique est déjà écrite dans
`calculate_last_vtx_size` (`command_processor.cpp:1512`), qu'il faut refactoriser
pour exposer les offsets en plus de la taille totale.

## Architecture retenue

Device D3D12 privé, séparé de celui de Dawn, sur le même adaptateur. C'est le
schéma qu'emploie DLSS5-Feeder pour son chemin 32 bits, et il évite de disputer
à Dawn sa file de commandes et son état interne.

```
Aurora (Dawn / WebGPU)              Device D3D12 privé (DXR)
──────────────────────              ────────────────────────
draw_prim
  └─ handle_draw_unmerged ── tee ──► résolution des indices
     (arrays + fmt + mtx)            décodage s16/f32 + frac
                                    skinning PNMTX → monde
                                          │
                                    BLAS (rebuild/frame)
                                          │
                                    TLAS
                                          │
                                    G-buffer propre
                                    (profondeur + normales)
                                          │
                                    RayQuery DXR 1.1 → AO / ombres
                                          │
  passe de composition ◄── SharedTextureMemory + SharedFence
```

Le G-buffer est rastérisé sur notre propre device plutôt qu'importé de Dawn :
on maîtrise les formats, on obtient de vraies normales géométriques, et la
seule donnée à récupérer d'Aurora est la matrice de caméra.

## Les quatre difficultés réelles

**Décodage des formats de sommets.** GX stocke les positions en virgule fixe
`s16` avec un nombre de bits fractionnaires variable, ou en `f32`, avec des
strides différents (`gx.hpp:439`, champs `compType` et `frac`). Il faut
normaliser en `float3` avant toute construction d'accélération.

**Le skinning PNMTX.** Les positions sont en espace modèle, transformées dans le
vertex shader par une matrice indexée *par sommet*. On ne peut donc pas se
contenter de poser la transformation dans l'instance TLAS : il faut transformer
les positions avant de construire, puis reconstruire les BLAS chaque frame.

Mesuré au jalon 3 : **2,05 ms en moyenne** pour 53 518 triangles, soit environ
12 % d'une frame à 60 Hz. C'est abordable, mais pas négligeable — l'estimation
initiale de ce document était trop optimiste. La mesure inclut par ailleurs une
attente GPU bloquante à chaque construction ; la pipeliner ramènerait ce coût
sous le seuil du visible.

**Synchronisation inter-device.** `SharedTextureMemory` + `SharedFence` côté
Dawn ; sans cela on obtient du tearing ou une frame de retard.

**Ce qui est traçable.** Le jeu n'a ni matériaux PBR ni métadonnées d'émissivité.
Viser l'occlusion ambiante et les ombres portées, pas de l'illumination globale
physiquement correcte.

## Jalons

| # | Objet | Vérification | État |
|---|---|---|---|
| 1 | Device D3D12 privé + capacités DXR 1.1 | Log au démarrage | **fait** |
| 2 | Dérivation de la géométrie, dump d'une frame en `.obj` | Ouverture dans Blender | **fait** |
| 3 | BLAS/TLAS depuis la géométrie captée | Statistiques de compteur | **fait** |
| 4 | ~~G-buffer~~ → visibilité primaire tracée | Visualisation debug | **fait** |
| 5 | RayQuery : ombres portées | Comparatif visuel | **fait** |
| 6 | AO tracée | Comparatif visuel | **fait** |
| 7 | Composition + synchronisation | Absence de tearing | **fait** |

### Jalon 1 — résultat

`lib/rt/rt_device.cpp`, appelé depuis `webgpu/gpu.cpp` après création du device
Dawn. L'adaptateur est apparié par vendor/device ID plutôt que choisi par
défaut, pour garantir que BLAS et textures partagées vivent sur le même GPU.

```
[INFO | aurora::rt] Ray tracing device ready: NVIDIA GeForce RTX 5090
                    (32187 MB VRAM), tier 1.1, inline RayQuery yes
```

Tier 1.1 disponible : le chemin `RayQuery` en compute shader est viable, les
tables de shaders ne sont pas nécessaires. L'échec d'initialisation n'est jamais
fatal — il désactive les passes RT et laisse le rendu inchangé.

Simplification trouvée pour le jalon 2 : `comp_type_size` et `comp_cnt_count`
sont déjà publics dans `gx/gx.hpp`. Les offsets d'attributs peuvent donc être
calculés dans le module RT, sans refactoriser `calculate_last_vtx_size`.

### Jalon 2 — résultat

`lib/rt/rt_capture.cpp`, branché dans `draw_prim` après `push_verts`. Positions
décodées depuis leur format GX puis transformées par la matrice de position du
sommet, produisant des triangles en espace vue.

Deux réglages par variable d'environnement :

- `AURORA_RT_DUMP_FRAME=N` — dump la frame N
- `AURORA_RT_DUMP_MIN_TRIS=N` — dump la première frame d'au moins N triangles

Le second existe parce que le premier ne sert à rien en pratique : le compteur
de frames tourne sans limite pendant le boot, si bien que les frames 300 et 1800
tombaient toutes deux sur un quad de chargement 576x480. Chercher un seuil de
géométrie trouve la bonne frame sans avoir à deviner.

Capture obtenue sur une frame de jeu :

```
53518 triangles, 0 draw ignoré
sommets       : 160554        non finis     : 0
bbox X        : -294.277 -> 640.000
bbox Y        : -190.020 -> 481.000
bbox Z        : -1021.665 ->   0.000
dégénérés     : 0 (0.0 %)     plans Z dist. : 19943
```

Aucune valeur non finie et aucun triangle dégénéré : le décodage des formats en
virgule fixe et le produit par les matrices `PNMTX` sont corrects. La boîte
englobante correspond à un volume de vue cohérent, `Z` négatif s'éloignant de la
caméra, `Z = 0` portant les surfaces 2D. Les 19 943 plans en `Z` distincts
confirment de la géométrie réellement tridimensionnelle et non des quads d'UI.

Le point de décision du projet est donc franchi : la géométrie est exploitable
telle quelle pour construire des structures d'accélération.

### Jalon 3 — résultat

`lib/rt/rt_accel.cpp`, espace de noms `aurora::rt::accel`. Une BLAS contenant la
frame entière en soupe de triangles non indexée, une TLAS la référençant avec une
transformation identité.

Ce choix mérite une justification. GX indexe les matrices de position *par
sommet*, donc découper la scène en BLAS par objet avec des transformations
d'instance demanderait d'abord de regrouper les sommets par matrice. Les
triangles arrivant déjà transformés depuis `rt_capture`, le problème est contourné
— au prix d'une reconstruction complète par frame.

Le skinning en compute prévu initialement s'est révélé inutile à ce stade : la
capture transforme déjà côté CPU. C'est une optimisation, pas un prérequis.

```
cold  : 172.33 ms   (création queue, allocateur, buffers incluse)
chaud : min 1.50 ms  avg 2.05 ms  max 3.57 ms   sur 120 frames
taille: BLAS 3236 KB   TLAS 2 KB   scratch 752 KB
```

`AURORA_RT_BUILD_FRAMES=N` reconstruit pendant N frames pour obtenir ces chiffres.
Il existe parce qu'une mesure unique ne dit rien : l'écart entre 172 ms et 2 ms
est entièrement de l'initialisation.

Deux réserves sur le chiffre de 2 ms. Il inclut une attente GPU bloquante par
construction, que la version finale devra pipeliner. Et la géométrie est
reconstruite intégralement alors qu'une grande partie du plateau est statique
d'une frame à l'autre — séparer statique et dynamique en deux BLAS est le gain
suivant, une fois les passes de rendu en place.

### Jalons 4 et 6 — le G-buffer est devenu inutile

Le jalon 4 prévoyait de rastériser un G-buffer profondeur et normales. Il n'y en
a pas besoin : l'inline `RayQuery` répond déjà à « qu'y a-t-il à ce pixel ». La
visibilité primaire est donc tracée, et le G-buffer supprimé du plan — avec lui
disparaît le partage de profondeur avec le device de Dawn, qui était la partie
la plus délicate de l'architecture.

`lib/rt/shaders/rt_ao.hlsl`, compilé en `cs_6_5` par dxc au build (règle CMake
dans `aurora_gx.cmake`) et embarqué en DXIL : aucune dépendance à un compilateur
à l'exécution. Le pilotage est dans `lib/rt/rt_trace.cpp`.

Deux détails d'implémentation qui ont demandé une correction :

**Pas d'inverse de projection.** Les équations de clip GX se résolvent
analytiquement — `clip.x = p0·x + p1·z`, `clip.w = -z` donne directement
`x = (ndc.x + p1)/p0` à `z = -1`. Passer les quatre paramètres XF évite d'inverser
une 4x4 et supprime tout risque d'erreur de convention ligne/colonne.

**La projection doit être lue au moment du draw.** Lue en fin de frame, elle
renvoie l'état du HUD, que le jeu bascule en orthographique après avoir dessiné
la scène — la première tentative a échoué exactement là. Les draws
orthographiques sont désormais exclus de la capture : leurs quads d'interface
occulteraient sinon la scène entière.

Résultat sur l'écran de sélection des personnages :

```
53512 triangles (3 draws orthographiques exclus)
AO 640x480, 16 échantillons, rayon 40, moyenne 0.925, 1.29 ms
histogramme : 76.9 % blanc pur, 22.9 % intermédiaire, 0.2 % noir pur
niveaux distincts : 17 / 256
```

Les 17 niveaux distincts sont la vérification arithmétique : avec 16 rayons,
`1 - occluded/16` ne peut produire que 17 valeurs. Le fond blanc est correct,
l'arrière-plan de cet écran étant en 2D donc exclu.

### Jalon 7 — moitié faite

**Ce qui fonctionne.** Aurora demande désormais les features `SharedTextureMemory`
et `SharedFence` en DXGI shared handle quand l'adaptateur les propose
(`webgpu/gpu.cpp`), et les obtient :

```
[aurora::gpu] Ray tracing interop: shared texture yes, shared fence yes
[aurora::rt]  Interop handles: texture exported, fence exported (value 1)
```

Côté D3D12, la texture de sortie est créée en `HEAP_FLAG_SHARED` avec
`ALLOW_SIMULTANEOUS_ACCESS` — ce dernier évite une transition d'état entre les
deux devices — et exportée en handle NT. Une seconde *fence* partagée est
signalée juste après le dispatch, avant l'attente CPU, pour qu'un consommateur
n'ayant besoin que du résultat GPU n'attende jamais après nous.

**L'import fonctionne.** `lib/rt/rt_interop.cpp` importe les deux handles et
ouvre l'accès sans erreur :

```
[aurora::rt] Ray tracing output imported into Dawn: 640x480
```

La passe de copie XFB reçoit un troisième binding échantillonné par
`textureLoad` — et non `textureSample`, `R32Float` n'étant pas filtrable en
WebGPU. Une texture blanche 1x1 sert de repli, ce qui évite d'avoir deux
pipelines selon que le ray tracing tourne ou non.

Un piège a coûté une itération : `g_CopyBindGroup` n'était construit que dans
`resize_swapchain`, donc bien avant le premier tracé. Il gardait le repli blanc
pour toute la session. D'où `webgpu::refresh_copy_bind_group()`, appelée une fois
la texture importée.

**Le tracé continu fonctionne.** `rt_capture` est armé à chaque frame quand le
réglage est actif, et reconstruit puis trace sans faire tomber le jeu :

```
[aurora::rt] Ray traced AO enabled
[aurora::rt] Ray tracing output imported into Dawn: 640x480
[aurora::rt] Ray tracing active: 53512 triangles, BLAS 3235 KB,
             build 145.87 ms, AO 640x480 16 samples 0.82 ms
```

**La composition fonctionne.** Elle a demandé de trouver un bind group caché.

Le symptôme : en remplaçant temporairement la composition par
`return vec4(ao, ao, ao, 1.0)`, l'écran devenait **entièrement blanc**. Le shader
lisait donc son binding — sinon on aurait vu l'image du jeu ou du noir — mais la
valeur valait 1.0 partout, c'est-à-dire la texture de repli.

La cause : `aurora.cpp` choisit `rmlBindGroup` plutôt que celui reconstruit dès
que l'overlay RmlUi est actif, et `rmlui.cpp` ne construit le sien qu'à la
création de sa cible de rendu. Il gardait donc la texture blanche pour toujours.
D'où `rmlui::refresh_copy_bind_group()`, appelée en même temps que celle de
`webgpu`.

Piège au passage : posée d'abord à côté de `ensure_render_target`, la fonction
tombait dans l'espace de noms anonyme de `rmlui.cpp` (lignes 23 à 325) et le lien
échouait sur un symbole non résolu. Elle doit être définie après.

Mesure sur l'écran-titre, même scène, seul le réglage change :

```
RT off : luminance moyenne 101.55   pixels sombres 24.06 %
RT on  : luminance moyenne  94.69   pixels sombres 30.11 %
```

Soit 6,8 % d'assombrissement, cohérent avec une AO de moyenne 0,925.

Méthode : la comparaison par luminance seule ne suffit pas — l'écran-titre est
animé, deux captures ne montrent jamais la même frame, et une première mesure
avait paru dire le contraire. C'est le shader de debug affichant l'AO seule qui
tranche ; la luminance ne sert qu'à confirmer l'amplitude ensuite.

À noter aussi : `SetForegroundWindow` est refusé à un processus qui n'a pas déjà
le premier plan, et une capture d'écran prise ainsi photographie silencieusement
la mauvaise fenêtre. `SetWindowPos` en `HWND_TOPMOST` fonctionne.

### Lecture hors limites dans la capture — corrigé

Premier retour de jeu réel : plantage (`0xC0000005`) au choix d'une sauvegarde,
ray tracing actif. Relecture de `capture_draw` :

```cpp
if (byteOffset >= array.size) { ... }   // avant
if (byteOffset + positionBytes > array.size) { ... }   // après
```

L'ancienne vérification ne contrôlait que le *début* de la position dans le
tableau d'attributs. `decode_position` lit ensuite jusqu'à 12 octets, donc une
position située près de la fin du tableau lisait au-delà. Les formats de sommets
de l'écran-titre passaient par chance ; ceux du jeu réel sont plus variés.

Deuxième garde ajoutée dans la foulée : `draw_prim` peut réutiliser une taille de
sommet mise en cache (`g_gxState.lastVtxSize`) alors que `compute_layout`
recalcule les offsets à neuf. Si les deux divergent, les lectures sortent du flux
de sommets. Les offsets sont désormais validés contre `vtxSize` avant la boucle.

### Qualité de l'échantillonnage

Trois changements empruntés aux techniques éprouvées ailleurs, tous dans le
shader, tous à coût nul.

**Bruit à gradient entrelacé** (Jimenez, 2014) à la place d'un hash. Un hash
disperse les échantillons au hasard, donc des pixels voisins peuvent tomber
d'accord par accident et former des taches. L'IGN répartit l'erreur selon un
motif écran que l'œil lit comme un grain uniforme — et qu'un filtre spatial
nettoie bien mieux, si on en ajoute un plus tard.

**Séquence de Hammersley** au lieu de nombres indépendants. Une suite à faible
discrépance couvre l'hémisphère beaucoup plus régulièrement à nombre de rayons
égal ; la rotation par IGN décorrèle les pixels voisins.

**Atténuation par distance** plutôt qu'une occlusion binaire. Un contact au loin
ne doit pas assombrir autant qu'un contact rasant : la contribution est pondérée
par `(1 - t/rayon)²`. C'est ce qui donne à l'AF de référence son dégradé doux au
lieu du halo plat que produit un simple comptage.

Le nombre d'échantillons passe de 16 à **48**, la mesure ayant montré que les
rayons sont quasi gratuits depuis la suppression de la lecture retour.

```
AO 1920x1440, 48 échantillons — 0.38 ms   (identique à 16 échantillons)

Énergie haute fréquence sur la zone de jeu (Laplacien) :
sans ray tracing      : 10.09   ← plancher
16 échantillons, hash : 14.39   (+4.30)
48 échantillons, IGN  : 11.09   (+1.00)
```

Le bruit *ajouté par le ray tracing* baisse donc de 77 %.

### Illumination indirecte — infrastructure faite, sans matière à colorer

L'AO seule est un assombrissement de quelques pour cent : subtil par nature, ça
ne se lira jamais comme du ray tracing. Ce qui se lit, c'est la lumière indirecte
colorée — un rayon qui rapporte la couleur de ce qu'il touche.

Implémenté : capture de `GX_VA_CLR0` avec les positions (tous les formats GX :
RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8), second buffer GPU, interpolation
barycentrique au point d'impact, sortie passée de `R32Float` à `RGBA16Float` et
composition multipliant trois canaux.

**Le résultat est gris.** Diagnostic ajouté pour trancher plutôt que supposer :

```
Draws carrying vertex colour: 16 of 1296
```

1,2 %. Les modèles de Mario Party 4 sont **texturés**, pas colorés par sommet. Le
rebond n'a rien à teinter et retombe sur son blanc par défaut.

**Résolu par la couleur moyenne des textures.** Échantillonner les textures par
point d'impact demanderait de transférer tout le cache de textures sur le device
RT. Mais la lumière indirecte est un signal basse fréquence : la couleur *moyenne*
d'une texture en porte l'essentiel.

`TextureRef` gagne donc un `averageColour`, calculé une seule fois dans
`new_static_texture_2d`, pendant que les texels décodés sont encore accessibles —
avant qu'ils ne partent chez Dawn. `rt_capture` lit ensuite la couleur de la
texture liée au draw et la multiplie par la couleur de sommet quand il y en a une.

```
avant : Draws carrying vertex colour: 16 of 1296
après : Albedo sources: 1296 textured, 16 vertex-coloured, of 1296 draws
```

Tous les draws fournissent désormais un albédo. Coût : nul à l'exécution, la
moyenne étant calculée au chargement de chaque texture.

Limite assumée : une couleur par texture, pas par texel. Un mur bicolore rebondit
sa teinte moyenne. Pour aller plus loin il faudrait les UV et les textures sur le
device RT — le chantier bindless décrit plus haut, qui reste ouvert.

**Correction.** Le dump de debug avait été noté ici comme cassé depuis le passage
en `RGBA16Float`. Il refonctionne sans modification : le blocage observé une fois
était intermittent, pas une conséquence du changement de format. La note était
trop hâtive.

### Pourquoi le rebond reste discret

Deux hypothèses testées, la première fausse.

**Hypothèse écartée : les textures moyennées seraient grises.** Mesure ajoutée —
saturation moyenne des albédos utilisés : **0,431**. Les textures sont franchement
colorées. Ce n'est pas là que la couleur se perd.

**Ce qui se passe réellement.** L'intégration sur l'hémisphère moyenne des
dizaines de surfaces de teintes différentes, et *cette* moyenne retombe vers le
gris. C'est le comportement correct d'une illumination diffuse à un rebond, pas
un défaut d'implémentation.

S'y ajoute l'amplitude : l'occlusion vaut environ 0,2 sur la majorité de l'image,
donc la teinte ne module que ~11 % du résultat.

**La limite est dans le contenu, pas dans le code.** Le color bleeding se voit
quand deux grandes surfaces colorées se font face — un mur rouge et un sol clair.
Un écran-titre où des personnages flottent sur un fond étoilé n'offre pas cette
configuration. Le rebond y est physiquement correct et visuellement discret.

Forcer la saturation donnerait un rendu plus spectaculaire, mais ce serait
fabriquer une couleur qui n'est pas là. `AURORA_RT_BOUNCE` permet de le faire à
la demande ; le défaut reste sur ce qui est juste.

**Limites connues.**

- `EndAccess` est appelé une fois et jamais rendu ; correct tant que Dawn est
  seul lecteur, à revoir si un autre consommateur apparaît.

### L'interface ne doit pas être assombrie

L'AO appliquée dans la copie XFB finale multiplie la frame déjà composée, UI
comprise. La corriger a demandé deux tentatives.

**Ce qui n'a pas marché.** Une passe séparée avant `rmlui::record_frame`, avec
`present_source()` renvoyant le résultat : plantage en `0xC0000005` dès le boot.
`aurora::end_frame` tourne sur le thread principal alors qu'Aurora possède un
*render worker* qui détient le device ; créer un encodeur et soumettre en
parallèle ne tient pas. Annulé.

**Ce qui marche.** Replier l'AO dans `EnsureFrameRenderingStarted`
(`rmlui/WebGPURenderInterface.cpp`), juste après le blit de l'image du jeu dans
la couche de base et avant tout dessin d'UI. C'est la passe et l'encodeur de
RmlUi, donc aucune soumission concurrente. Trois ajouts contenus :

- `BlendMode::Multiply` (`dst * src`) dans `rmlui/pipeline.hpp` / `.cpp`
- `PipelineKind::AoMultiply`, dont le fragment diffuse `.r` sur RGB
- `create_copy_bind_group(source, withAo)` : RmlUi passe `false`, pour que
  l'AO ne soit pas appliquée une seconde fois à la présentation

Le shader dédié est nécessaire : la texture AO est `R32Float`, un échantillon
vaut `(ao, 0, 0, 1)`, et le blend matériel multiplie composante par composante —
sans diffusion, vert et bleu tombent à zéro et l'image vire au rouge. C'est
exactement ce qu'a montré la première capture.

```
RT off : luminance moyenne 101.55   pixels sombres 24.06 %
RT on  : luminance moyenne  92.87   pixels sombres 28.64 %
```

Zone de jeu assombrie de 8,5 %, interface inchangée.

### Jalon 5 — ombres portées

Un rayon d'ombre par pixel, dans la même passe que l'AO, vers la lumière GX la
plus lumineuse de la frame. Les lumières GX vivent en espace vue, comme la
géométrie captée : aucune transformation supplémentaire n'est nécessaire. La
position est relevée au moment du draw, pour la même raison que la projection.

Le résultat se multiplie dans le canal existant — `gOutput = ao * shadow` — donc
ni format ni passe supplémentaires.

**Un choix qui compte.** La première version assombrissait aussi les surfaces
tournant le dos à la lumière (`dot(n, L) <= 0`). C'était faux : l'éclairage
vertex du jeu s'en charge déjà, et le résultat ombrait deux fois — Waluigi
ressortait entièrement noir. Seule une occlusion réelle assombrit désormais.

L'ombre atténue à 0,55 plutôt que de noircir : le rendu d'origine porte déjà
l'essentiel de l'ombrage, et un noir franc se lirait comme un bug.

```
AO seule    : luminance moyenne 92.87   pixels sombres 28.64 %
AO + ombres : luminance moyenne 89.87   pixels sombres 31.52 %
```

Coût : 1,06 ms à 1,26 ms de tracé, pour un rayon de plus par pixel.

### Ombres douces

Huit rayons par pixel vers un disque autour de la lumière plutôt qu'un seul vers
son centre : la fraction de rayons qui l'atteignent donne la pénombre. Rayon du
disque réglable par `AURORA_RT_LIGHT_RADIUS`, 60 unités d'espace vue par défaut ;
0 rend les ombres dures.

C'est la mesure du coût par échantillon qui a rendu ce choix évident : les rayons
étant presque gratuits, passer de 1 à 8 coûte 0,38 → 0,46 ms.

```
RT off       : luminance moyenne 101.55   pixels sombres 24.06 %
AO + ombres  : luminance moyenne  89.39   pixels sombres 31.10 %
```

### Coût des structures d'accélération — refit tenté et annulé

Après l'optimisation de la lecture retour, la reconstruction devient le poste
dominant, cinq fois le tracé :

```
Steady-state sur 300 frames : min 1.41 ms, avg 2.24 ms
(le max de ~155 ms est le build à froid, création des objets comprise)
```

**Ce qui a été essayé.** DXR sait rafraîchir une BLAS existante
(`PERFORM_UPDATE`) au lieu de la reconstruire, tant que la topologie ne change
pas — bien moins cher. Refit quand le nombre de sommets est identique, rebuild
complet forcé toutes les 60 frames pour éviter la dérive de qualité.

**Pourquoi c'est annulé.** `DXGI_ERROR_DEVICE_HUNG` : le GPU se bloque. Une
première correction — ne pas passer `PERFORM_UPDATE` à la requête de tailles,
puisque c'est un modificateur de build et que la taille de scratch retournée
n'est alors pas celle d'une mise à jour — n'a pas suffi. La couche de validation
D3D12 est désactivée dans ce build (`backendValidationLevel = Disabled`,
`webgpu/gpu.cpp`), donc diagnostiquer plus loin revenait à deviner en faisant
tomber le pilote à chaque essai. Annulé après deux hangs.

**Outil de diagnostic mis en place.** `AURORA_RT_DEBUG=1` active DRED
(*Device Removed Extended Data*) sur le device RT : le pilote garde des fils
d'Ariane, et `report_device_removal()` les publie quand le device disparaît —
opération exécutée au moment du blocage, et adresse GPU en cas de page fault.

Le chemin évident, `ID3D12Debug::EnableDebugLayer()`, a été essayé et **écarté** :
il agit sur tout le processus, donc la validation stricte s'applique aussi aux
appels de Dawn, dont le device est alors perdu (`DXGI_ERROR_DEVICE_RESET`) et le
jeu tué par son gestionnaire d'erreur fatal. DRED n'impose rien à personne
d'autre et reste actif en permanence sans coût notable — vérifié : le jeu tourne
normalement avec la variable posée.

**Ce que DRED a répondu.** Tentative relancée avec les fils d'Ariane actifs. Le
hang se reproduit, et le rapport est instructif par ce qu'il ne contient pas :

```
[aurora::rt] Ray tracing device removed: 0x887a0006
(aucun breadcrumb, aucune sortie de page fault)
```

`GetPageFaultAllocationOutput` ne renvoie rien et la liste de breadcrumbs est
vide. Cela **écarte** la corruption mémoire et l'accès hors limites, qui étaient
les hypothèses de travail. `DEVICE_HUNG` sans page fault est un TDR : le GPU
dépasse le délai imparti, l'opération de mise à jour tourne sans finir.

La piste à explorer n'est donc pas le dimensionnement des buffers mais la
validité de la mise à jour elle-même — en premier lieu la mise à jour *sur
place* (`SourceAccelerationStructureData == Dest`) combinée au partage du buffer
de scratch avec la construction TLAS de la même liste de commandes. Séparer les
deux scratchs est le premier essai à tenter.

Abandonné après trois hangs : le coût de chaque itération est un blocage du
pilote, et le gain visé (2,2 ms) ne le justifie pas sans une piste plus sûre.

### Correction d'instrumentation

`AURORA_RT_BUILD_FRAMES` n'était plus lu depuis la réécriture de `end_frame`
pour le tracé continu : `g_buildFrames` restait à 0 et `record_build_time` ne
publiait jamais son résumé. Le code paraissait présent mais ne mesurait rien.
Restauré — c'est lui qui a fourni les chiffres ci-dessus.

### Le HUD du jeu reste ombré — tentative annulée

Le HUD dessiné par le jeu lui-même (draws orthographiques GX) reçoit encore
l'occlusion de la scène 3D derrière lui. La correction de l'interface décrite
plus haut ne concerne que l'overlay RmlUi.

**Ce qui a été essayé.** Capter les triangles orthographiques, les replacer
devant la caméra dans le frustum perspectif, les ajouter à la structure
d'accélération après la scène, et laisser le shader ignorer tout pixel dont le
triangle touché dépasse l'indice de séparation.

**Pourquoi ça échoue.** Les quads orthographiques de cet écran couvrent toute la
surface : ce sont des calques translucides. La structure d'accélération les
traite comme opaques, donc le masque supprimait l'AO sur l'image entière. Sans
l'alpha des textures, un rayon ne peut pas distinguer un pixel de HUD réellement
opaque d'un calque transparent, et aucune heuristique géométrique ne sauve ce
cas. Annulé.

**Ce qu'il faudrait.** Appliquer l'AO au framebuffer entre le dernier draw
perspectif et le premier draw orthographique, c'est-à-dire insérer une passe de
composition dans le flux de commandes GX enregistré par `gfx`. Même principe que
la correction RmlUi, mais à l'intérieur du rendu du jeu — ça demande un point
d'insertion que `gfx` n'expose pas aujourd'hui.

### Optimisation — la lecture retour coûtait tout

L'AO était tracée à 640x480 fixes puis échantillonnée au point à la composition,
ce qui la rendait visiblement grossière. La passer à la résolution de rendu du
jeu (1920x1440 ici, plafonnée à 1920 de large) a fait passer le tracé de 1,26 à
5,15 ms.

Avant d'optimiser à l'aveugle, mesure du coût par nombre d'échantillons :

```
 4 échantillons : 4.49 ms
 8 échantillons : 4.61 ms
16 échantillons : 4.96 ms
```

Quasi plat. Les rayons n'étaient donc pas le goulot : le coût venait de la
**lecture retour**, qui rapatriait 11 Mo de texture vers le CPU à chaque frame et
attendait le GPU — pour une moyenne servant uniquement au log de debug.

La copie, l'attente et la boucle CPU ne sont désormais faites que lorsqu'un dump
BMP est demandé. Dawn se synchronise déjà par la *fence* partagée ; bloquer le
CPU ici ne servait qu'à sérialiser.

```
avant : AO 1920x1440, 16 échantillons — 5.15 ms
après : AO 1920x1440, 16 échantillons — 0.38 ms
```

Treize fois moins cher, et la haute résolution coûte maintenant moins que
l'ancienne version en 640x480. Le nombre d'échantillons étant devenu presque
gratuit, il reste à 16.


Le jalon 2 est le point de décision : si la géométrie sort correctement dans
Blender, le reste n'est que de l'ingénierie ordinaire. S'il achoppe sur les
formats de sommets, le coût du projet change d'ordre de grandeur.

## Ce que ça donnera

De vraies ombres de contact et une occlusion correcte, y compris pour la
géométrie hors champ — ce qui distingue exactement cette approche des shaders
ReShade en espace écran actuellement installés.

Ça ne transformera pas le rendu en path tracing moderne : l'éclairage du jeu
reste vertex-lit et ses textures sont en 2002.

## Débruitage et normales (8 septembre 2026)

Deux défauts distincts se cachaient l'un derrière l'autre.

### Le grain

48 rayons par pixel laissent du bruit de Monte Carlo : l'estimateur converge en
1/racine(N), donc le nettoyer par le nombre de rayons coûterait des centaines de
rayons. Un filtre à-trous à évitement d'arêtes (Dammertz et al., 2010) le fait
pour 0,07 ms : quatre passes d'un noyau 3x3 espacé de 1, 2, 4 puis 8 pixels,
chaque voisin pondéré par son accord en normale et en profondeur avec le pixel
central. `rt_ao.hlsl` écrit ces deux grandeurs dans un tampon guide (`u1`) ; la
profondeur vient du rayon primaire lui-même, pas d'un tampon rastérisé.

Réglage contre-intuitif : durcir le test d'arête à chaque passe
(`pow(dot, 32..256)`) ne filtrait que 25 % du bruit, parce que les normales
étaient géométriques et que deux pixels d'une même surface courbe diffèrent déjà
de l'angle de facette. Un exposant constant de 12 en filtre 36 %.

### Les facettes

Le vrai coupable de l'aspect « sale » n'était pas le bruit mais l'ombrage par
polygone : une normale géométrique est constante sur un triangle, donc le test
`dot(n, versLumière) > 0` fait basculer le triangle entier d'un coup. Aucun
filtre ne répare ça — la donnée d'entrée est en marches d'escalier.

`GX_VA_NRM` est maintenant décodé comme les positions (mêmes types en virgule
fixe ; les variantes NBT rangent la normale en premier), pivoté par la matrice
de normales `pnMtx[].nrm`, et interpolé par les barycentriques que `RayQuery`
fournit déjà. Repli sur la normale géométrique quand le draw n'en portait pas.
Le décalage anti-auto-intersection reste appliqué le long de la normale
géométrique : une normale interpolée fortement inclinée ramènerait l'origine
sous la surface.

### La taille angulaire de la lumière

Les ombres douces ne l'étaient pas. Mario Party 4 construit une lumière infinie
en poussant sa position à un million d'unités (`VECScale(dir, pos, -1000000)`
dans `hsfman.c`) : un disque placé là ne sous-tend plus aucun angle, et les huit
échantillons de pénombre traçaient le même rayon. Les rayons d'ombre échantillonnent
désormais un cône autour de la direction (demi-angle ~2,3°), et leur portée est
bornée par la taille de la scène au lieu d'aller jusqu'à cette position.

### Métrique, et sa limite

Le laplacien sur le tampon AO mesure le grain, mais aussi les vraies arêtes.
Il montre 5,156 → 3,314 (36 %) pour le filtre seul. Avec les normales
interpolées il remonte à 4,268, alors que l'image est nettement meilleure : les
dégradés lisses produisent plus de variation à petite échelle que les facettes
plates. Le chiffre cesse d'être comparable dès que le signal change de nature ;
au-delà de ce point, il faut regarder les images.

Mesurer sur l'image composite finale ne vaut rien : les textures du jeu écrasent
le signal, et un filtrage qui retire 36 % du bruit AO n'y déplace que 1 %.

## Matériaux, mesurés plutôt que supposés (9 septembre 2026)

L'étape « matériaux » ne consiste pas à inventer une rugosité que GX n'a pas.
Tout est dérivé de ce que le jeu déclare déjà :

- **Réfléchissant** : `tcg.src == GX_TG_NRM`, coordonnées de texture générées
  depuis la normale. C'est l'environment mapping du GameCube, donc c'est le jeu
  lui-même qui désigne la surface comme brillante.
- **Découpé / translucide** : l'alpha de la texture liée, moyenné à
  l'upload en même temps que la couleur (`averageAlpha`, `cutoutFraction`).

Deux pièges rencontrés, tous deux du même genre : **les tableaux d'état GX
gardent des entrées périmées**.

- Tester `blendMode`/`alphaCompare` classait 1296 draws sur 1296 comme
  translucides. Le jeu laisse ces états actifs pour la géométrie opaque aussi ;
  ils ne disent rien. Seul l'alpha des texels le dit.
- Balayer les huit entrées `tcgs` classait 1296 sur 1296 comme réfléchissants.
  Il faut s'arrêter à `numTexGens`.

### Ce que la mesure donne, sur l'écran-titre

| | draws | pixels |
|---|---|---|
| découpés | 14 / 1296 | 0,31 % |
| uniformément translucides | 0 | 0 % |
| environment mappés | 30 / 1296 | 12,82 % |

Le compte de draws est trompeur et il fallait la vue de diagnostic pour le voir :
30 draws sur 1296 semble négligeable, mais ces surfaces couvrent **un quart de
la surface visible** — c'est le Party Cube, le gros cube blanc central.

Conséquences directes :

- Tracer les découpes comme opaques est une approximation à 0,31 % des pixels.
  Construire un chemin de hits non opaques pour ça ne vaut pas son coût ; la
  limitation est mesurée et assumée, plus seulement documentée.
- Les réflexions, elles, porteraient sur 12,8 % de l'image. C'est ce qui les
  justifie, et le matériau par triangle est maintenant en place pour les
  alimenter.

## Réflexions (9 septembre 2026)

Un rayon par pixel réfléchissant, miroir autour de la normale d'ombrage,
coloré par les couleurs de sommets déjà présentes — les mêmes données que le
rebond indirect, donc un rayon de plus et aucune plomberie nouvelle côté tracé.
Seules les surfaces que le jeu environment-mappe lui-même sont tracées.

### Pourquoi une seconde texture

La composition faisait `couleur × ao`. Une réflexion ne s'y exprime pas :

- **additive** — invisible. Les surfaces environment-mappées de ce jeu sont
  déjà claires (le Party Cube est blanc) ; y ajouter de la lumière ne change
  rien.
- **remplaçante** — c'est ce qu'il faut, mais il faut alors la couleur du reflet
  *et* un poids, à côté du terme multiplicatif. Six canaux : plus qu'une seule
  cible RGBA.

D'où `mix(couleur × ao, reflet.rgb, reflet.a)`, avec une seconde texture
partagée. Elle voyage sur la **même barrière de synchronisation** : les deux
cibles sont écrites par le même dispatch, donc un seul signal les couvre et Dawn
n'attend qu'une fois. Son import est facultatif — sans lui, la composition n'a
simplement rien à mélanger.

### Fresnel

`0.08 + 0.92 · (1 − cosθ)^5`, approximation de Schlick. Sans elle toute la
surface paraît uniformément brillante ; avec elle le reflet se concentre sur les
angles rasants, ce que la vue de diagnostic 7 montre directement : une traînée
vive le long de l'arête haute du cube, presque rien sur la face de front.

La force est plafonnée à 0,35. Le jeu dessine déjà son propre environment map ;
il s'agit d'ancrer la surface dans son entourage réel, pas de discuter avec la
direction artistique.

### Mesure

12,8 % des pixels sont classés réfléchissants, mais seuls **4,70 %** portent
effectivement un reflet, d'intensité maximale 21/255. L'écart vient de deux
choses, toutes deux correctes : les rayons qui partent vers le ciel ne touchent
rien (le fond n'est pas dans la structure d'accélération), et Fresnel annule
presque le terme sur les surfaces vues de face.

## Structures d'accélération : ne reconstruire que si nécessaire

Le BLAS était reconstruit inconditionnellement chaque frame — le poste le plus
lourd de la passe (1,7 ms sur l'écran-titre).

**Détection.** Un FNV-1a accumulé pendant l'émission des triangles, donc sans
passe supplémentaire sur les sommets. Hash identique ⇒ le BLAS décrit encore la
scène. Le TLAS, lui, est toujours reconstruit : son instance porte la caméra,
qui change même quand la scène ne bouge pas.

**Espace de référence.** GX ne donne que `pnMtx`, déjà combinée modèle-vue :
cuire en espace vue rend donc chaque sommet dépendant de la caméra. En cuisant
relativement à la première matrice de la frame, la vue s'annule :

```
inverse(V·M₀) · (V·Mᵢ)  =  M₀⁻¹ · Mᵢ
```

La matrice de référence devient la transformation de l'instance TLAS, et le
shader ramène les sommets en espace vue avec `CommittedObjectToWorld3x4()` —
DXR la fournit, rien à suivre soi-même.

### Ce que ça donne, et ce que ça ne donne pas

| scène | réutilisations | frames caméra seule |
|---|---|---|
| sélection de mode (statique) | 3085 / 3301 | 0 |
| écran-titre (animé) | 0 / 4501 | 0 |

**Zéro frame « caméra seule » sur les deux scènes.** L'espace de référence est
correct — la vue des normales est identique au pixel près avant/après, ce qui
valide l'aller-retour — mais il ne rapporte rien ici : l'écran-titre anime sa
géométrie (3008 frames sur 4501), et la sélection de mode ne bouge pas du tout,
donc le hash simple suffisait déjà.

Il est conservé parce qu'il est gratuit (coût mesuré dans le bruit) et parce que
c'est le cas d'un plateau de jeu — décor fixe, caméra qui se déplace — qu'il
adresse. Mais ce cas n'a pas pu être atteint, donc le gain reste **non
démontré**, pas démontré nul.

## Un BLAS par groupe, l'animation dans le TLAS

La mesure précédente ayant établi que **les maillages sont rigides** (100 % des
sommets sources inchangés), la géométrie est désormais émise en espace **modèle**
et chaque draw reçoit une instance TLAS portant sa matrice `pnMtx`. L'animation
ne touche donc plus que des transformations, jamais des sommets.

### Deux erreurs en chemin

**Un seul BLAS multi-géométries instancié plusieurs fois.** Faux : une instance
pointe un BLAS entier, donc chaque transformation aurait dupliqué la scène
complète. Il faut un BLAS *par groupe*, sous-alloués dans un tampon commun.

**Scratch partagé entre les builds.** Ça impose une barrière UAV entre chacun,
ce qui sérialise le GPU : **304,9 ms** de reconstruction. En donnant à chaque
build sa propre région de scratch, ils deviennent indépendants — 185,7 ms.

### Fusion des draws consécutifs

Les draws successifs d'un même modèle partagent sa matrice et leurs triangles
sont déjà contigus : ils peuvent partager une structure. 1296 groupes → **32**,
et la mémoire redescend de 6356 Ko à 3323 Ko, soit le niveau du BLAS unique
d'origine.

### Résultat

| | avant | après |
|---|---|---|
| structures | 1 | 32 |
| coût d'une reconstruction | 1,7 ms | **0,5 ms** |
| frames réutilisées (écran-titre animé) | 0 / 4501 | **3296 / 4801** |
| dont « caméra seule » | 0 | 3296 |
| mémoire BLAS | 3235 Ko | 3323 Ko |

Sur une scène qui bouge en permanence, la structure survit maintenant à
l'animation. Le coût total de construction passe d'environ 8,2 s à 1,7 s sur
80 secondes de jeu.

### L'« observation non résolue » n'existait pas

J'avais noté que `accel::build` semblait tourner 1,4 fois par frame, et j'y
voyais deux boucles de rendu qui se contredisent. Il y a bien deux boucles —
`src/game/main.c:209` et `src/port/portmain.cpp:90` appellent toutes deux
`aurora_begin_frame`/`aurora_end_frame` — mais ce n'était pas ça.

Le chiffre venait de comparer `reusedFrames`, imprimé toutes les 300 frames,
avec un compteur de reconstructions cumulé sur **tout** le run. Deux instants
différents. Comptés directement, au même endroit :

```
end_frame calls 3901, acceleration builds 2409
reused 2402 ... 7 with geometry that actually moved
```

Sept reconstructions réelles en 3901 frames. Les 1492 frames restantes sont des
menus et des écrans de chargement, sans géométrie perspective, qui sortent avant
la construction.

Les deux compteurs sont conservés dans le code : raisonner sur le compteur de
frames pour en déduire la fréquence des constructions s'est trompé deux fois.

## Une observation non résolue

3296 réutilisations et 3443 reconstructions pour 4801 frames : `accel::build`
est appelé environ 1,4 fois par frame. Quelque chose invoque `end_frame` plus
d'une fois par image, et les deux appels ne voient pas la même géométrie, donc
ils se contredisent. C'est antérieur à ce travail — l'ancien code reconstruisait
à chaque appel aussi — mais c'est la prochaine chose à regarder.

## Le blocage GPU de l'instancing, et sa cause

Signalé comme un plantage à la validation de la sauvegarde. Reproduit, puis
réduit par bissection — **avec le ray tracing réellement actif à chaque étape** —
au commit des structures par groupe. Puis, à ce commit, par une série de
commutateurs qui isolent chaque moitié du changement :

| configuration | résultat |
|---|---|
| tout cuit, un groupe (comportement d'avant) | 26 étapes |
| instancing, mais **sans aucun tracé** | **mort** |
| instancing, avec une barrière entre chaque build | **mort** |
| 32 BLAS construits, mais TLAS à **une seule instance** | 26 étapes |

Le premier test écarte le reste du commit, le deuxième écarte la traversée, le
troisième écarte la concurrence des builds. Le quatrième désigne le coupable :
le TLAS multi-instances.

### La cause

Le test de réutilisation comparait le hash des positions. Or ces positions sont
désormais en espace **modèle**, donc indépendantes de la caméra — c'est tout
l'intérêt. Mais le **découpage en groupes**, lui, dépend des transformations :
deux draws adjacents ne fusionnent que tant que leurs matrices coïncident.

Quand la caméra bouge, le découpage change alors que le hash ne bouge pas. Les
BLAS ne sont donc pas reconstruits, mais leurs offsets sont recalculés pour le
nouveau découpage : chaque instance pointe alors sur une adresse où rien n'a été
construit. Ce n'est pas une image fausse, c'est un GPU bloqué.

Le correctif est un second hash, sur les bornes des groupes
(`firstTriangle`, `triangleCount`), exigé identique lui aussi pour réutiliser.
Les transformations en sont volontairement exclues : elles changent à chaque
frame et les inclure supprimerait toute réutilisation.

### Ce qui a été écarté en chemin

- **Charge de rendu** : bloque aussi à `internalResolutionScale = 2`, ombres 1×.
- **Plages hors limites, transformations non finies** : validées avant chaque
  construction, aucune violation.
- **Sous-allocation** : 32 groupes, 3323 Ko, 3584 à 307712 octets par BLAS,
  scratch 779 Ko — rien d'anormal.
- **Convention de matrice** : `flatten()` écrit `m0` en première ligne, et
  `transform()` — connue correcte — fait bien le produit scalaire de `m0` avec
  `(p,1)`. Cohérent avec le format ligne-majeur attendu par D3D12.
- **Matrices singulières** : un garde a été ajouté, correct mais sans effet sur
  le blocage. Le jeu laisse des slots `pnMtx` à zéro entre deux scènes et une
  instance TLAS singulière est un comportement indéfini, donc il reste.

### Deux bugs trouvés au passage

Le rebond indirect et la réflexion indexaient les couleurs avec
`CommittedPrimitiveIndex()` seul. Un index de primitive est **local à sa
géométrie** : sans l'offset d'instance, les deux lisaient la couleur d'une autre
surface. Correct tant qu'il n'y avait qu'une instance, faux depuis.

### Un piège de protocole

Mes trois premiers tests « ray tracing désactivé » avaient en réalité le ray
tracing **actif** : le maître est l'OR des trois réglages, et
`enableRayTracedReflections` était absent de la config, donc à `true` par défaut.
Trois runs interprétés à l'envers avant que ça se voie.
