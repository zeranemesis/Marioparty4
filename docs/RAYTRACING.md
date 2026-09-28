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

## Le plateau : un plantage et un variateur de lumière (9 septembre 2026)

Deux signalements le même jour : « les mini-jeux ont beaucoup de bugs graphiques
liés au ray tracing » et « le jeu plante lors de la sélection du plateau ». Ils
n'avaient rien en commun, et aucun des deux ne venait de là où je regardais.

### Ce que la trace ne rendait jamais

`trace::run()` soumettait sa passe de calcul et rendait la main sans attendre le
GPU. Le commentaire qui justifiait ce choix disait :

> Sans lecture retour, l'attente de Dawn sur la barrière partagée est la seule
> synchronisation nécessaire ; bloquer le CPU ici sérialiserait pour rien.

C'est faux deux fois. La barrière partagée ordonne le GPU, elle ne dit rien au
thread CPU. Donc au retour de `run()`, la passe tourne encore, et :

* `g_allocator->Reset()` à la frame suivante réinitialise un allocateur dont le
  GPU exécute encore les commandes. D3D12 l'interdit explicitement.
* `accel::build()` agrandit les tampons de positions, couleurs, normales et
  matériaux dès que la scène dépasse ce qui est alloué — et agrandir, c'est
  libérer l'ancien. Ces quatre tampons sont exactement ce que la passe en vol
  lit par ses SRV racine. D3D12 ne garde pas une ressource en vie parce qu'un
  travail déjà soumis s'y réfère.

Le second point explique pourquoi le plantage arrivait à un écran précis : les
menus ne faisaient jamais grandir les tampons, donc rien n'était jamais libéré
sous la passe. Le premier écran plus gros que les menus déclenche la
réallocation, et la réallocation tombe sur un usage après libération.

`trace::wait_idle()` attend la dernière soumission. Il est appelé avant
`accel::build()`, en tête de `run()` avant tout ce qui peut libérer une texture,
et dans `shutdown()`.

La fenêtre de risque a été comptée plutôt que supposée, parce qu'une attente qui
n'attend jamais signalerait un danger théorique :

```
Trace still running when the next build wanted its buffers: 2 times, 6.1 ms
```

Une à deux fois par plusieurs milliers de frames dans les menus, deux fois sur
un plateau. Rare — ce qui correspond à un plantage sur une transition d'écran
plutôt qu'à un plantage permanent. Le coût de l'attente est celui-là et pas
davantage.

### L'ombre qui éteignait le jeu

Le second signalement n'était pas un bug de mini-jeu : c'est toute la scène 3D
qui était concernée, plateaux compris. Écran identique, mesure de la luminosité
moyenne de l'image, terme par terme :

| réglage | luminosité | écart |
|---|---|---|
| ray tracing éteint | 151,3 | — |
| réflexions seules | 151,7 | 0 % |
| occlusion ambiante seule | 140,5 | −7 % |
| **ombres seules** | **84,6** | **−44 %** |
| les trois | 78,3 | −48 % |

La saturation, elle, est identique partout (0,348). L'image n'était pas délavée
comme je l'avais d'abord lu à l'œil : elle était seulement sombre. `SHADOW_DARKNESS`
vaut 0,55, donc −44 % veut dire que *chaque* pixel était entièrement à l'ombre.

La cause est un décalage entre le modèle et le contenu. La lumière de Mario
Party 4 est une lumière infinie stylisée, posée par les graphistes pour ombrer
les modèles ; sur ce plateau elle est en (−341440, 255362, 902161), soit à 1e6
unités. Les plateaux, eux, sont des pièces fermées. `shadowRange` valait 1,5 fois
la diagonale de la scène, soit 33940 unités ici : de n'importe quel pixel, le
rayon traverse la pièce et touche le plafond. Géométriquement c'est correct pour
une vraie lumière extérieure ; ce n'est pas ce que la lumière du jeu représente.

Balayage de la portée, même instant de la même scène :

| facteur | portée | luminosité | écart |
|---|---|---|---|
| 1,50 | 33940 | 84,4 | −44,2 % |
| 0,10 | 2263 | 145,7 | −3,7 % |
| 0,02 | 453 | 147,2 | −2,7 % |

La falaise est entre 0,10 et 1,50, pas entre 0,02 et 0,10 : les occulteurs
responsables sont la coque de la pièce, pas ce qu'il y a dedans. 0,10 garde tout
ce qu'un personnage, un décor ou le plateau lui-même peut projeter, et laisse
tomber le plafond. C'est le nouveau défaut ; `AURORA_RT_SHADOW_RANGE` l'écrase.

Avec les trois termes actifs, le plateau passe de −48 % à **−10,6 %**, saturation
inchangée : une contribution visible, plus un variateur.

Ce réglage a été mesuré sur un seul plateau. La portée reste une fraction de la
diagonale, donc elle varie avec la scène ; un menu, plus petit, aura des ombres
plus courtes qu'avant.

### Deux constats au passage

**Une frame peut utiliser deux projections.** Le tracé reconstruit un rayon
primaire par pixel à partir d'une seule, et c'était celle du dernier draw
perspectif de la frame — un choix arbitraire. Les menus en utilisent deux
(14253 et 32066 triangles), les plateaux une seule. C'est maintenant celle qui
couvre le plus de triangles. Là où il n'y en a qu'une, cela ne change rien.

**Les réflexions ne se voient pas.** Sur le plateau, 63 des 438 draws sont
marqués « environment mapped », donc le terme s'applique bien à 14 % de la
scène — et l'écart de luminosité mesuré est de 0,0 %. Avec `refl.a = fresnel ×
0,35` et une vue majoritairement frontale, le mélange tourne autour de 3 % :
présent, mais sous le seuil du visible. La fonctionnalité coûte un rayon par
pixel réfléchissant pour un résultat que la mesure ne distingue pas de son
absence.

### Atteindre un plateau

Rien de tout cela n'était mesurable depuis les menus. Le canal d'automatisation
ne transportait que des boutons, et les menus de mise en place de la partie
déplacent leur curseur au stick analogique en ignorant la croix : A appuyait,
mais ne choisissait rien. Deux champs optionnels de plus dans
`audio_diagnostics.enable` (`compteur port boutons frames stickX stickY`) et la
séquence va jusqu'au plateau. Les anciens écrivains, qui n'écrivent que quatre
champs, continuent de fonctionner.

## Le HUD était éclairé comme s'il faisait partie de la scène (10 septembre 2026)

Reste du signalement sur les mini-jeux. La composition multiplie l'image
*finie* — interface comprise — par le terme tracé pour la géométrie. Une
interface posée devant une zone occultée était donc assombrie par l'occlusion de
ce qu'elle cachait.

Mesuré sur l'écran de sélection des personnages du mode Mini-Game, en séparant
le panneau 2D du décor 3D visible à côté :

| | panneau 2D | scène 3D | image entière |
|---|---|---|---|
| ray tracing éteint | — | — | — |
| ray tracing, sans masque | −53,5 % | −25,4 % | −36,3 % |
| ray tracing, avec masque | **±0,0 %** | −8,1 % | −9,6 % |

Le panneau perdait plus de la moitié de sa luminosité, deux fois plus que le
décor qu'il recouvre. Sur un plateau, la boîte de dialogue perdait 22 %.

### Le masque

`capture_draw` voyait déjà passer les draws orthographiques — il les jetait. Il
en garde maintenant l'emprise à l'écran. GX stocke une projection orthographique
sous la forme `clip.x = m0[0]*x + m0[3]`, `clip.y = m1[1]*y + m1[3]` avec `w = 1`
(voir `command_processor.cpp`), donc les sommets en espace vue se projettent sans
division. Une boîte englobante par draw suffit : les éléments d'interface *sont*
des quads.

La grille fait 96 × 72 tuiles, un `uint` par tuile, envoyée au shader en t5. Un
pixel marqué sort immédiatement : blanc dans `gOutput`, zéro dans `gReflection`,
et aucun rayon tiré.

### Le piège, et pourquoi la première version ne servait à rien

Première mesure : **100 % de l'écran masqué**. Le diagnostic ajouté en même temps
que le masque — le pourcentage de tuiles couvertes — l'a dit tout de suite, ce
qui a évité de livrer un réglage qui éteignait silencieusement tout l'effet.

La cause : le calque 2D dessine un quad plein écran. Sa géométrie ne dit rien de
l'endroit où quelque chose est réellement peint, mais elle couvre tout. La
distribution le montre : sur un écran de mini-jeu, **1 draw plein écran et 45
draws bornés à 2,5 % de l'écran en moyenne**. Écarter les draws couvrant plus de
90 % de l'écran ramène la couverture à 54 % là, 18,9 % sur un plateau, et donne
les chiffres du tableau.

C'est ce quad plein écran qui, la fois précédente, m'avait fait conclure qu'un
masque de couverture ne pouvait pas marcher. Ce n'était vrai que de lui.

### Ce que ça coûte

Là où un élément 2D est translucide, le décor qu'on voit à travers perd son
occlusion. C'est le compromis assumé : une boîte de dialogue à moitié
transparente laisse voir une scène non ombrée plutôt qu'une interface assombrie
de 22 %. `AURORA_RT_ORTHO_MASK=0` rétablit l'ancien comportement.

## Un vrai mini-jeu, enfin (10 septembre 2026)

`m416Dll`, joué jusqu'à son écran de résultats, puis retour au menu. Aucune
erreur, aucun retrait de périphérique, 60 FPS.

### Ce qu'il a fallu pour y entrer

Les écrans de mise en place lisent un port de manette **par joueur** —
`HuPadBtnDown[cfg.unk6C]` dans `mgmodedll/main.c` — et la boucle ne sort que
lorsque les quatre ont confirmé. Un appui envoyé au seul port 0 ne pouvait donc
jamais la satisfaire : c'est ce qui bloquait toutes les tentatives précédentes
sur « Select the characters that will be joining in the party ». Le canal
d'automatisation portait déjà un numéro de port ; il suffisait de s'en servir, en
espaçant les quatre écritures d'au moins une frame puisqu'une seule commande est
verrouillée par sondage.

Deux autres détails appris en chemin, tous deux appliqués :

* Les menus se pilotent au **stick**, pas à la croix.
* Ils lisent le stick **sans détection de front** : une impulsion tenue huit
  frames déplaçait le curseur de huit crans.

### Mesures sur le mini-jeu

| | |
|---|---|
| triangles | 23 400 – 25 600 |
| construction | médiane 0,80 ms, p95 1,00 ms, max 2,80 ms |
| tracé | 0,05 ms en 1920 × 1440 |
| masque 2D | 9,8 % de l'écran |
| projections par frame | 2 |
| luminosité contre ray tracing éteint | **−2,3 %**, saturation identique |

La scène est une salle de château éclairée à la bougie : elle est sombre par
choix artistique, pas à cause du tracé. Les deux images sont à deux pour cent
l'une de l'autre.

### Le correctif de durée de vie valait bien plus que ce que j'avais mesuré

J'avais annoncé, sur des écrans de menu, que la passe de tracé était encore en
vol au moment où la construction suivante voulait ses tampons « une à deux fois
par plusieurs milliers de frames ». En jeu réel :

```
Trace still running when the next build wanted its buffers: 166 times, 455.4 ms
```

Et la progression est par rafales : 49, puis 67 pendant neuf cents frames sans
bouger, puis **99 occurrences en trois cents frames** — un tiers des frames — pour
415 ms d'attente. Les menus ne sollicitent pas le GPU ; un mini-jeu, si, et la
file de calcul privée passe alors après le travail de Dawn.

Autrement dit, avant le correctif, chacune de ces 166 occasions était un usage
après libération ou une réinitialisation d'allocateur interdite, concentrés
précisément sur les moments chargés — les transitions d'écran. C'est bien plus
cohérent avec « ça plante à la sélection du plateau » que le chiffre des menus ne
le laissait croire. Ce n'est toujours pas une reproduction du plantage signalé,
mais ce n'est plus le même ordre de grandeur de présomption.

## Composer avant le calque 2D, et rendre les ombres lisibles (11 septembre 2026)

Retour d'usage : « des ombres propres sans bavure ; les découpes ne sont pas
bonnes non plus sur pas mal de menus et d'éléments ; revoir la définition du
ray tracing », avec **sm64coopdx** (le fork sm64rt de DarioSamo) comme
référence — ombres, réflexions, réfractions et illumination globale tracées en
temps réel, débruiteur temps réel, DLSS et FSR 2.

### La composition multipliait l'image finie

Le terme de ray tracing était appliqué sur la frame terminée, HUD compris. D'où
le masque de couverture construit à partir des draws orthographiques pour
épargner les pixels de l'interface — et d'où les découpes : un masque construit
sur la géométrie ne voit pas l'alpha d'une texture, donc chaque rectangle
protégé était plus grand que le panneau qu'il contenait. Autour de chaque boîte
de dialogue s'étalait une bande de décor que l'ombrage n'atteignait pas, avec
une marche franche au bord.

La disposition de la frame a été **mesurée avant d'être supposée** : elle bascule
deux à trois fois entre les deux projections, mais **aucun draw 3D ne suit jamais
la dernière transition 3D vers 2D**, et seulement zéro à quatre de ses 112 à 148
draws 2D la précèdent. C'est donc là qu'il faut composer. Et comme la liste de
commandes est rejouée plus tard, la frontière est connue au moment où la passe
est scellée : la commande est **insérée** à la position déjà enregistrée, sans
rien prédire.

Le masque est éteint par défaut. `AURORA_RT_COMPOSITE=present` rétablit l'ancien
chemin pour comparer.

Deux défauts sont sortis en chemin :

- **Le terme était appliqué deux fois.** La frame rend dans plusieurs passes ;
  mesuré à exactement 2 compositions par frame, la scène 20 à 30 de luma trop
  sombre. L'une des deux était une copie EFB vers une texture du jeu, qui
  n'aurait jamais dû être ombrée. Désormais : passes résolues et hors écran
  ignorées, une composition par frame, et un compteur dit si une seconde passe
  était éligible (ça n'est jamais arrivé).
- **RmlUi appliquait l'occlusion de son côté**, quand son calque est affiché — et
  ce chemin prend **le seul canal rouge** et le diffuse, reliquat de l'époque où
  la cible était en R32Float, ce qu'elle n'est plus depuis que le rebond indirect
  porte de la couleur. Compteur d'images affiché, la scène était donc multipliée
  par du rouge seul, et les réflexions n'étaient pas appliquées du tout.

Vérifié avec un mode de diagnostic qui écrit une rampe en espace écran dans le
tampon (`AURORA_RT_DEBUG_MODE=10`) : rouge en abscisse, vert en ordonnée, une
grille aux huitièmes. Composée, la nouvelle voie restitue les deux rampes à
pleine amplitude et au bon endroit ; l'ancienne restituait le rouge et perdait
le vert — c'est ce qui a mis le `.r` en cause.

### Les ombres : une pénombre, pas du bruit

Le tampon d'ombre brut était un champ de mouchetis gris à cinquante pour cent
sur des surfaces planes entières. Tracé avec une lumière quasi ponctuelle, le
même tampon revient net et parfaitement propre : donc **pas d'acné**
d'auto-ombrage et aucun problème de décalage de rayon. Tout était de la pénombre.

Le demi-angle du cône valait 6,8 degrés, vingt-cinq fois celui du soleil. La
largeur de pénombre croît avec la distance de l'occulteur, donc les structures
lointaines du plateau projetaient des pénombres larges de centaines d'unités, et
quatre rayons ne peuvent pas les résoudre. Le commentaire qui défendait 0,12 a
été écrit quand la portée d'ombre valait encore 1,5 fois la diagonale de la
scène : presque tout était à l'ombre, il ne restait aucun bord à juger. À 0,10
de portée, le même réglage fait l'inverse.

Mesuré sur w01Dll, grain dans le tampon brut :

| réglage | grain |
|---|---|
| 0,12 — 4 échantillons (livré) | 0,0345 |
| 0,03 — 4 échantillons | 0,0221 |
| 0,03 — 8 échantillons | 0,0201 |
| 0,03 — 16 échantillons | 0,0149 |
| lumière ponctuelle | 0,0139 (le plancher) |

Resserrer le cône à 0,03 en récupère un tiers et ne coûte rien. Les rayons
d'ombre pèsent désormais plus lourd que les rayons d'occlusion dans les niveaux
de qualité : ils sont moins chers (quatre de plus coûtent 0,075 ms d'un tracé de
1,1 à 2,0 ms) et ce sont eux que l'œil lit comme propre ou non. Défaut : 12.

L'autre moitié était le filtre. L'à-trous pondérait ses échantillons par la
normale et la profondeur seules ; un bord d'ombre sur un sol plat a la même
normale et la même profondeur des deux côtés, donc rien n'arrêtait le flou, et
quatre passes portent à quinze texels. Un **poids d'accord de signal**, mis à
l'échelle de la dispersion locale — la solution de repli documentée de SVGF
quand il n'y a pas de variance accumulée — a été ajouté : là où l'estimation est
encore bruitée, tout le voisinage s'accorde et le filtre travaille comme avant ;
là où elle a convergé, la dispersion s'effondre et une vraie marche écarte
l'échantillon.

| | grain | netteté |
|---|---|---|
| sans filtre | 0,0163 | 0,1608 |
| filtre, sans le poids | 0,0108 | 0,1490 (−7,3 %) |
| filtre, avec | 0,0111 | 0,1569 (−2,4 %) |

### La définition : une échelle de l'écran, pas de la cible interne

L'échelle de tracé était une fraction de la cible de rendu interne, ce qui liait
la définition du ray tracing à un réglage qui n'a rien à voir : le même « 0,5 »
valait 1280×960 à Résolution interne 4 et 320×240 à 1. C'est maintenant une
fraction de ce qui arrive à l'écran, bornée par la cible de rendu. Vérifié : à
Résolution interne 2 comme à 4, le même plateau trace en 1280×960.

Un texel de tracé par pixel affiché est la bonne cible, et c'est mesuré. Marche
la plus franche que le tampon d'ombre peut produire, et grain qui y reste, dans
une fenêtre de 1272×958 :

| tracé | netteté | grain | coût |
|---|---|---|---|
| 640×480 (0,5×) | 0,43 | 0,0076 | 0,59 ms |
| 1280×960 (1,0×) | 0,67 | 0,0095 | 1,78 ms |
| 1920×1440 (1,5×) | 0,63 | 0,0179 | 4,28 ms |

Sous un texel par pixel, les bords sont visiblement mous. Au-dessus, ils ne sont
pas plus nets et le bruit double : l'image est ramenée à la fenêtre de toute
façon, donc un texel plus fin qu'un pixel moyenne moins d'échantillons dans ce
qui est montré, et le rayon fixe du filtre couvre moins d'image. L'échelle
s'arrête donc à 1,0 et le niveau supérieur achète des rayons à la place.

### Le rayon primaire traverse ce qui est surtout du vide

La géométrie découpée est dans la structure d'accélération comme des triangles
opaques ordinaires, donc le rayon primaire s'arrêtait sur le quad d'une bulle
même là où sa texture est transparente. Le rasteriseur avait dessiné ce qu'il y
a derrière, et ombrer le quad posait l'occlusion du quad par-dessus ce fond :
un rectangle sombre autour de chaque bulle et de chaque touffe de corail du
mini-jeu sous-marin, exactement à la forme du quad. Même famille de défaut que
le masque 2D, et même cause — de la géométrie qui tient lieu de quelque chose
que seule la texture connaît.

Le rayon primaire franchit maintenant jusqu'à quatre surfaces majoritairement
translucides. Deux corrections ont été nécessaires pour que ce test veuille dire
quelque chose : l'octet d'opacité portait la fraction de texels à demi-alpha ou
plus, qui compte les trous et rate la brume — c'est maintenant l'alpha moyen de
la texture ; et seule l'unité de texture 0 était inspectée, alors que balayer
les huit est pire encore (le tableau garde ce qui a été lié en dernier, comme
celui des texgens), donc ce sont les unités effectivement nommées par les étages
TEV actifs.

Le seuil vient de l'art du jeu et non du goût : l'alpha moyen des draws
translucides se répartit en deux populations — quelques centaines de sprites
entre 0,3 et 0,6, et une traîne d'une vingtaine de vraies surfaces de 0,6 à 0,9
— avec un creux net entre les deux. Le plancher est à 0,6.

Honnêteté sur la portée : le corail et l'essentiel de la grappe de bulles
reviennent propres, mais quelques rectangles pâles subsistent. Je n'ai pas
réussi à séparer l'amélioration de la variation d'une image à l'autre avec une
métrique — les sprites s'animent et les runs ne tombent pas sur la même image.
Ce qui est mesuré, c'est la classification ; le reste est ce que montrent les
captures. Éliminer le reste demande un vrai test alpha en traversée.

### L'accumulation temporelle ne faisait rien

Le motif d'échantillonnage était identique à chaque frame, choix fait avant qu'il
y ait une accumulation pour le converger. Une fois celle-ci en place, ce choix
lui a coûté toute sa raison d'être : une scène immobile produit la même
estimation à chaque frame, et mélanger une valeur avec elle-même converge vers
la même valeur bruitée. `lerp(x, x, a)` vaut `x`. Le filtre ne retirait aucun
bruit — il ne faisait que ralentir les changements, soit la moitié « traînée » du
compromis sans aucun de ses bénéfices.

Les deux graines portent maintenant un compteur de frames, tenu à zéro quand
rien n'accumule, pour qu'un motif ne bouge jamais sans converger.

Le taux d'acceptation de l'historique était une pure supposition ; il a sa vue
(`AURORA_RT_DEBUG_MODE=11`) : vert accepté, rouge la normale a refusé, bleu la
distance. Sur un plateau posé, **99,1 %** sont acceptés.

| réglage | grain | variation image à image |
|---|---|---|
| motif fixe, sans accumulation | 0,0112 | 0,00810 |
| mobile, alpha 0,05 | 0,0111 | 0,00751 |
| mobile, alpha 0,10 | 0,0144 | 0,00850 |
| mobile, alpha 0,15 (livré) | 0,0133 | 0,00864 |

0,05 est le seul poids qui égale l'ancien grain tout en étant plus stable.

Essayé puis abandonné : borner l'écart entre l'historique et l'estimation
courante, faute de pouvoir faire un cadrage par le voisinage dans cette passe.
À 0,15 la borne attrape le bruit d'échantillonnage plutôt que la traînée — grain
de 0,0133 à 0,0146, stabilité inchangée. Un vrai cadrage demande les voisins,
donc de faire le mélange là où on peut les lire.

### État après ces changements

Balayage de huit mini-jeux, **sept scènes distinctes, zéro erreur** : tracé de
0,50 à 1,37 ms, masque à 0 % partout, une à deux projections par frame. Sur
plateau, 1,9 ms pour 29 000 triangles.

### Ce que ces mesures ne disent pas

Presque toutes les comparaisons de cette session portent sur deux exécutions
scriptées distinctes, qui ne tombent pas sur la même image : les personnages
s'animent, l'ordre des tours est tiré au sort. Quand l'écart mesuré est large et
monotone (le grain d'ombre, les rampes de composition) ça tient ; quand il est
de l'ordre de la variation d'une image à l'autre, non, et c'est dit à chaque
fois plutôt qu'arrondi dans le bon sens. Un banc déterministe — même séquence,
même image, un seul processus qui bascule le réglage — reste à faire et rendrait
la moitié de ces réserves inutiles.

## Des bornes de scène NaN que mon « zéro erreur » ne voyait pas (15 septembre 2026)

Le balayage du 11 annonçait « sept scènes distinctes, zéro erreur ». Il ne
comptait que les lignes `ERROR`. En relisant les journaux, deux des huit scènes
avaient des bornes NaN dans certains rapports : **m402Dll (2 sur 27) et m405Dll
(2 sur 21)**, avec `AO radius -nan` et `shadow range -nan`. Un run plus ancien de
m405Dll, lumière encore à 6,84°, porte les mêmes lignes : c'est antérieur au 11.
Le regex d'entier du balayage échouait en silence sur `-nan`, et j'avais lu les
cases vides comme « pas de lumière ».

### Mécanisme et source

Les bornes sont initialisées sur le premier sommet de la frame ; s'il n'est pas
fini, toute comparaison suivante est fausse et elles restent NaN jusqu'à la frame
d'après. Le rayon d'occlusion et la portée d'ombre en dérivent et partent au
shader comme portée de chaque rayon.

Un journal ponctuel du premier triangle rejeté a donné la source : un quad
« cuit » dont la position décodée est parfaitement finie, (−600, 480, 1200), sort
NaN par l'emplacement de matrice 0. Le rasteriseur téléverse ce même tableau
`pnMtx` (`build_uniform`), donc — sauf si un draw fusionné garde un uniforme plus
ancien, ce que je n'ai pas vérifié — le jeu dessine ces sommets en NaN lui aussi.
Le quad fait 1200×960 et l'arène capturée paraît complète. Sur w01Dll, un autre
cas : des positions `GX_F32` qui décodent en 4 294 967 296, soit 2³², par rafales
de 100 à 1 400 triangles par tranche de 300 frames.

### Le correctif, et sa première version fausse

Un triangle est désormais écarté si une position n'est pas finie en espace vue,
dépasse 5·10⁵ unités de la caméra (aucun rayon n'approche : le primaire s'arrête
à 10⁵, ombre et occlusion à quelques milliers), ou n'est pas finie dans l'espace
propre du draw. Les rayons dérivés retombent sur la dernière taille de scène
finie, en seconde ligne.

La première version bornait aussi l'espace propre du draw, et **retirait 20 191
triangles réels** sur w01Dll : une bande `GX_F32` à z = 709 987 dans son propre
espace, à 477 unités devant la caméra une fois sa transformation d'instance
appliquée. Seule la finitude y est exigée maintenant ; le même plateau rejette
2 800 triangles, tous dans les rafales de valeurs aberrantes.

Retirer ces triangles ne coûte rien à l'ombrage : un triangle à sommet non fini
est déjà inactif pour DXR, donc les triangles NaN n'occultaient rien. m402Dll
traçait 25 747 triangles le 11 et 16 681 aujourd'hui, un écart du même ordre que
la dizaine de milliers rejetés par frame. Ceux à 2³², eux, étaient actifs — des
triangles larges de milliards d'unités.

### Vérification, et ce qui reste

Plateau w01Dll : aucune borne non finie ni absurde sur 10 rapports, composition
300 fois par tranche de 300 frames, aucune passe éligible en double ; m402Dll et
m405Dll : aucune borne invalide ; ray tracing éteint : PASS. Les scripts échouent
maintenant sur des bornes non finies ou au-delà de 10⁶, et sur une composition
en double.

Pas corrigé : des étendues **finies mais absurdes** passent encore une fois par
run, dans le menu de sélection de mode — 1620 × 103 840 × 127 050 dans une scène
qui mesure sinon 1620 × 275 × 3056. Un seuil fixe ne distingue pas un grand
plateau d'une valeur aberrante dans une petite scène ; il faut des bornes qui
ignorent les valeurs isolées, ce qui changerait les rayons partout et mérite sa
propre mesure.

Au passage : un balayage de vérification s'est bloqué une fois à 12 images par
seconde sur le dialogue du mode Mini-jeux, sans cause trouvée (coût de tracé
normal, aucune seconde instance du jeu) ; il ne s'est pas reproduit.

## Un banc A/B sur une même frame (15 septembre 2026)

Toutes les comparaisons de réglages de ce projet opposaient deux exécutions
scriptées, qui ne tombent jamais sur la même image. Quand l'effet était large,
ça tenait ; quand il était de l'ordre de la variation d'une image à l'autre, la
mesure ne concluait pas.

`tools/test_raytracing.ps1 -AB "shadowSamples=4"` fait tracer une frame de la
scène atteinte deux fois : B d'abord, en trace annexe qui n'avance ni le
compteur de frames ni la parité des tampons d'historique, puis A, qui reste
affiché. Les deux sans accumulation et avec le même motif d'échantillonnage ;
les tampons sont comparés en flottants bruts par
`tools/compare_raytracing_ab.ps1`. Réglables : aoSamples, shadowSamples,
lightRadius, aoRadius, bounce, denoisePhi, denoisePasses, debugMode. Le banc
compare des estimations d'une frame, pas des réglages d'accumulation.

Validé sur w01Dll :

| test | pixels différents | grain en pénombre |
|---|---|---|
| nul : B = A, 12 rayons d'ombre | 0 sur 1 228 800 | 0,02492 → 0,02492 |
| réponse connue : 4 rayons contre 12, tampon brut | 242 628 | 0,03937 → 0,05787 (+47 %) |

Deux leçons en chemin. Le premier déclencheur, un seuil de triangles, a écrit
sa paire sur la séquence titre — 53 512 triangles, plus qu'aucun plateau — : la
paire est maintenant armée par un fichier que le script crée une fois la scène
atteinte. Et sur cette image titre, débruiteur actif, 4 rayons contre 12 ne
déplaçaient le grain de l'image entière que de 0,03 % : le filtre lisse la
différence et les zones plates noient le reste. D'où le grain mesuré sur la
seule pénombre, et le tampon brut quand c'est l'échantillonnage qu'on mesure.
La netteté, elle, monte avec le bruit sur un tampon brut (+25 % ici) : ne la
comparer que sur des tampons débruités.

Au passage, non corrigé : ray tracing actif, `AURORA_RT_DUMP_FRAME=N` vide dès
la première frame et non à la frame N — `wantDump` ne vérifie jamais l'index.

## Le coût réel par frame, enfin mesuré (15 septembre 2026)

Tous les coûts annoncés jusqu'ici étaient ceux de la passe de tracé seule,
chronométrée par horodatages GPU, pendant que le jeu tournait sous vsync à
60 Hz : les « 60 FPS » de toutes les captures étaient le plafond.

`AURORA_FRAME_STATS` journalise la période de frame vue du thread principal,
d'un `end_frame` au suivant — simulation, enregistrement des commandes, et
l'attente que `begin_frame` impose quand le rendu prend du retard — sous forme de
distribution toutes les 600 frames. `tools/test_raytracing.ps1 -FrameStats
-Uncapped` coupe la vsync, demande 240 FPS, le maximum du régulateur de cadence,
et remet les deux réglages d'origine une fois le jeu fermé.

Sur la RTX 5090 :

| scène | ray tracing | moyenne | p99 | max |
|---|---|---|---|---|
| w01Dll | éteint | 4,17 ms | 4,7–4,8 | 9,7 |
| w01Dll | allumé | 6,8–7,0 ms | 7,8–8,2 | 12,0 |
| m401Dll | éteint | 4,17 ms | 4,7–4,8 | 6,0 |
| m401Dll | allumé | 7,0–8,9 ms | 8,1–11,2 | 12,2 |

Ce que ça dit, et ce que ça ne dit pas. Éteint, la boucle bute sur le plafond :
4,17 ms, c'est exactement 1/240 s, et la vraie période est inconnue et plus
basse. L'écart est donc un **plancher** du coût, pas le coût. Même ce plancher
vaut au moins 2,7 ms sur le plateau, où la passe isolée annonce 1,71 ms de GPU,
et 2,8 à 4,7 ms sur le mini-jeu, où elle annonce 0,75 ms : le chiffre isolé que
je citais sous-estime le coût réel d'au moins 1,6 fois sur l'un et 3,8 fois sur
l'autre. Où passe la différence — construction des structures, capture des
sommets sur le thread principal, synchronisation entre le périphérique privé et
Dawn — n'est pas mesuré.

À 60 Hz, soit 16,7 ms par frame, rien de tout ça ne gêne sur cette carte : le pire
p99 mesuré, 11,2 ms, tient dans la frame, et aucune frame tracée d'aucun rapport
ne dépasse le double de la médiane. Une seule carte, cependant : sur une carte
plus modeste, cet écart ne se transpose pas simplement.

## Le balayage des mini-jeux : ce qu'il a couvert, et ce qu'il n'a pas pu (15 septembre 2026)

Le balayage devait passer les 61 mini-jeux avec les nouveaux contrôles. Je l'ai
arrêté après 39 runs, pour une raison que ses propres chiffres donnaient :

- les index 0 à 14 atteignent 15 mini-jeux distincts, dans l'ordre du menu ;
- à partir de l'index 15, 22 runs sur 24 tombent sur **m456Dll**, la dernière
  entrée de la liste : le curseur bute en bas sans reboucler, et le script ne
  sait pas changer de catégorie. Deux exceptions, l'index 23 sur m416Dll et
  l'index 31 sur m401Dll, un parcours perdu.

Donc **17 mini-jeux distincts sur 61**, pas 61 : m401, m402, m403, m405 à m416,
m443 et m456. Les 22 runs restants auraient remesuré m456Dll pendant une heure.
Atteindre les autres demande que le script change de catégorie dans le menu du
mode Mini-jeux ; c'est un manque de l'outillage, noté pour la suite.

Sur ce qui a été couvert, les contrôles tiennent :

| | |
|---|---|
| runs arrivés en scène | 39 sur 39 |
| rapports de bornes non finies ou absurdes | 0 |
| composition par tranche de 300 frames | 300 partout |
| coût de la passe de tracé | 0,59 à 5,58 ms |

Dont m402Dll et m405Dll, qui produisaient des bornes NaN avant le correctif du
lot A. Une valeur sort du lot : **m443Dll à 5,58 ms**, 2,6 fois le suivant
(m414Dll, 2,12 ms). Une seule mesure, à regarder avant d'en conclure quoi que ce
soit.

Au passage, une fausse alerte de ma part : j'ai soupçonné que les grands index
dépasseraient le plafond de 60 étapes de navigation. C'était faux : le script
envoie tout le déplacement dans la liste en une étape, et chaque run atteint sa
scène vers l'étape 38, quel que soit l'index.

## Un vrai test alpha en traversée (15 septembre 2026)

Lot C1 du plan. Jusqu'ici toute la géométrie entrait dans le BLAS marquée
opaque. Une surface découpée, feuillage, bulle ou grille, bloquait donc les
rayons selon l'alpha moyen de sa texture : uniformément, sans trou. Seul le
rayon primaire franchissait ce qui est surtout du vide, par une heuristique.

### Ce qui change

- Les groupes qui contiennent des triangles **découpés et texturés** entrent
  non opaques dans le BLAS. Tout le reste reste opaque et ne coûte rien de plus.
- Pour un candidat de ces groupes, le shader lit l'alpha du texel touché dans la
  vignette 16×16 déjà liée pour les réflexions, aux coordonnées interpolées du
  triangle.
- Chaque rayon s'en sert selon ce qu'il cherche :
  - rayon primaire et réflexion : le texel est la surface s'il atteint 0,5 ;
  - ombre : la lumière perd, couche après couche, ce que chaque texel couvre ;
  - occlusion ambiante : le premier texel qui n'est pas un trou (alpha > 0,05),
    pondéré par son propre alpha.
- `AURORA_RT_ALPHA_TEST=0` rend l'ancien comportement. Le banc A/B compare les
  deux sur une même frame avec `-AB "alphaTest=0"`.

### La première version était fausse

Elle faisait un test binaire à 0,5 pour tous les rayons, surfaces translucides
comprises. Sur m401Dll, en vue d'occlusion brute, 35,9 % des pixels changeaient,
et toute la partie haute de la scène s'assombrissait.

Ma première hypothèse, les surfaces uniformément translucides, était fausse. Je
les ai exclues du test, sans effet sur m401Dll : la vue des matériaux et le
rapport de la capture (« 0 uniformly translucent ») montraient que le fond
assombri était classé *découpé*. Ses texels dépassent 0,5 sans être opaques, et
le test binaire le faisait passer d'occultant partiel à occultant total.

D'où la version livrée, où l'ombre et l'occlusion sont pondérées par l'alpha du
texel. L'exclusion des translucides reste : un voile se mélange, il ne se teste
pas.

### Vérification

| | |
|---|---|
| test nul, w01Dll, `alphaTest=1` | 0 pixel différent sur 1 228 800 |
| même frame, m416Dll, `alphaTest=0`, occlusion brute | 23,6 % des pixels changent ; luminance moyenne 0,517 sans le test, 0,701 avec |

Le script est tombé sur m416Dll et non sur m401Dll : le mini-jeu atteint varie
d'un run à l'autre. Sur m416Dll, un calque posé au-dessus du sol et classé
découpé (11 draws découpés sur 368) occultait tout le sol. Avec le test, sa
partie transparente laisse passer l'occlusion. Le trou apparaît en marches
d'escalier, parce que la vignette ne fait que 16×16.

### Coût

Mesuré sur la même build, test allumé puis coupé (`AURORA_RT_ALPHA_TEST=0`), avec
`-FrameStats -Uncapped`. Les deux runs suivent les mêmes menus au même rythme :
leurs rapports se correspondent un à un, et seuls comptent ceux qui suivent le
chargement de la scène. Coupé, le test laisse les drapeaux non opaques du BLAS en
place : l'écart mesure le travail du shader sur les candidats, pas la structure.

| scène, rapports après chargement | tracé GPU, test coupé | tracé GPU, test allumé | écart par rapport |
|---|---|---|---|
| w01Dll, 6 rapports (14 à 34 k triangles) | 0,67 à 2,07 ms | 0,72 à 2,42 ms | +0,01 à +0,35 ms |
| m401Dll, 5 rapports (26 à 29 k triangles) | 0,95 à 1,22 ms | 0,83 à 1,76 ms | −0,12 à +0,63 ms |

Sur m401Dll, deux rapports sur cinq coûtent 0,5 à 0,6 ms de plus, les autres
rien. Le quatrième compare deux frames différentes, 29 k triangles contre 26 k.

La période de frame bouge moins qu'elle ne varie d'un rapport à l'autre d'un
même run. Moyennes médianes : 6,25 ms coupé et 6,35 ms allumé sur le plateau,
6,65 et 7,12 ms sur m401Dll. Ces runs sont courts, avec cinq ou six rapports en
scène, dont deux ou trois de période.

### Ce qui reste

- **La version pondérée n'a pas été revérifiée sur m401Dll**, là où la première
  version assombrissait le fond.
- **Les rectangles des bulles de m401Dll restent.** Les bulles et le corail sont
  bien classés découpés, mais leur alpha est lu au mauvais endroit. La capture
  lit la vignette de l'unité de texture 0 aux coordonnées TEX0 brutes, sans les
  matrices de texture avec lesquelles une planche de sprites choisit son image.
  C'est le lot suivant, C1b.
- Une vignette 16×16 résout un disque de bulle, pas un grillage.

## Lire les textures comme le TEV les combine (15 septembre 2026)

Lot C1b, ouvert par ce que C1 laissait : sur m401Dll, les bulles et le corail sont
bien classés découpés, mais leur alpha remplissait des blocs rectangulaires rayés
au lieu de disques. Le test alpha lisait la vignette au mauvais endroit, et en
fait, souvent, la mauvaise vignette.

### Trois écarts avec ce que le jeu dessine

La capture lisait, pour chaque draw :

1. **la vignette de l'unité de texture 0**, alors que la classification regardait
   déjà les cartes que nomment les étages TEV actifs ;
2. **l'attribut TEX0 brut**, alors que l'étage peut lire une autre coordonnée ;
3. **sans matrice de texture**. Or c'est avec elle qu'une particule choisit son
   image dans une planche de sprites : échelle puis translation chargées dans
   `GX_TEXMTX0` (`src/game/hsfanim.c`). Sans elle, toute la planche s'étalait
   sur la bulle, d'où les bandes.

### Première version : les coordonnées, pas encore les cartes

Elle reproduisait le générateur de coordonnées du vertex shader d'aurora
(`gx/shader.cpp`) : source de l'étage, matrice de texture fixe ou par sommet,
matrice de post-transformation, division projective. Pour la carte, elle gardait
« la plus transparente que nomment les étages ».

Pour le vérifier, une vue de débogage lit la vignette touchée par le rayon
primaire, là où la lisent le test alpha et les réflexions :
`AURORA_RT_DEBUG_MODE=12`, ou `-AB "debugMode=12"` pour l'avoir sur la frame du
banc pendant que la fenêtre montre le jeu. `AURORA_RT_TEV_TEXTURES=0` rend
l'ancienne lecture sur le même binaire.

Sur le plateau w01Dll, la vue v1 peignait en bleu uni l'anneau du looping, les
bandes des rails et les poteaux. J'y ai d'abord lu une carte d'environnement
prise pour la couleur de la surface. **Cette lecture était fragile** : le bleu
était aussi la couleur que la vue donnait à une surface *sans* vignette. Les
rayons de lumière de m401Dll, bleus eux aussi, sont très probablement de la
géométrie sans texture, à qui l'ancienne lecture prêtait la texture qu'avait
laissée le draw précédent dans l'unité 0. Le marqueur est maintenant fait de
rayures magenta et noires, qu'aucune texture du jeu ne ressemble.

### Ce que disent les étages du jeu

Le code de dessin des modèles (`src/game/hsfdraw.c`) tranche la question des
cartes. Quatre sortes d'étages échantillonnent une carte **sans lire son alpha** :
ils reprennent celui de l'étage précédent.

| étage | coordonnée | combineur d'alpha |
|---|---|---|
| toon | `GX_TG_SRTG` depuis `GX_TG_COLOR0` | `KONST × APREV` |
| surbrillance | `GX_TG_NRM`, `GX_TEXMTX7` | `APREV × A0` |
| reflet | `GX_TG_NRM`, `GX_TEXMTX8` | `APREV` |
| ombre projetée | `GX_TG_POS`, `GX_TG_MTX3x4`, `GX_TEXMTX9` | `APREV` |

Leurs cartes ne disent rien des trous. Et une surbrillance, presque transparente,
peut gagner le « plus transparent » : ce choix décidait déjà du découpage avant
C1. Sur w01Dll, 248 draws sur 396 étaient classés découpés.

### Seconde version, livrée

- **Couverture** (découpé, translucide, et donc la carte du test alpha) : la plus
  transparente des cartes dont un combineur d'alpha lit l'alpha.
- **Couleur** (albédo, réflexions) : la première carte qu'un combineur de couleur
  lit à une coordonnée de texture. Une carte générée depuis la normale ne sert
  qu'à défaut.
- La vignette tracée est celle de la couverture sur une surface ajourée, celle de
  la couleur partout ailleurs, lue à la coordonnée que son étage génère.
- Une coordonnée impossible à reproduire (source couleur, relief) ne reçoit pas
  de vignette : la moyenne de la texture vaut mieux qu'un texel pris au hasard.

### Mesuré

Test nul sur w01Dll, `alphaTest=1` contre lui-même : **0 pixel différent** sur
1 228 800.

Compteurs de la capture, par rapport de 300 frames :

| scène, lecture | draws découpés | classés d'après une autre carte | ne nommant aucune carte |
|---|---|---|---|
| w01Dll, ancienne (état de C1) | 230 sur 367 | — | — |
| w01Dll, première version | 248 sur 396 | — | — |
| w01Dll, seconde version | 53 sur 402 ; 78 sur 726 | 202 ; 262 | 62 ; 73 |
| m401Dll, ancienne | 96 sur 605 | — | — |
| m401Dll, seconde version | 92 sur 605 | 8 | 142 |

Les draws « ne nommant aucune carte » recevaient, dans l'ancienne lecture, la
texture restée dans l'unité 0 depuis le draw précédent.

Vues vignette, même binaire, ancienne lecture puis première et seconde versions :

- **m401Dll.** Les rayons de lumière n'ont aucune texture : damier gris dans
  l'ancienne lecture, la texture périmée de l'unité 0, rayures dans la seconde
  version. Les bulles qui montent lisent de petits disques.
- **w01Dll.** L'anneau du looping, les bandes des rails et les poteaux n'ont pas
  de texture non plus : vert sombre dans l'ancienne lecture, bleu en v1, rayures
  en v2. Le disque central lisait en v1 une carte générée depuis la normale
  (anneaux, arcs) ; la v2 y relit l'arc-en-ciel de sa texture de base.

Occlusion brute, test coupé contre allumé sur la même frame. Rouge : plus clair
avec le test, un trou ouvert. Bleu : plus sombre, un texel qui bloque plus que la
moyenne de sa texture.

| scène, lecture | plus clair de plus de 0,1 | plus sombre | où |
|---|---|---|---|
| w01Dll, ancienne | 11,38 % des pixels | 2,55 % | les cases des rails, mais aussi les gobelets et la structure centrale : des objets pleins que le test perçait |
| w01Dll, ancienne, second run | 9,28 % | 5,87 % | idem, et des gobelets assombris ; 317 draws découpés sur 726 |
| w01Dll, seconde version | 6,44 % | 0,91 % | les cases des rails, presque seules ; 78 draws découpés sur 726 |
| w01Dll, seconde version, second run | 6,56 % | 0,96 % | idem ; 65 draws découpés sur 404 |
| m401Dll, première version | 0,76 % | 2,76 % | anneaux des bulles ; blocs sombres en haut |
| m401Dll, seconde version | 0,81 % | 2,94 % | idem |

Ce ne sont pas les mêmes frames d'un run à l'autre, et c'est l'image qui tranche.
Sur le plateau, en ancienne lecture, le test ouvrait des objets pleins classés
découpés à tort ; en seconde version, il n'ouvre plus que les découpes des cases.

Sur m401Dll, les blocs sombres du haut viennent d'une couche de lumière du plafond,
classée découpée. Pondérée par ses texels plutôt que par sa moyenne, elle bloque
maintenant par ses cellules opaques de 16×16, et les sommets des rochers dessous
s'assombrissent par blocs.

Une paire du plateau est tombée sur une frame où aucun rayon ne touche rien : A et
B uniformément blancs, zéro pixel différent. Elle ne prouvait rien et a été
refaite ; le banc devrait signaler ce cas lui-même.

### Coût

Mesuré comme pour C1 (`-FrameStats -Uncapped`). Les rapports qui suivent le
chargement de la scène sont alignés un à un sur le run de C1.

| w01Dll | rapports | tracé GPU par rapport |
|---|---|---|
| C1, ancienne lecture | 6 | 0,74 · 0,73 · 0,72 · 2,15 · 2,42 · 1,84 ms |
| C1b, seconde version | 7 | 0,71 · 0,68 · 0,68 · 1,95 · 2,06 · 1,99 · 1,71 ms |

Aux mêmes moments du script, la seconde version coûte de 0,03 à 0,36 ms de moins,
sauf un rapport à +0,15 ms. C'est cohérent avec cinq fois moins de groupes non
opaques, mais cela reste dans l'ordre de ce qui varie d'un run à l'autre.

Pas de chiffre pour le mini-jeu. Le run a atterri sur w01Dll, et le script l'a
accepté : il prend n'importe quel plateau ou mini-jeu pour la scène visée. Ce
défaut est corrigé au lot suivant.

### Ce qui reste

- **Les couches additives ne devraient pas occulter.** Le jeu les marque :
  `HSF_MATERIAL_ADDCOL` règle `GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA,
  GX_BL_ONE, …)` (`hsfdraw.c`), et les particules font de même (`hsfanim.c`).
  Une surface qui ajoute de la lumière ne peut pas en retirer ; aurora expose
  l'état de mélange, la capture pourrait s'en servir. C'est très probablement le
  cas du plafond de m401Dll.
- Les rayons de lumière de m401Dll, translucides par leur couleur de sommet et
  sans texture, restent tracés opaques : rien dans leur texture ne les classe.
- La vignette fait 16×16 ; la division projective se fait par sommet, exacte pour
  les générateurs affines des sprites, approchée pour une projection.
- Sur une surface ajourée à plusieurs cartes, les réflexions lisent la couleur de
  la carte de couverture.

## Deux façons dont les tests passaient sans rien prouver (15 septembre 2026)

Les deux sont apparues pendant la validation de C1b.

- **La scène atteinte n'était pas vérifiée.** `tools/test_raytracing.ps1` prenait
  n'importe quel plateau ou mini-jeu pour la scène qu'il cherchait. Le run de coût
  « mini-jeu » de C1b a atterri sur w01Dll, l'a mesuré, et a affiché PASS. La
  cible compte désormais : `^m\d` pour `-Target minigame`, `^w\d` pour
  `-Target board`. Une autre scène fait échouer la tentative en la nommant, et le
  script réessaie.
- **Une paire A/B pouvait tomber sur une frame vide.** Une paire du plateau est
  sortie uniformément blanche : aucun rayon primaire ne touchait rien, zéro pixel
  différent. Un test nul sur une telle frame passe par construction.
  `tools/compare_raytracing_ab.ps1` signale maintenant une paire uniforme, et le
  script de test échoue dessus.

Vérifié :

- le comparateur sur cette paire uniforme réelle : avertissement, `Uniform = True` ;
- le comparateur sur une paire normale : sortie inchangée, `Uniform = False` ;
- l'analyse syntaxique du script ;
- un test nul sur w01Dll avec les deux changements : scène reconnue comme un
  plateau, 0 pixel différent sur 1 228 800, sur une vraie frame (766 543 pixels
  en pénombre), PASS.

Le cas d'une scène du mauvais type n'a pas été reproduit exprès : il dépend d'une
erreur de navigation que le script ne sait pas provoquer.

## Un banc pour les séquences : ce que l'accumulation fait d'une frame à l'autre (15 septembre 2026)

Préalable aux lots C2 et C3, qui touchent l'accumulation temporelle. Le banc A/B
ne peut rien en dire : pour comparer deux réglages sur une même frame, il trace
les deux côtés sans historique. Or l'accumulation, le cadrage par le voisinage et
la variance accumulée n'existent que d'une frame à l'autre.

### Ce que fait le banc

`AURORA_RT_SEQUENCE=N` écrit la sortie finale du tracé — après accumulation et
filtre, ce qui est composé — pour N frames consécutives, `rt_seq_000.pfm` et
suivantes. L'armement est celui du banc A/B : le fichier `rt_ab_arm`, créé par le
script quand la scène est là. Les deux bancs s'excluent, car les tracés d'une
seule frame de la paire casseraient l'accumulation que la séquence enregistre.

`tools/test_raytracing.ps1 -Sequence N` pose la variable, arme le banc, vérifie
que les N frames sont écrites, les copie dans le dossier du run et les mesure avec
`tools/measure_raytracing_sequence.ps1` :

- par frame, le grain sur l'image et sur la pénombre, défini comme dans
  `compare_raytracing_ab.ps1` ;
- par paire de frames consécutives, l'écart moyen de luminance et la part des
  pixels qui bougent de plus de 0,02. Cette part compte le scintillement, mais
  aussi tout ce qui bouge vraiment, et une scène scriptée bouge toujours ;
- sur toute la séquence, l'écart type de chaque pixel dans le temps, moyenné sur
  les pixels restés en pénombre à chaque frame.

Deux runs ne tombent jamais sur les mêmes frames : on compare des séquences prises
au même moment du script, et on lit un petit écart comme une absence d'écart.

### La référence avant la passe temporelle

Même build, avec l'accumulation actuelle : mélange à poids fixe, sans cadrage
ni variance. Douze frames consécutives, prises au moment où le script arme le
banc. Médianes sur les frames et sur les paires :

| scène | grain | grain de pénombre | écart d'une frame à l'autre | part au-delà de 0,02 | écart type temporel en pénombre |
|---|---|---|---|---|---|
| w01Dll, dès la frame 7440 | 0,0216 | 0,0233 | 0,0211 | 11,5 % | 0,0441 sur 593 235 pixels |
| m401Dll, dès la frame 7080 | 0,0217 | 0,0205 | 0,0083 | 3,2 % | 0,0237 sur 1 082 279 pixels |

**Les deux scènes bougent.** Sur w01Dll, la caméra de l'introduction survole le
plateau : l'écart grandit de paire en paire, de 0,0199 à 0,0245, et c'est surtout
du mouvement. Sur m401Dll, la caméra dérive peu, mais personnages et bulles
bougent. Écart et écart type mesurent donc bruit et mouvement ensemble. Ils ne se
comparent qu'à des séquences prises au même moment du script, et quelques pour
cent de différence n'y veulent rien dire.

Vérifié aussi, avec le banc en place :

- le test nul A/B reste à 0 pixel ;
- les trois runs ont atteint la bonne sorte de scène au premier essai.

## Une couche additive n'occulte pas (15 septembre 2026)

Lot C1c, ouvert par ce que C1b laissait visible. Sur m401Dll, une couche de lumière
du plafond, classée découpée, assombrissait les rochers dessous par blocs de
16×16 : pondérée par ses texels depuis C1, elle bloquait par ses cellules opaques.
Or cette couche ne retire pas de lumière, elle en ajoute.

### Le signal que le jeu donne déjà

Le mélange additif garde la destination entière : le résultat vaut la source
multipliée par son alpha, plus ce qui était déjà dessous. Une telle surface ajoute
de la lumière et ne peut rien cacher. Le jeu dessine ainsi ses halos, ses rayons de
lumière et ses caustiques :

- les matériaux `HSF_MATERIAL_ADDCOL` règlent
  `GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, …)` (`src/game/hsfdraw.c`) ;
- les particules font de même (`src/game/hsfanim.c`).

L'état de mélange ne dit pas en général si une surface est pleine : le jeu laisse
le mélange actif sur de la géométrie opaque. Mais un facteur de destination égal à
un dit sans ambiguïté que rien derrière n'est caché.

### Ce qui change

- La capture lit l'état de mélange de chaque draw. Un draw en `GX_BM_BLEND` avec
  `GX_BL_ONE` en destination reçoit le bit `MaterialAdditive`.
- Deux draws consécutifs ne sont plus fusionnés en un groupe si l'un est additif
  et l'autre non.
- Chaque groupe est une instance du TLAS. Les groupes additifs portent le masque
  0x02, les autres 0x01.
- Sous `FEATURE_SKIP_ADDITIVE`, les quatre rayons — primaire, ombre, occlusion,
  reflet — n'incluent que 0x01.

Le rayon primaire aussi, et c'est voulu. Le terme tracé multiplie l'image pixel
par pixel, et le rendu du jeu ajoute la couche de lumière par-dessus. Le terme qui
revient à ce pixel est donc celui de la surface derrière la couche.

`AURORA_RT_ADDITIVE=0` rend l'ancien comportement. Le banc compare les deux sur une
même frame avec `-AB "additive=0"`. La vue des matériaux peint les surfaces
additives en jaune.

### Mesuré

Occlusion brute, surfaces additives tracées contre laissées de côté, sur la même
frame. Rouge : plus clair une fois la couche retirée. Bleu : plus sombre, là où le
rayon primaire atteint maintenant la surface derrière la couche.

| scène | draws additifs | plus clair de plus de 0,1 | plus sombre |
|---|---|---|---|
| m401Dll | 15 sur 605 | 8,28 % des pixels | 2,75 % |
| w01Dll | 5 sur 382 | 1,28 % | 1,06 % |

- **m401Dll.** La vue des matériaux, surfaces additives tracées, les peint en
  jaune : ce sont les rayons de lumière et la grande lueur du plafond, 25,9 % des
  impacts primaires. Une fois retirées, les dalles des rayons de lumière
  disparaissent de l'occlusion, la bande sombre des sommets de rochers s'éclaircit,
  et les blocs du plafond partent. Le bleu se trouve derrière les rayons de
  lumière : on y voit désormais le fond, plus occulté que la couche ne l'était.
  Les rayons de lumière que je croyais « translucides par leur couleur de sommet »
  sont en fait additifs : C1c les règle aussi.
- **w01Dll.** Quelques lueurs sur la structure centrale et sur le chapiteau en
  haut à gauche ; les cases et le reste du plateau ne bougent pas.
- **Test nul** sur w01Dll : 0 pixel différent.

Coût, rapport par rapport après chargement de la scène : sur w01Dll, 0,68 à
2,06 ms contre 0,68 à 2,06 ms pour C1b (écarts de 0 à 0,05 ms). Sur m401Dll,
0,60 à 1,15 ms contre 0,83 à 1,76 ms pour le run de C1. Mais C1b s'intercale
entre ces deux builds, et seul le plateau isole C1c : aucun coût mesurable.

### Un plantage au démarrage, qui ne vient pas du rendu

Pendant la validation, le premier essai du test nul a planté au démarrage :
violation d'accès dans `HuMemMemoryFree` (`src/game/memory.c:133`), l'allocateur
du jeu, en lisant le bloc suivant d'un bloc. C'est le seul plantage du jeu que
Windows a enregistré aujourd'hui, et il est tombé sur ce build.

La cause est dans `NintendoDataDecode` (`src/REL/bootDll/main.c`). Sur PC,
`nintendoData` appelle `GetRelIncludeData`, qui charge le logo dans un tampon
alloué par `HuMemDirectMalloc`. La fonction lit ensuite deux entiers en avançant
`src`, puis appelle `HuMemDirectFree(src)` : elle libère ce tampon **8 octets
après son début**. L'allocateur lit donc un en-tête de bloc décalé de 8 octets,
dont le champ `magic` tombe dans un pointeur du vrai en-tête, et la valeur de ce
pointeur dépend de l'adresse où le tas a été placé.

- D'habitude, cet octet ne vaut pas 165. L'allocateur écrit alors
  « HuMem>memory free error », s'arrête, et le tampon n'est jamais libéré. C'est
  le cas dans les 148 journaux de runs d'aujourd'hui, ray tracing coupé compris.
- Quand l'adresse du tas donne 165 à cet octet, le contrôle passe, et
  l'allocateur suit des pointeurs faux jusqu'à la violation d'accès.

Dix démarrages de ce même build, journaux conservés, ont donné un plantage : le
quatrième, à la même adresse. Son journal s'arrête juste après le chargement de
`bootDll`, là où les autres écrivent « memory free error ». Au total, ce build a
planté deux fois en dix-sept lancements. Le journal de Windows ne compte aucun
autre plantage du jeu depuis le matin, sur plusieurs dizaines de lancements des
builds précédents. Pourquoi ce build tombe plus souvent sur 165 n'est pas établi.

C'est un bug du portage, hors du ray tracing. Le correctif tient en une ligne :
libérer le pointeur rendu par `nintendoData`, et non `src` avancé de 8 octets.
Mais il touche au code du jeu, et il rejoint donc les bugs du lot D, qui attendent
une décision.

### Limites

- Seul le mélange additif est reconnu. Les couches soustractives (`GX_BM_SUBTRACT`)
  et multiplicatives assombrissent l'image sans être solides, et restent tracées.
- La décision se prend par draw, sur l'état de mélange au moment de la capture.

## Une vraie passe temporelle : cadrage par le voisinage, variance accumulée (15 septembre 2026)

Lots C2 et C3 du plan. L'accumulation mélangeait chaque frame à son historique
avec un poids fixe, dans la passe de tracé elle-même. Or cette passe calcule chaque
pixel sans voir ses voisins, d'où deux défauts :

- **rien ne bornait un historique périmé.** Une ombre qui s'est déplacée, un objet
  parti : la seule garde était le test de normale et de distance. La borne par
  pixel essayée le 11 attrapait le bruit d'échantillonnage plutôt que la traînée.
- **le filtre ne savait pas séparer le bruit d'un bord.** Le filtre à trous
  pondérait les voisins par la dispersion 3×3 de la frame, qui monte autant sur
  un vrai bord que sur du grain.

### Ce qui change

Une passe de calcul à part (`rt_temporal.hlsl`) s'insère entre le tracé et le
filtre :

- **cadrage** (Salvi, 2016) : l'historique est ramené dans la boîte des voisins
  3×3 de la frame courante, à ± 2 écarts types (`AURORA_RT_CLAMP_SIGMA`) ;
- **longueur d'historique** : moyenne vraie tant que l'historique est court, puis
  le poids fixe. Un pixel neuf se pose en quelques frames au lieu de porter vingt
  frames sa première estimation bruitée ;
- **variance** (Schied et al., SVGF, 2017) : deux moments de luminance accumulés
  donnent une variance temporelle. Un pixel à l'historique trop court prend la
  variance spatiale de son voisinage ;
- **filtre** : il lit cette variance, préfiltrée en 3×3, à la place de sa
  dispersion, et la propage d'une passe à l'autre avec le carré de ses poids.

La passe de tracé n'écrit plus que l'estimation de la frame. La passe temporelle
écrit le résultat accumulé dans la texture que le filtre reprend et que Dawn
importe, avec la variance dans l'alpha. Deux réglages permettent de comparer :

- `AURORA_RT_CLAMP_SIGMA` règle la largeur du cadrage ; 0 le coupe ;
- `AURORA_RT_VARIANCE=0` rend au filtre sa dispersion 3×3.

### Vérifié

- **Test nul A/B** sur w01Dll : 0 pixel différent. Le banc trace une seule frame,
  sans historique ; la passe temporelle ne doit rien y changer.
- **Tous les runs passent**, plateau et mini-jeu, sans erreur de tracé.
- **Vue de l'historique** (`AURORA_RT_DEBUG_MODE=11`) sur m416Dll, dernière de
  quatre frames : 72,4 % des pixels acceptés, 0,1 % refusés par la normale, 0,0 %
  par la distance, 27,4 % noirs. Le noir ne veut pas dire « pas d'historique »
  seulement : dans les vues de débogage, le tracé écrit du noir là où le rayon
  primaire ne touche rien. Ici, ce sont les parties du sol de la salle faites
  d'une couche additive, que C1c laisse de côté.
- **Coût**, rapport par rapport après chargement de la scène (tracé, passe
  temporelle et filtre compris) :
  - w01Dll : de −0,11 à +0,40 ms, les deux écarts forts sur les rapports du
    début ;
  - m401Dll : de +0,05 à +0,50 ms.

### Une première comparaison ratée

La référence de l'étape 3 ne se comparait pas à ces séquences, pour trois raisons :

- la séquence du plateau a démarré six secondes plus tard, en plein survol de la
  caméra ;
- celle du mini-jeu est tombée sur m416Dll, et non sur m401Dll ;
- la référence précède C1c, qui a retiré les rayons de lumière de m401Dll.

Les écarts mesurés — plus de variation sur le plateau, bien moins sur le
mini-jeu — ne disent donc rien de la passe temporelle.

D'où deux changements pour mesurer :

- `test_raytracing.ps1 -Scene` exige une scène précise et réessaie sinon ;
- la mesure se fait sur un seul build, sur m401Dll, dont la caméra ne bouge pas,
  en coupant tour à tour le cadrage (`AURORA_RT_CLAMP_SIGMA=0`) et la variance
  (`AURORA_RT_VARIANCE=0`).

### Mesuré, sur une scène et un build

m401Dll, douze frames depuis l'armement du banc, dans un masque de pénombre
commun aux cinq runs (72,5 % des pixels). La « pénombre calme » garde en plus les
pixels sans saut de plus de 0,05 d'une frame à l'autre dans les cinq runs, soit
59,5 % de l'image.

| réglage | écart type temporel : moyenne (p90) | pénombre calme : médiane | grain | netteté |
|---|---|---|---|---|
| par défaut | 0,0127 (0,0207) | 0,00055 | 0,0229 | 0,466 |
| par défaut, second run | 0,0156 (0,0380) | 0,00064 | 0,0220 | 0,458 |
| sans cadrage | 0,0155 (0,0378) | 0,00033 | 0,0225 | 0,469 |
| sans variance | 0,0148 (0,0354) | 0,00039 | 0,0218 | 0,460 |
| ni l'un ni l'autre | 0,0134 (0,0255) | 0,00064 | 0,0221 | 0,463 |

Le run sans variance a démarré à la frame 8100, les autres entre 6360 et 6420.

**Aucun effet mesurable.** Les deux runs du même réglage diffèrent de 23 % en
moyenne et de 83 % au 90e centile, plus que les réglages entre eux.

Les cartes d'écart type montrent pourquoi. La variation vient de ce qui bouge :
personnages, bulles, bords des rochers. Le reste de l'image tient déjà à 0,0005
près dans tous les réglages. Sur ces pixels calmes, le cadrage ajoute un peu de
bruit (médiane 0,00055, contre 0,00033 sans), le prix attendu d'un historique
qu'on empêche de traîner, et il reste invisible.

Ce banc ne mesure donc pas ce que la passe est venue régler : la traînée derrière
ce qui bouge, et la séparation du grain et des bords. Il faudrait comparer chaque
frame à une référence convergée, tracée sur la même frame ; l'erreur compterait
alors bruit et retard ensemble. C'est l'outil à construire avant les vecteurs de
mouvement (C4).

La passe est gardée pour sa structure : un mélange qui lit les voisins, là où
viendra la reprojection. Ses réglages permettent de revenir en arrière sans
rebuild, pour 0,05 à 0,5 ms par rapport.

### Ce que ça ne fait pas

- Pas de vecteurs de mouvement. Un historique ne survit que là où la même surface
  est au même pixel. Le cadrage empêche une traînée de s'installer ; il ne recolle
  pas l'historique d'un objet qui bouge. C'est le lot C4.

## Une référence convergée à côté de chaque frame (16 septembre 2026)

Le banc de séquences mesurait de combien un pixel bouge d'une frame à l'autre.
Cela mélange le bruit et le mouvement, et surtout cela ne voit pas le retard :
une accumulation qui traîne est parfaitement stable. Or c'est la traînée que le
cadrage de C3 est censé empêcher, et c'est pour cela que la mesure de C2+C3 n'a
rien pu conclure.

### Ce que fait le banc

`AURORA_RT_SEQUENCE_REF=<échantillons>` trace chaque frame de la séquence une
seconde fois : beaucoup d'échantillons, une seule frame, sans accumulation ni
filtre, écrite dans `rt_seq_ref_000.pfm` et les suivantes. Cette référence passe
avant le tracé de la frame, comme la sonde du banc A/B, pour que ce qui
s'affiche et ce qui nourrit l'historique reste le tracé normal.

L'erreur entre l'image affichée et cette référence compte d'un seul coup le bruit
d'échantillonnage et le retard de l'accumulation.

`tools/test_raytracing.ps1 -SequenceReference <n>` pose la variable, vérifie
qu'il y a autant de références que de frames et les copie dans le dossier du run.
`tools/measure_raytracing_sequence.ps1` en donne l'erreur par frame, sur l'image
et sur la pénombre de la référence, puis la médiane.

Ce que la référence n'est pas :

- elle garde un seul motif d'échantillonnage, puisqu'elle est tracée « une seule
  frame » : son propre grain est le même à chaque frame ;
- à 32 échantillons, ce grain existe encore : elle borne la mesure par le bas ;
- elle coûte cher, chaque frame vidée étant tracée deux fois.

### Ce qu'il montre sur m401Dll

Quatre réglages de la passe temporelle, douze frames chacun, plus un second run
du réglage par défaut pour borner le bruit de la mesure. Erreur moyenne contre la
référence, sur un masque commun aux cinq runs, séparé en pixels **calmes** (aucun
saut de plus de 0,05 d'une frame à l'autre dans aucun run, 46,3 % de l'image) et
pixels **qui bougent** :

| réglage | pixels calmes : moyenne (médiane) | ce qui bouge : moyenne |
|---|---|---|
| par défaut | 0,00562 (0,00439) | 0,01820 |
| par défaut, second run | 0,00798 (0,00534) | 0,02062 |
| sans cadrage | 0,00661 (0,00516) | 0,02979 |
| sans variance | 0,00573 (0,00439) | 0,01892 |
| ni l'un ni l'autre | 0,00434 (0,00303) | 0,01835 |

Deux enseignements, l'un solide, l'autre non :

- **la variance sans le cadrage coûte cher sur ce qui bouge** : 0,0298 contre
  0,018 à 0,021 partout ailleurs, bien au-delà de l'écart entre deux runs du même
  réglage (0,0182 contre 0,0206). Cela se comprend : un historique qui traîne
  garde une variance faussement basse, le filtre lui fait confiance et lisse trop.
  Les deux morceaux de C2 et C3 vont ensemble ;
- **sur les pixels calmes, le cadrage coûte un peu** : 0,0056 avec, 0,0043 sans.
  C'est le prix attendu d'un historique qu'on empêche de s'installer. Sur cette
  scène à caméra fixe, il ne rend rien en échange.

### Et sur le plateau, caméra en mouvement

Le survol de l'introduction de w01Dll, où la caméra ne s'arrête jamais : c'est là
qu'un historique périmé se voit. Masque commun aux runs retenus, 15,9 % de
l'image — il est petit parce que les runs ne tombent pas au même instant du
survol.

| réglage | erreur moyenne | pixels calmes | ce qui bouge |
|---|---|---|---|
| par défaut | 0,0325 | 0,0168 | 0,0382 |
| par défaut, second run | 0,0339 | 0,0143 | 0,0410 |
| sans cadrage | 0,0728 | 0,0269 | 0,0897 |
| ni cadrage ni variance | 0,0734 | 0,0272 | 0,0904 |
| ni l'un ni l'autre, second run | 0,0657 | 0,0312 | 0,0784 |

**Le cadrage divise l'erreur par deux dès que la caméra bouge.** Avec lui, 0,0325
et 0,0339 ; sans lui, 0,0657 à 0,0734 dans les trois runs qui s'en passent. Les
deux runs du réglage par défaut ne diffèrent que de 4 % : l'écart est bien réel.
La variance, elle, ne change rien ici — sans cadrage, avec ou sans elle, c'est la
même erreur.

C'est l'exact inverse du mini-jeu à caméra fixe, où le cadrage coûtait 0,0013 sur
les pixels calmes. Le compromis est donc celui qu'on attendait d'un cadrage, et il
penche du bon côté : petit quand rien ne bouge, décisif quand la caméra bouge.

**Ce que cela dit de C2 et C3 :** la mesure précédente, qui concluait « aucun
effet », se trompait faute d'outil. La passe temporelle vaut ce que vaut son
cadrage, et son cadrage vaut cher au bon moment.

Un run a dû être refait : sa séquence est sortie uniformément blanche, le banc
ayant armé pendant une transition, et son erreur médiane valait zéro. Les paires
A/B refusaient déjà ce cas ; les séquences ne le voyaient pas. L'outil de mesure
signale maintenant une séquence sans aucune pénombre, et le script fait échouer le
run — vérifié sur la séquence blanche et sur une bonne.

## Reprojection : chercher l'historique là où le pixel était (16 septembre 2026)

Lot C4, première moitié. La mesure contre référence a montré que le cadrage
divisait l'erreur par deux dès que la caméra bouge. C'était le signe que
l'historique était jeté presque partout : le test de normale et de distance
compare le pixel courant au même pixel de la frame d'avant, et dès que la caméra
tourne, ce n'est plus la même surface. Le vrai remède est de le chercher au bon
endroit.

### Le mouvement de caméra, mesuré depuis la géométrie

La capture travaille en espace vue : un objet qui ne bouge pas ne change de
transformation que si la caméra bouge. Pour chaque groupe présent dans les deux
frames avec le même nombre de triangles, composer l'inverse de sa transformation
courante avec celle de la frame précédente donne ce mouvement. Les objets qui ont
bougé donnent une autre réponse : la médiane, composante par composante, tranche.
Cela demande que les immobiles soient majoritaires ; là où ils ne le sont pas, le
test de normale et de distance rejette ce que la reprojection a ramené, et on
retombe sur le comportement d'avant.

**Sur combien de frames ?** Le rapport le compte : 6 525 frames tracées sur
6 745 pour un run complet, menus compris. Par tranches de 300 frames, c'est 98 à
100 % tant que la liste de draws garde la même forme, et 40 % sur la tranche où
la scène change — 1 221 draws avant, 399 après. Là, faute de correspondance, la
passe temporelle relit le même pixel, c'est-à-dire fait ce qu'elle faisait avant
ce lot ; jamais pire.

Aucune matrice de caméra n'est demandée au jeu : rien n'est supposé de sa manière
de bouger la vue.

### Ce que fait la passe temporelle

Pour chaque pixel : la position en espace vue, depuis le rayon et la distance
d'impact ; la transformation vers l'espace vue précédent ; la projection avec les
paramètres de projection de cette frame-là. L'historique, les moments et le guide
sont lus à ce pixel. La normale est tournée avec la caméra avant d'être comparée,
et la distance attendue est celle du point reprojeté, pas celle du pixel courant.

`AURORA_RT_REPROJECT=0` rend la lecture au même pixel.

### Ce que montre la vue de l'historique

Sur le survol de w01Dll, dernière frame d'une séquence de quatre
(`AURORA_RT_DEBUG_MODE=11`, vert accepté, rouge refusé par la normale, bleu par la
distance) :

| | accepté | refusé par la normale | par la distance |
|---|---|---|---|
| lecture au même pixel | 91,5 % | 8,0 % | 0,5 % |
| lecture reprojetée | 97,9 % | 2,0 % | 0,1 % |

L'image dit mieux que les chiffres : sans reprojection, **chaque silhouette du
plateau est soulignée de rouge** — rails, anneau, structures, bords de tout ce qui
se découpe. C'est exactement la signature d'un historique lu au mauvais pixel
pendant que la caméra bouge. Avec la reprojection, ces liserés disparaissent
presque tous. Elle vise donc juste.

### Ce que dit l'erreur contre la référence

Toutes les mesures sur le survol de w01Dll, douze frames, référence à 32
échantillons.

**Avec le cadrage, celui qui est livré :** 0,0386 avec la reprojection, 0,0387 et
0,0393 sans. Le grain et l'écart type temporel ne bougent pas davantage. Le
cadrage ramène l'historique dans la boîte du voisinage à chaque frame ; son
origine ne change alors plus grand-chose.

**Sans le cadrage,** là où l'historique est cru sur parole :

| | erreur moyenne | pixels calmes | ce qui bouge |
|---|---|---|---|
| lecture reprojetée | 0,0694 et 0,0607 | 0,0226 et 0,0185 | 0,0837 et 0,0736 |
| lecture au même pixel | 0,0741 et 0,0706 | 0,0281 et 0,0368 | 0,0881 et 0,0809 |

La reprojection gagne alors environ 10 % sur l'image entière et un tiers sur les
pixels calmes, dans le même sens pour les deux paires.

**Et le retard se mesure.** En comparant chaque frame affichée à la référence de
la frame t−k :

- avec le cadrage, le minimum tombe sur k = 0 dans tous les runs : rien ne traîne ;
- sans lui, il passe à k = 1 ou 2. L'image ressemble davantage à ce que la scène
  était une ou deux frames plus tôt. C'est la traînée, mesurée.

Le cadrage fait donc aujourd'hui le travail que la reprojection devait rendre
inutile, et il le fait bien. La reprojection ne le remplace pas : elle améliore
l'historique là où il est cru.

**Là où l'historique pèse davantage**, poids 0,05 au lieu de 0,15, soit une
vingtaine de frames de passé au lieu de sept : 0,0322 et 0,0328 avec la
reprojection, 0,0336 et 0,0335 sans. Trois pour cent, dans le même sens pour les
deux paires et au-delà de l'écart interne à chaque paire. Petit, mais réel.

### Ce qu'elle coûte

Deux runs sur le plateau, `-FrameStats -Uncapped`, comparés rapport par rapport.
Le temps GPU mesuré couvre le tracé, la passe temporelle et le filtre.

| | premiers rapports | rapports suivants |
|---|---|---|
| lecture reprojetée | 0,71 / 0,71 / 0,70 ms | 1,95 / 1,98 / 1,75 ms |
| lecture au même pixel | 0,73 / 0,74 / 0,72 ms | 1,95 / 1,98 / 2,07 / 1,75 ms |

L'écart va dans le sens qui ne peut pas être vrai — le run qui fait le travail en
plus est le plus rapide — et il vaut deux à trois centièmes de milliseconde : il
est sous le bruit de la mesure. La période de frame dit la même chose, 7,05 ms de
moyenne médiane contre 7,69 ms. Les deux runs ne tombent pas sur les mêmes
frames, donc la comparaison est grossière ; mais le calcul ajouté est d'une
trentaine d'opérations par pixel sur une seule passe, plus une médiane sur
quelques centaines de groupes côté processeur, et rien de cela ne se voit.

### Ce qui reste

- **Les objets qui bougent** gardent un historique rejeté : il faudrait un
  identifiant d'instance par pixel pour suivre chacun. C'est la seconde moitié
  de C4.
- La lecture reprojetée prend le pixel le plus proche, sans interpolation.
- La médiane suppose une majorité de géométrie immobile dans la frame.
- Les groupes sont appariés par leur rang et leur nombre de triangles : un draw
  qui apparaît ou disparaît décale la liste et fait échouer la mesure pour cette
  frame. Les apparier par identité la rendrait disponible pendant les
  transitions.

## Le poids de l'historique, vérifié contre une intuition fausse (16 septembre 2026)

Le lot C4 laissait une piste : à cadrage identique, l'erreur semblait tomber de
0,0386 à 0,0322 quand l'historique pèse plus longtemps. Un réglage par défaut à
changer, gratuitement, aurait été une bonne affaire.

Les deux chiffres venaient de lots lancés à des moments différents du survol.
Mis dans le même outil, ils ne partagent aucun pixel : le masque commun — les
pixels que la référence montre entre l'ombre et la lumière dans toutes les
frames de tous les runs — est vide. La comparaison ne disait rien.

### Refaite comme il faut

Trois poids, entrelacés dans un même lot pour qu'ils subissent la même dérive du
survol, deux runs chacun, et un seul masque pour les six. Tout le reste est au
réglage livré, cadrage et reprojection compris.

| poids | erreur moyenne | pixels calmes | ce qui bouge |
|---|---|---|---|
| 0,15 — livré | 0,0266 et 0,0271 | 0,0102 et 0,0094 | 0,0327 et 0,0338 |
| 0,10 | 0,0284 et 0,0301 | 0,0122 et 0,0111 | 0,0345 et 0,0372 |
| 0,05 | 0,0322 et 0,0319 | 0,0133 et 0,0140 | 0,0392 et 0,0387 |

Les paires ne se chevauchent pas d'un réglage à l'autre : le pire run à 0,15
reste meilleur que le meilleur à 0,10, et de même entre 0,10 et 0,05. C'est
l'inverse de l'intuition — plus l'historique remonte loin, pire c'est, de 9 %
puis de 20 % — et cela vaut aussi bien sur les pixels calmes que sur ce qui
bouge.

Le détecteur de retard ne départage pas : k = 0 et k = 1 sont à moins de
0,0005 l'un de l'autre à tous les poids. Sur un survol qui bouge sans arrêt, le
retard est inférieur à la frame partout.

### Et là où la caméra tient en place

Le survol bouge sans arrêt, et un historique long ne peut qu'y perdre. La même
question sur un mini-jeu, m401Dll, quatre runs, masque commun à 67 % des pixels
dont la moitié calmes :

| poids | erreur moyenne | pixels calmes | ce qui bouge |
|---|---|---|---|
| 0,15 — livré | 0,00694 et 0,00632 | 0,00426 et 0,00379 | 0,0133 et 0,0123 |
| 0,05 | 0,00763 et 0,00724 | 0,00473 et 0,00438 | 0,0145 et 0,0140 |

Même sens : l'écart entre les moyennes des deux paires, 0,0008, soit 12 %,
dépasse la dispersion interne de chacune — 0,0006 et 0,0004 — mais de peu. Le
mini-jeu confirme donc le plateau sans le durcir. Il ajoute en revanche ceci : même
sur les pixels calmes d'une caméra immobile, l'historique long ne gagne rien. La
moyenne courante a convergé bien avant vingt frames, et ce qui est gagné ensuite
sur le bruit est perdu sur la fraîcheur. Ici le retard se voit nettement : le
minimum tombe sur k = 0, et l'erreur contre la référence d'une frame plus tôt est
presque le double.

Le premier des quatre runs est tombé sur m416Dll quand les autres ont eu
m401Dll ; l'outil d'analyse l'a écarté de lui-même, et le run manquant a été
relancé avec la scène imposée.

### Ce que ça change

Rien dans le code : le poids reste à 0,15. Ce qui change est la méthode. Une
erreur contre référence ne se compare qu'à l'intérieur d'un lot entrelacé qui
partage son masque ; deux chiffres venus de deux lots ne se comparent pas, même
quand la scène porte le même nom. Le banc le dit maintenant de lui-même — il
refuse un run dont la séquence n'a pas de pénombre, ce qui a écarté deux des six
premiers runs de ce lot.

## Le garde-fou de scène ne gardait rien (16 septembre 2026)

Le 15 septembre, l'option `-Scene` avait été ajoutée pour qu'une comparaison qui
demande une scène précise refuse les runs tombés ailleurs, et je l'avais déclarée
vérifiée. Elle n'a jamais rien refusé.

En PowerShell, `$scene` et `$Scene` sont la même variable. La boucle de
navigation écrivait l'overlay trouvé dans `$scene` juste avant de construire le
motif attendu à partir de `$Scene` :

```powershell
$scene = $overlay                                        # écrase la scène demandée
$wanted = if ($Scene) { '^' + [regex]::Escape($Scene) }  # ... et le motif en vient
$reached = $overlay -match $wanted                       # l'overlay contre lui-même
```

Le test comparait donc l'overlay à lui-même, et passait toujours. Ce qui l'a
révélé : un run du lot C5, lancé avec `-Scene w01Dll`, a atterri sur **w10Dll** —
un autre plateau — et a affiché « reached w10Dll.dll ». C'est l'outil d'analyse,
qui compare la scène de chaque run à celle du premier, qui l'a écarté ; le script
de test, lui, avait dit oui.

La variable locale s'appelle maintenant `$found`, et les lignes de journal que le
rapport découpe ne s'appellent plus `$scene` non plus, pour que le paramètre ne
puisse plus être recouvert. Vérifié en demandant `-Scene m401Dll` sur une cible
`board` : « attempt 1 did not reach a board (landed on w01Dll.dll) », puis échec,
là où l'ancienne version aurait mesuré le plateau et affiché PASS.

Ce que cela change aux mesures déjà publiées : rien qui ait été affirmé sur la
foi de `-Scene` seul. Les comparaisons contre référence passent toutes par
l'outil d'analyse, qui refuse un run dont la scène diffère de celle des autres —
c'est lui qui a attrapé le run m416Dll du balayage des poids, et celui-ci. Mais
la phrase du 15 septembre qui présentait `-Scene` comme un verrou était fausse :
le verrou n'existait pas.

## Le disque de la lumière en strates, et ce que la mesure a appris de l'ombre (16 septembre 2026)

Lot C5, première moitié. L'hémisphère de l'AO est échantillonné proprement
depuis longtemps : l'échantillon *i* prend sa propre strate, et une rotation par
pixel empêche les voisins de tirer la même chose. Le disque du cône de lumière,
lui, tirait deux nombres au hasard par échantillon et par pixel. Des tirages
indépendants se groupent : avec douze rayons, un pixel peut en envoyer huit vers
l'occultant quand son voisin en envoie huit à côté, et c'est le gros grain de la
pénombre.

La correction est le même procédé que l'hémisphère : rayon stratifié — un
échantillon par anneau —, angle par inverse radicale, les deux tournés par pixel
par un bruit à gradient entrelacé pris ailleurs sur l'écran que celui de l'AO.
`AURORA_RT_SHADOW_STRATIFY=0` rend le bruit blanc, et le banc A/B compare les
deux sur une même frame avec `AURORA_RT_AB=shadowStratify=0`.

### La première mesure n'a rien montré, et c'était le plus instructif

Sur la frame d'ouverture de m401Dll : **zéro pixel différent sur 1 228 800**.
Pas un bug — la vue de l'ombre n'y prend que trois valeurs :

| valeur | part des pixels | ce que c'est |
|---|---|---|
| 0,5498 | 59,8 % | entièrement à l'ombre |
| 1,0000 | 37,7 % | en pleine lumière |
| 0,0000 | 2,4 % | le fond |

Aucun pixel entre les deux : les douze rayons d'un pixel sont toujours d'accord,
et un disque dont tous les tirages donnent la même réponse ne peut pas être mieux
échantillonné. Vérifié en le prenant par l'autre bout : sur cette même frame, **un
seul rayon d'ombre donne exactement la même image que douze**, zéro pixel
différent. Onze douzièmes du budget d'ombre n'y achètent rien.

Pour situer, la vue d'occlusion du lot C1c sur la même scène compte un millier de
valeurs distinctes et 47 % de pixels strictement entre l'ombre et la lumière.
Tout le grain de cette scène est dans l'AO, aucun dans l'ombre.

### Sur le plateau en jeu, la pénombre existe

Même banc, w01Dll après six pas de jeu — l'interface est en place, la caméra
regarde le plateau :

| A/B sur une même frame, vue de l'ombre, sans filtre | grain de pénombre A | B | pixels différents |
|---|---|---|---|
| stratifié contre bruit blanc | **0,0351** | 0,0468 | 200 789 |
| douze rayons contre un seul | 0,0360 | 0,1375 | 246 748 |

La stratification enlève **un quart du grain** de la pénombre — du terme brut,
avant le filtre ; la suite dit ce qu'il en reste dans l'image finie. Et les douze
rayons, ici, ne sont pas du gaspillage : un seul quadruple le grain. La
différence entre les deux scènes tient à la distance entre ce qui fait de l'ombre
et ce qui la reçoit : le cône ne fait qu'un degré et demi de demi-angle, donc la
pénombre ne s'ouvre que là où l'occultant est loin.

### Dans l'image que le joueur voit

La même frame tracée deux fois, filtre compris — une paire A/B tient
l'accumulation à l'arrêt par construction :

| | grain (image) | grain (pénombre) |
|---|---|---|
| stratifié | 0,02723 | 0,03178 |
| bruit blanc | 0,02737 | 0,03199 |

Un demi pour cent. Le filtre à trous enlevait déjà presque tout ce que la
stratification enlève. Sur m401Dll en jeu, l'écart tombe à 0,01 % et 0,05 %,
ce qui est cohérent avec une ombre sans pénombre.

### Sur douze frames, filtre et accumulation compris

Deux paires entrelacées sur le plateau en jeu, masque commun aux quatre runs :

| | erreur contre référence | grain | grain de pénombre |
|---|---|---|---|
| stratifié | 0,0290 et 0,0287 | 0,02295 | 0,02567 |
| bruit blanc | 0,0321 et 0,0322 | 0,02354 | 0,02604 |

Onze pour cent d'erreur en moins, les paires ne se chevauchant pas. **Mais cette
colonne-là ne se lit pas telle quelle** : la référence de chaque run est tracée
avec l'échantillonnage de ce run. Mesuré sur les références elles-mêmes, leur
grain vaut 0,0328 et 0,0328 côté stratifié contre 0,0359 et 0,0360 côté bruit
blanc — neuf pour cent. La référence stratifiée est simplement plus propre, et
l'essentiel des onze pour cent vient de là, pas de l'image montrée.

Ce que le banc peut affirmer sans cette réserve, ce sont ses mesures sans
référence : 2,5 % de grain en moins sur l'image, 1,4 % sur la pénombre. C'est
petit, c'est constant, et c'est l'ordre de grandeur de l'A/B sur l'image
composée. Le quart de grain gagné sur le terme brut ne se retrouve pas dans
l'image finale : le débruiteur en avait déjà pris la plus grande part.

### Ce que ça coûte

Deux runs sur le plateau, `-FrameStats -Uncapped`, rapport par rapport :
0,71 / 0,74 / 0,70 ms puis 1,79 à 1,84 avec la stratification, 0,72 / 0,74 /
0,73 / 0,70 puis 1,79 à 1,81 sans. Médiane de trace identique des deux côtés,
1,79 ms. La période de frame donne 6,56 ms de moyenne médiane avec contre 7,29
sans — encore une fois dans le sens qui ne peut pas être vrai, donc du bruit de
mesure. Deux divisions et une inverse radicale par échantillon : rien de
mesurable.

### Ce qui reste

- **Le nombre de rayons d'ombre pourrait suivre la scène.** Là où l'ombre est
  binaire, onze rayons sur douze ne servent à rien ; là où elle ne l'est pas, ils
  servent tous. Rien ne les compte aujourd'hui.
- **Le banc ne peut pas comparer proprement deux échantillonnages contre
  référence**, puisque chaque run trace la sienne avec le sien. Il faudrait que
  la référence soit tracée d'une manière fixe, indépendante du réglage comparé.
- **La largeur du cône est un choix, pas une mesure.** À 1,72 degrés de
  demi-angle, la pénombre reste sous le pixel partout où l'occultant est proche.
  L'élargir donnerait des ombres franchement douces — et c'est là que la
  stratification paierait le plus. Cela regarde le rendu voulu, pas le banc.

## Le budget de rayons, et une mesure qui aurait fait couper à tort (16 septembre 2026)

Le lot C5 a montré une ombre sans pénombre sur un mini-jeu — douze rayons qui
tombent toujours d'accord — pendant que l'occlusion porte tout le grain. La
question suivait : les douze rayons d'ombre sont-ils au bon endroit ?

### Ce que disait le grain

La même frame tracée deux fois, image composée, filtre compris :

| A/B | plateau : grain image / pénombre | mini-jeu |
|---|---|---|
| AO à 16 rayons au lieu de 8 | −0,27 % / −0,29 % | −0,25 % / −0,28 % |
| AO à 4 au lieu de 8 | +0,58 % / +0,73 % | — |
| ombre à 4 au lieu de 12 | +0,61 % / +0,75 % | 0,00 % / 0,00 % |

Moins d'un pour cent partout. Lue telle quelle, cette table dit qu'on peut
couper l'ombre des deux tiers sans rien perdre.

### Ce que dit la référence

Ce grain est ce qu'un flou 3×3 enlève — exactement ce que le filtre à trous
enlève aussi. Une mesure qui ne voit que ça ne peut pas voir ce que le filtre
laisse : les taches plus larges qu'un pixel, et le flou lui-même. L'erreur contre
une référence convergée voit les deux, et ici la comparaison est équitable : la
référence est tracée avec ses propres 32 échantillons d'occlusion et d'ombre,
quel que soit le budget du run comparé.

Plateau en jeu, trois budgets entrelacés, deux runs chacun, un masque commun :

| budget (AO / ombre) | erreur moyenne | pixels calmes | ce qui bouge | trace, médiane |
|---|---|---|---|---|
| 8 / 12 — livré | **0,0283 et 0,0280** | 0,0134 et 0,0133 | 0,0343 et 0,0339 | 1,80 ms |
| 8 / 4 | 0,0317 et 0,0311 | 0,0154 et 0,0164 | 0,0382 et 0,0370 | **1,23 ms** |
| 16 / 4 | 0,0291 et 0,0291 | 0,0131 et 0,0143 | 0,0355 et 0,0350 | 1,84 ms |

- **Couper l'ombre à quatre rayons** économise un tiers de la trace, et coûte
  **12 % d'erreur**. Les paires ne se chevauchent pas : le pire run du budget
  livré reste meilleur que le meilleur à quatre rayons. Le grain annonçait moins
  d'un pour cent.
- **Rendre ces huit rayons à l'occlusion** coûte autant que le budget livré et
  fait 3 % moins bien, là encore sans chevauchement.

Le budget livré est le meilleur des trois, et rien ne change dans le code.

### Ce que ça change à la méthode

Le grain d'un pixel ne juge pas un nombre de rayons. Il mesure le travail du
filtre plus que celui des rayons, et il aurait fait couper ce qui sert. Toute
comparaison de budget passe désormais par la référence, et le grain ne sert plus
qu'à ce qu'il mesure vraiment : ce que le filtre aura à enlever.

### Ce qui reste

L'ombre binaire du mini-jeu reste vraie : là, quatre rayons donnent la même
image que douze, au pixel près. Un nombre de rayons qui s'adapte au pixel —
quelques rayons d'abord, les autres seulement s'ils ne sont pas d'accord —
prendrait le tiers de trace là où l'ombre est franche et garderait les douze là
où la pénombre existe. Il faudrait pour cela que les premiers rayons couvrent le
disque à eux seuls : avec les strates actuelles, les quatre premiers occupent
quatre anneaux consécutifs — un tiers d'un seul tenant de la surface du disque,
décalé par pixel — et laissent le reste vide.

## Le balayage atteint enfin les autres catégories : 48 mini-jeux (16 septembre 2026)

Le balayage du 15 septembre s'était arrêté à 17 mini-jeux distincts : le curseur
de la liste bute en bas de la première catégorie sans passer à la suivante, et
le script ne savait pas en changer.

### Ce que fait la liste

`src/REL/mgmodedll/free_play.c` le dit directement : haut et bas déplacent le
curseur dans la catégorie courante ; **gauche et droite, ou les gâchettes L et R,
changent de catégorie**, en rebouclant, et remettent le curseur en haut de la
liste. Le script envoie donc d'abord les pas vers la droite, puis les pas vers le
bas.

Deux détails ont compté :

- la gâchette R ferait l'affaire, mais le jeu la déduit de la valeur analogique
  (`triggerRight & 0xC0` dans `pad.c`), que le canal d'automatisation ne
  transmet pas. La droite du stick et de la croix, une frame chacune, comme pour
  les pas vers le bas, suffit ;
- chaque changement fait glisser la liste pendant vingt frames, donc les pressions
  sont espacées de 900 ms pour tomber après.

### Ce que contient la liste avec cette sauvegarde

Un run par catégorie, premier mini-jeu de chacune, vérifié sur les captures :

| catégorie | entrées | premier mini-jeu |
|---|---|---|
| 0 — 4P | 16 | m401Dll, Manta Rings |
| 1 — 1vs3 | 9 | m416Dll, Candlelight Flight |
| 2 — 2vs2 | 9 | m425Dll |
| 3 — BATTLE | 6 | m404Dll, Trace Race |
| 4 — BOWSER | 3 | m435Dll, Darts of Doom |
| 5 — STORY | 5 | m445Dll, Bowser Bop |

Six pas vers la droite ramènent à 4P : l'onglet « etc. » est affiché mais n'entre
pas dans le cycle. **48 mini-jeux atteignables**, pas 61 — le reste n'est pas
proposé par ce menu avec cette sauvegarde.

Deux fautes d'outillage en route, rattrapées avant de coûter : une liste
`2,3,4,5,6` passée à un paramètre `[int[]]` à travers `powershell -File` est
arrivée comme l'entier 23456 — le piège que `sweep_raytracing.ps1` documentait
déjà, et dans lequel je suis retombé ; le run a été arrêté avant son premier pas.
Et une chaîne `"category $c: …"` que PowerShell lit comme une portée de variable.

### Trois fautes de navigation, trouvées par le balayage lui-même

Le premier passage a mesuré trois fois m416Dll depuis la liste 4P, puis laissé
trois runs sur quatre sur l'écran de règles d'un mini-jeu. Chaque fois, les
captures ont dit ce qui se passait.

1. **Le cycle générique poussait le stick sur la liste.** Une fois ses pas faits,
   le script retombait sur son cycle d'entrées par défaut, qui contient une
   poussée à droite, une vers le haut et une vers le bas. À droite, la catégorie
   changeait et le curseur revenait sur le premier jeu de 1vs3 — m416Dll. La
   reconnaissance de la liste lit en plus la couleur de l'aperçu, et celui de
   Mario Speedwagons, une route grise, ne passait pas : le script croyait avoir
   quitté la liste. Correction : une liste reconnue le reste jusqu'au changement
   d'overlay, et sur la liste le cycle garde ses boutons mais perd ses poussées.
2. **Presser A seul sur la liste laissait l'écran de règles sourd à START.** Mon
   premier correctif n'envoyait que A — sur un port, puis sur quatre — et trois
   runs sont restés bloqués sur l'écran de règles, qui n'attend que START
   (`btnDown == PAD_BUTTON_START` dans `instDll/main.c`). Revenir au rythme du
   cycle, poussées exceptées, a réglé le cas : un seul pas sur l'écran de règles,
   comme avant. **La cause exacte n'est pas établie** ; ce qui est établi, c'est
   que le cycle passe et que A seul ne passait pas.
3. **Les pas envoyés pendant que la liste glisse sont perdus.** Un run BATTLE a
   mesuré m401Dll : ses captures montrent la liste 4P immobile, curseur en haut,
   jusqu'à la confirmation. Les pas étaient partis au premier pas où la liste
   était reconnue, pendant son entrée. Correction : rien n'est envoyé avant la
   deuxième reconnaissance.

Vérifié après coup sur les trois cas qui avaient failli : BATTLE +5 atteint
m455Dll, 1vs3 +1 atteint m417Dll et en sort, 4P +14 atteint m443Dll.

### Ce que les 48 mini-jeux disent du ray tracing

Les runs qui ont dérapé sont écartés ; chaque mini-jeu est mesuré depuis sa
place dans la liste.

| | |
|---|---|
| mini-jeux atteints et mesurés | **48 sur 48** |
| rapports de bornes non finies ou absurdes | **0** |
| composition par tranche de 300 frames | 300 partout, **sauf m423Dll : 212** |
| coût de la passe de tracé | 0,77 à 4,99 ms, médiane 1,54 ms |

Par catégorie, trace en millisecondes :

- **4P** — m401 1,21 · m402 1,48 · m403 1,65 · m405 0,98 · m406 1,45 · m407 1,30 ·
  m408 1,80 · m409 1,00 · m410 1,27 · m411 0,83 · m412 1,96 · m413 1,59 ·
  m414 2,38 · m415 1,01 · m443 3,45 · m456 1,06
- **1vs3** — m416 0,77 · m417 1,55 · m418 1,88 · m419 1,00 · m420 1,86 ·
  m421 1,90 · m422 2,77 · m423 1,97 · m424 1,43
- **2vs2** — m425 1,07 · m426 1,27 · m427 2,20 · m428 1,90 · m429 4,99 ·
  m430 1,59 · m431 1,69 · m432 1,88 · m434 1,75
- **BATTLE** — m404 1,32 · m438 2,20 · m439 4,38 · m440 1,54 · m441 1,84 ·
  m455 0,78
- **BOWSER** — m435 1,51 · m436 1,32 · m437 1,31
- **STORY** — m445 0,85 · m446 0,88 · m447 1,11 · m448 1,37 · m449 2,56

Chaque chiffre vient d'un seul run, sur les frames où il est tombé : m443Dll a
donné 3,45 ms dans ce balayage, 5,95 ms à la vérification et 5,58 ms le 15
septembre. Les plus coûteux, m429Dll, m439Dll et m443Dll, sont à regarder de
près avant d'en conclure quoi que ce soit.

### Ce qui reste

- **m423Dll ne compose pas à chaque frame.** Ses rapports donnent 148 puis 212
  compositions par tranche de 300. Dans les premières, le seul draw 2D tombe au
  draw 123 sur 567 et 440 draws 3D le suivent : la composition, insérée à cette
  frontière, passerait sous l'essentiel de la scène. Et une frame sans passe
  éligible n'est pas composée du tout. À regarder avec
  `AURORA_RT_COMPOSITE_TRACE`, qui journalise chaque passe.
- **Le coût de m429Dll, m439Dll et m443Dll**, sur plusieurs runs.
- **L'onglet « etc. »** n'entre pas dans le cycle de la liste ; ce qu'il contient
  reste hors de portée du balayage.

## Une silhouette géante sur le terrain de m423Dll, et une hypothèse fausse avant la bonne (16 septembre 2026)

Le balayage des 48 mini-jeux n'a signalé qu'un écart de composition : m423Dll,
212 compositions par tranche de 300 frames au lieu de 300. Les captures en jeu
ont montré pire qu'un compte : **une ombre en forme de personnage, grande comme
le terrain**, qui part du but vers les joueurs et déborde jusque sur le tableau
d'affichage.

### Ce que ce n'était pas

Le rapport de ce mini-jeu donnait deux projections perspectives par frame — 7 767
et 29 128 triangles — et la même petite projection revient dans la plupart des
mini-jeux, avec des tailles qui se répètent d'un jeu à l'autre (7 123 triangles
dans quatre d'entre eux, 5 635 dans trois). L'hypothèse était tentante : une
géométrie dessinée par une autre caméra, placée par la capture juste devant celle
qu'on trace, et qui jette son ombre sur toute la scène.

Elle a été écrite, construite et mesurée : écarter des rayons les groupes de
l'autre projection change **26 pixels** de la vue de l'ombre sur 1 228 800, 918
de l'image composée, et rien sur le plateau, qui n'en a qu'une. Le terme d'ombre
tracé de cette frame, regardé directement, était juste : de petites ombres
portées, celles du but et du public, aucune silhouette. Le changement a été
retiré ; il n'avait pas d'effet mesuré.

Et la vérification qui aurait dû venir en premier : **sans ray tracing, pas de
silhouette**. Le défaut venait donc de là où le terme est appliqué, pas de ce
qu'il contient.

### Ce que c'était

`AURORA_RT_COMPOSITE_TRACE` ne journalisait que la passe qui recevait la
composition. Il journalise maintenant chaque passe à sa clôture : taille, 3D ou
non, frontière 2D et son viewport, copie dans une texture et son rectangle. Une
frame de m423Dll, 1 065 fois sur 1 070 :

| passe | 3D | copiée dans une texture | reçoit la composition |
|---|---|---|---|
| 0 | oui | 1 536 × 1 536 | non |
| 1 | oui | 1 280 × 960 | non |
| 2 | oui | 2 560 × 1 920 | non |
| 3, affichée | **non** | — | **oui** |

Le jeu dessine son terrain dans trois copies et les affiche comme des quads dans
la passe 3, qui ne contient plus aucune 3D. La composition, insérée à la
frontière 2D de cette passe, multipliait le terme — tracé pour l'écran de la
caméra — sur une image recomposée ailleurs : les ombres des joueurs agrandies et
décalées sur tout le terrain.

### La règle, et pourquoi elle est étroite

Refuser toute frame dont la 3D passe par une copie aurait été plus simple, et
faux : m401Dll copie de sa 3D dans 685 frames sur 686 et dessine pourtant sa
scène dans la passe affichée. Même relevé sur quatre scènes :

| scène | 3D copiée | composée dans une passe avec 3D | dans une passe sans |
|---|---|---|---|
| w01Dll | 0 | 1 344 | 2 |
| m401Dll | 685 sur 686 | 686 | 0 |
| m443Dll | 0 | 682 | 2 |
| m423Dll | 1 065 sur 1 070 | 36 | **794** |

La règle retenue : **une passe ne reçoit la composition que si elle a dessiné de
la 3D elle-même**. `AURORA_RT_COMPOSITE_ANY_PASS` rend l'ancienne.

Après correction, sur les mêmes scènes :

- m423Dll : plus aucune composition dans une passe sans 3D ; 29 frames composées
  dans une passe qui en a, 1 041 pas du tout. **La silhouette a disparu** des
  quatre captures, et l'image est celle du jeu sans ray tracing ;
- m401Dll : 686 sur 686, inchangé ;
- w01Dll : 1 341 frames composées ; les 2 frames de transition autrefois
  composées dans une passe sans 3D ne le sont plus.

Le test nul A/B sur le plateau reste à 0 pixel sur 1 228 800 : la règle ne
touche que la composition, pas le tracé.

### Ce qui reste

- **m423Dll n'a plus de ray tracing.** C'est honnête — le terme ne correspond pas
  à l'image montrée — mais ce n'est pas une solution : il faudrait savoir quelle
  copie devient quel morceau de l'écran, et composer dans la copie.
- **Les 29 frames encore composées** dans m423Dll, dans une passe qui dessine un
  peu de 3D par-dessus les copies, peuvent encore montrer le défaut par éclairs.
- **Les autres mini-jeux qui passent par le même chemin** ne sont pas encore
  connus : le balayage des 48 avec la trace des passes les listera.

## Fusion avec la version GitHub, et ce qu'elle change à la façon de committer (17 septembre 2026)

La branche GitHub `audio-local` avait avancé de 122 commits — netplay,
diagnostics, mods CubeShelf, correctif MusyX D3, porte de publication — pendant
que celle-ci en portait 54 de ray tracing. Fusion `afbd6c62`.

### Ce qui a demandé plus qu'une fusion de texte

- **Le patch MusyX** avait changé des deux côtés. C'est un fichier généré : il a
  été reconstruit, pas fusionné comme du texte. Le patch GitHub a été appliqué
  sur l'amont `a2b978d`, cet état fusionné avec le fork local, puis le patch
  régénéré et vérifié par hachage d'arbre. La version GitHub contenait tout le
  fork local, plus les points de diagnostic audio ; le seul fichier propre au
  local, la source de régression `test/wait_ms_regression.c`, est gardé.
- **`src/port/thp_player.cpp`** : GitHub rend la position des films
  déterministe sous netplay ; le local avait ajouté une horloge de secours pour
  qu'un film sans piste son ne bloque pas le jeu. La version GitHub seule
  bloquerait encore hors netplay, donc les deux sont gardées.

### La convention des sous-modules

Côté GitHub, les sous-modules restent sur leur commit amont — aurora `5143394`,
MusyX `a2b978d` —, les modifications PartyBoard vivent dans leur arbre de travail
sans être committées, et `patches/*.patch` est ce que la CI applique et livre.
Ce journal committait au contraire dans les sous-modules et enregistrait ces
commits dans le dépôt principal : aurora `adc1325`, par exemple, qui n'existe
qu'ici, et qu'une CI n'aurait pas pu récupérer.

La fusion revient à la convention GitHub. Les pointeurs sont les commits amont,
les arbres de travail ont été vérifiés identiques au hachage près aux branches
`partyboard-local`, qui gardent l'historique. **Un lot ne se committe plus dans
aurora** : le patch est réécrit par `tools/test_submodule_patches.ps1 -Update`,
contrôlé par le même outil sans `-Update`, et le commit n'a lieu que si ce
contrôle passe.

### Deux défauts d'outillage trouvés en route

- **Le contrôle des patches échouait à tort**, à longueur égale, sur le patch
  aurora. PowerShell décodait la sortie de git avec la page de code de la
  console (ibm850) et le fichier avec Windows-1252 ; les quatre tirets
  cadratins de ce patch devenaient deux chaînes différentes. Pire : `-Update`
  aurait réécrit la mauvaise. Les deux côtés sont lus en UTF-8 ; l'outil passe,
  et échoue toujours sur une vraie différence.
- **`test_raytracing.ps1` aurait fait échouer la porte de publication.** Le
  lanceur de tests découvre les scripts par motif et le prenait pour un test
  autonome ; il a besoin d'un disque et du binaire du jeu. Il est désormais
  classé comme tel, ignoré sans `-DiscPath` avec sa raison, et transmet ce
  disque au jeu par `PARTYBOARD_DISC_IMAGE`.

### Vérifié sur le code fusionné

| | |
|---|---|
| configuration et build complets | EXIT 0 |
| auto-tests du moteur, porte de la CI | 9 sur 9 |
| suite de scripts, comme la CI la lance | 14 réussis, 0 échec, 3 ignorés avec leur raison, 1 non applicable |
| contrôle des patches de sous-modules | réussi pour aurora et MusyX |
| test nul A/B du ray tracing sur le plateau | 0 pixel sur 1 228 800 |

`test_audio_wait` passe au lieu d'être « non applicable » : sa source de
régression manquait dans le patch GitHub, et la fusion l'y a mise.

## Deuxième fusion avec GitHub : aurora devient une pile de huit patches (28 septembre 2026)

GitHub avait avancé de 104 commits — Android, Meta Quest, conversion PAL,
RetroAchievements, sessions en ligne à quatre — et aurora n'y est plus portée
par un patch mais par **huit, que la CI applique dans l'ordre**. Le ray tracing
vit dans le premier, `aurora-partyboard.patch`. Fusion `b8413d8e`.

### Fusionner du code, pas du texte de patch

Dans un worktree jetable d'aurora, la pile GitHub a été rejouée en un commit par
patch (T1 à T8), et le premier étage local — le ray tracing — fusionné en trois
voies avec celui de GitHub contre leur version commune. Les sept étages suivants
ont ensuite été rejoués par-dessus :

| patch GitHub | par-dessus le ray tracing |
|---|---|
| render-fixes, android-surface-deadlock, mobile-one-local-player, render-worker-idle | s'appliquent tels quels, gardés octet pour octet |
| quest-stereo | conflit dans `webgpu/gpu.cpp` et `gpu.hpp` : l'interop D3D12 (Windows) et la stéréo (Android) ajoutaient chacune leurs fonctionnalités au même endroit ; les deux blocs sont gardés côte à côte |
| android-surface-generation | contexte déplacé, repris en trois voies sans conflit |
| quest-quality | conflit dans `gfx/texture.cpp` et `.hpp` : statistiques de texture du ray tracing et indicateurs de mips, les deux gardés |

Vérifié dans les deux sens, par ensembles de lignes : toute ligne que produit la
pile GitHub est dans l'arbre fusionné, sauf les six que le ray tracing modifiait
déjà, et toute ligne ajoutée par le ray tracing y est aussi. MusyX a fusionné de
la même façon, sans conflit.

Le lecteur de films prend la version GitHub : son horloge de simulation reprend
la main quand l'audio prend une seconde de retard, ce qui couvre — de façon
déterministe — le cas pour lequel l'horloge de secours locale existait. Elle
disparaît.

### L'outil de contrôle des patches, face à la pile réelle

L'outil GitHub posait une règle : un fichier ne doit appartenir qu'à un patch.
La pile GitHub elle-même partageait déjà **douze fichiers** — `gpu.cpp` entre
trois patches —, et l'outil ne le voyait pas : six des huit patches sont des
diffs simples, sans l'en-tête `diff --git` sur lequel il découpait. Plutôt que
de contourner la règle, l'outil est aligné sur ce que fait la CI : il lit les
deux formats, accepte les fichiers partagés comme une pile appliquée dans
l'ordre — sa vérification applique tout et compare fichier par fichier, ce qui
est exact dans ce cas —, et son mode `-Update`, qui ne peut pas redistribuer un
fichier partagé entre plusieurs patches, refuse au lieu de deviner. La
correction UTF-8 du 17 septembre y est reportée.

Conséquence pour ce journal : un lot de ray tracing ne se committe plus avec
`-Update`. La branche `partyboard-local` d'aurora garde la pile en huit commits ;
un lot est replié dans le premier, les sept autres rejoués par-dessus, chaque
patch régénéré depuis son étage, et le commit n'a lieu que si l'outil de
contrôle passe.

### Trois défauts du code GitHub, trouvés parce qu'il fallait que ça tourne ici

1. **Le build Windows ne compilait plus.** Le code Quest appelle `std::max` là
   où `windows.h` définit `max` (C2589). La CI GitHub est rouge sur le job
   Windows à chaque push depuis le 25 septembre — dix builds de suite, vérifié
   sur l'API publique. Neuf appels écrits `(std::max)(…)` : rien ne change à ce
   qui est calculé.
2. **Deux tests Quest échouaient faute de SDK Android**, et auraient fait
   échouer la porte de publication pour cette seule raison. Ils utilisent
   maintenant la convention du lanceur, code 2 : « cet environnement ne peut pas
   me lancer ».
3. **Le menu Party Board s'ouvrait sur l'écran titre** après chaque compilation
   des shaders au démarrage, et comme un document visible bloque la manette, le
   jeu restait sur PRESS START. L'écran de compilation se fermait avec `pop()`,
   qui montre aussi le haut de la pile de documents — or cet écran est passif,
   hors de la pile, et le haut de la pile était la barre de menu poussée cachée
   juste avant. Il se ferme maintenant avec `hide(true)`. Un joueur le voyait à
   chaque mise à jour ; les scripts de test, eux, restaient bloqués soixante pas
   sur l'écran titre.

### Vérifié sur le code fusionné

| | |
|---|---|
| build complet Windows | réussi, après le correctif 1 |
| auto-tests du moteur (porte CI) | 9 sur 9 |
| suite de scripts, comme en CI | 16 réussis, 0 échec après le correctif 2 ; les autres ignorés ou non applicables, avec leur raison |
| contrôle des patches | réussi : 8 patches aurora appliqués dans l'ordre, MusyX |
| test nul A/B du ray tracing sur le plateau | 0 pixel sur 1 228 800 |

### Les copies d'image entière, lues dans le code avant de lancer le jeu

La silhouette de m423Dll venait d'une frame recomposée à partir de copies de
l'image. Au lieu de repasser les 48 mini-jeux, une recherche dans `src/REL` de
`GXSetTexCopySrc(0, 0, HU_FB_WIDTH, HU_FB_HEIGHT)` donne en une seconde les onze
overlays qui copient l'image entière : m405, m410, m416, m417, m421, m423, m427,
m430, m440, m448, m460. Ce qui suit la copie, dans la même fonction, les sépare :

- **m410Dll a exactement le motif de m423Dll** : copie, chargement de la copie
  comme texture 640 × 480, redessin avec la matrice caméra dans le même hook.
  C'est le candidat suivant au même défaut ;
- **m448Dll** charge aussi sa copie comme texture dans la même fonction, à des
  échelles variables ;
- les autres copient puis retournent : la copie sert ailleurs, et la scène 3D
  continue après elle.

m460Dll n'est pas atteignable depuis la liste des mini-jeux. La confirmation de
m410Dll et m448Dll demande deux runs tracés, et ils attendent : le 28 septembre
au matin, un `llama-server` étranger à ce travail occupait tout le processeur, le
jeu tournait à 12 images par seconde, et la navigation par menus, cadencée en
temps réel, n'arrivait plus au bout.
