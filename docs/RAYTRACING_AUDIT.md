# Audit du ray tracing — 10 septembre 2026

Relecture complète des 4 072 lignes de `extern/aurora/lib/rt/`, confrontée aux
recommandations publiées de NVIDIA et de Microsoft (liens en fin de document).
Chaque constat porte soit une mesure prise sur cette machine, soit une référence.

---

## 1. Critique — le coût du tracé n'avait jamais été mesuré

`TraceStats::traceMs` était un `steady_clock` qui s'arrêtait juste après
`ExecuteCommandLists`. Rien n'attendait le GPU sur le chemin normal : il mesurait
donc le temps d'enregistrer et de soumettre la liste de commandes, et l'appelait
le coût du tracé.

C'est ce qui a produit « 1920 × 1440 en 0,05 ms » dans tous mes rapports depuis
le début. 2,76 M pixels × 64 rayons en 0,05 ms font 3,5 Trayons/s — un ordre de
grandeur au-dessus de ce que fait le meilleur matériel actuel. J'aurais dû le
voir.

Corrigé par des requêtes d'horodatage GPU autour du dispatch, résolues dans un
tampon de lecture et relues au début de l'appel suivant, là où `wait_idle()` a
déjà retiré le travail. Chiffres réels, même scène de plateau, 28 847 triangles,
1920 × 1440 :

| terme | GPU |
|---|---|
| occlusion ambiante seule (48 échantillons) | **6,28 ms** |
| ombres seules (16 échantillons) | **2,25 ms** |
| réflexions seules | **0,33 ms** |
| les trois | **8,10 ms** |
| soumission CPU (l'ancien « traceMs ») | 0,07 ms |

Sur d'autres cadrages du même plateau : 4,80 et 5,35 ms. Le budget d'une frame à
60 Hz est de 16,7 ms.

**Le tracé consomme donc 30 à 50 % de la frame, pas 0,3 %.**

Réserve d'honnêteté : un horodatage sur la file de calcul mesure le temps écoulé
sur cette file, préemption et partage du GPU avec Dawn compris. C'est le temps
que la passe occupe réellement, ce n'est pas forcément son coût exclusif.

## 2. Critique — une optimisation rejetée sur un chiffre faux

J'avais écarté le tracé en demi-résolution en écrivant que « le tracé coûte
0,05 ms, il n'y a rien à gagner ». Ce raisonnement reposait entièrement sur le
bug ci-dessus. À 1920 × 1440 pour 8,10 ms, passer en demi-résolution rendrait
environ 4 à 6 ms par frame.

C'est aujourd'hui le plus gros levier disponible, et il a été rejeté sur une
mesure fabriquée.

---

## 3. Les tampons de géométrie sont en mémoire système, lus par rayon

`g_vertexBuffer`, `g_colourBuffer`, `g_normalBuffer` et `g_materialBuffer` sont
créés en `D3D12_HEAP_TYPE_UPLOAD` (`rt_accel.cpp:233-250`). Ils sont lus deux
fois :

* par la construction des BLAS, à chaque reconstruction ;
* par le shader, **à chaque impact de rayon** (`gVertices`, `gNormals`,
  `gColours`, `gMaterials`).

Un tas d'upload est de la RAM système en écriture combinée, atteinte par le GPU
à travers le PCIe. La recommandation constante est de placer les tampons de
sommets d'accélération en tas `DEFAULT` et de passer par une copie de transit.
Ici c'est l'accès par impact qui coûte, pas la copie.

Ce n'est pas mesuré séparément : c'est un candidat sérieux pour une partie des
6,28 ms de l'occlusion ambiante, mais je ne l'affirme pas sans l'avoir isolé.

## 4. `Proceed()` n'est appelé qu'une fois, et rien ne garantit que ce soit correct

Trois des quatre requêtes sont déclarées `RayQuery<RAY_FLAG_NONE>` et appellent
`Proceed()` une seule fois, hors boucle (`rt_ao.hlsl:196, 342, 389`).

Cela ne fonctionne que parce que **toutes** les géométries portent
`D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`. La spécification autorise `Proceed()` à
rendre `true` pour une candidate demandant l'avis du shader ; avec
`RAY_FLAG_NONE` en drapeau de compilation, rien ne dit au compilateur que ce cas
est impossible. Le jour où une géométrie non opaque entre dans la structure, la
traversée s'arrêtera silencieusement au mauvais endroit.

Correctif : déclarer `RayQuery<RAY_FLAG_FORCE_OPAQUE>`. Cela documente
l'hypothèse, la rend vraie par construction, et suit la recommandation de NVIDIA
de préférer les drapeaux de compilation aux drapeaux d'exécution.

## 5. L'adaptateur est identifié par vendorId + deviceId, pas par son LUID

`find_adapter()` (`rt_device.cpp:44`) cherche l'adaptateur de Dawn en comparant
`VendorId` et `DeviceId`. Ces champs identifient un **modèle** de matériel, pas
une instance. Deux cartes identiques dans la même machine, et le premier match
gagne — qui peut ne pas être celui de Dawn. Les handles partagés seraient alors
créés sur le mauvais GPU.

La clé correcte est le LUID : `ID3D12Device::GetAdapterLuid` est explicitement
conçu pour être apparié à `IDXGIFactory4::EnumAdapterByLuid`.

## 6. Le décalage anti auto-intersection est une constante, l'échelle des scènes varie de 15×

`origin = hit + geometric * 0.05f` (`rt_ao.hlsl:259`), avec `TMin = 0` partout.
Or l'étendue des scènes mesurée va de 1 476 unités (un mini-jeu) à 22 600 (la
diagonale d'un plateau). Un epsilon fixe contre une échelle qui varie d'un
facteur quinze est le cas d'école de l'acné d'auto-ombrage à un bout et de la
perte de contact à l'autre.

Aucun artefact n'a été observé — le tampon d'occlusion vidé sur disque est
propre. C'est un risque latent, pas un défaut constaté. La méthode de référence
est celle de Wächter et Binder (*Ray Tracing Gems*, ch. 6), ou plus simplement un
`TMin` proportionnel à la distance d'impact.

## 7. Le bruit est figé d'une frame à l'autre

La graine ne contient pas d'index de frame :
`hash(tid.x + tid.y * gDims.x + gSampleCount * 9781u)` et
`interleaved_gradient_noise(float2(tid.xy))` sont purement spatiales.

Conséquences : le motif d'échantillonnage est verrouillé à l'écran, donc il
« colle » à la caméra quand elle bouge — l'artefact dit de la porte de douche —
et aucune accumulation temporelle n'est possible. En contrepartie, aucun
scintillement. Ce compromis n'est écrit nulle part ; il devrait l'être, ou être
corrigé.

## 8. Les réflexions coûtent un rayon pour un résultat invisible

Mesuré sur un plateau : 63 des 438 draws sont marqués « environment mapped »,
soit 14 % de la scène, et l'écart de luminosité entre réflexions actives et
éteintes est de **0,0 %**. Coût : 0,33 ms.

Avec `refl.a = fresnel × 0,35` et une vue majoritairement frontale, le mélange
tourne autour de 3 % : présent, sous le seuil du visible. Soit on assume le
terme et on lui donne de quoi se voir, soit on le retire.

---

## Points mineurs

| | |
|---|---|
| `rt_denoise.hlsl:28` | Le commentaire dit que la netteté des normales « monte avec l'index de passe ». Le code utilise 12,0 constant, et le commentaire au point d'appel explique pourquoi. Contradiction. |
| `rt_accel.cpp:487-488` | Deux barrières UAV nommées avant le TLAS ; NVIDIA recommande une seule barrière globale. |
| `rt_ao.hlsl:155` | Groupes de 8 × 8 ; NVIDIA recommande 8 × 8 **ou mieux** 16 × 8 pour RayQuery. |
| `rt_accel.cpp` | Aucun tampon d'indices : trois sommets uniques par triangle, soit des tampons trois fois plus gros et des constructions plus lentes, alors que la source GX est souvent déjà indexée. |
| `rt_accel.cpp:353` | `PREFER_FAST_TRACE` sur des BLAS reconstruits ~78 % des frames. NVIDIA dit de choisir expérimentalement pour la géométrie dynamique. Non testé ici. |
| `rt_denoise.hlsl:16` | La source du filtre est liée en UAV ; un SRV permettrait la mise en cache en lecture. |
| — | Pas de compactage des BLAS. Peu d'enjeu ici : 1,5 à 3 Mo. |

## Ce qui a été vérifié et tenu

* **Mémoire de scratch par construction** — chaque BLAS a sa propre région, donc
  aucune barrière entre les constructions. C'est exactement la recommandation de
  NVIDIA, et c'est ce qui avait fait passer une reconstruction de 304,9 ms à
  185,7 ms.
* **`PREFER_FAST_TRACE` sur le TLAS** — conforme.
* **`ACCEPT_FIRST_HIT_AND_END_SEARCH` en drapeau de compilation sur les rayons
  d'ombre** — conforme, c'est l'optimisation recommandée.
* **Durée de vie des ressources** — corrigée pendant cet audit et l'itération
  précédente ; le compteur `waitedFrames` prouve que la fenêtre s'ouvrait
  réellement (166 fois dans un mini-jeu).
* **Validation avant construction** — les plages de triangles et la finitude des
  transformations sont vérifiées avant d'être remises au pilote.
* **DRED plutôt que la couche de debug** — justifié : `EnableDebugLayer()` est
  global au processus et faisait tomber le périphérique de Dawn.

## Ordre de traitement suggéré

1. Demi-résolution du tracé (§2) — le plus gros gain, environ 4 à 6 ms.
2. `RAY_FLAG_FORCE_OPAQUE` (§4) — quelques caractères, supprime un piège réel.
3. Tampons de géométrie en tas `DEFAULT` (§3).
4. LUID pour l'adaptateur (§5).
5. Trancher sur les réflexions (§8) : les rendre visibles ou les retirer.
6. Décalage proportionnel à l'échelle (§6), index de frame dans la graine (§7).

## Sources

- [Best Practices for Using NVIDIA RTX Ray Tracing](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/)
- [Tips and Tricks: Ray Tracing Best Practices](https://developer.nvidia.com/blog/rtx-best-practices/)
- [DirectX Raytracing (DXR) Functional Spec](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
- [Managing Memory for Acceleration Structures in DXR](https://developer.nvidia.com/blog/managing-memory-for-acceleration-structures-in-dxr/)
- [ID3D12Device::GetAdapterLuid](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getadapterluid)
- [IDXGIFactory4::EnumAdapterByLuid](https://learn.microsoft.com/en-us/windows/win32/api/DXGI1_4/nf-dxgi1_4-idxgifactory4-enumadapterbyluid)
- [A Fast and Robust Method for Avoiding Self-Intersection](https://link.springer.com/content/pdf/10.1007/978-1-4842-4427-2_6.pdf)
- [How to Compact Acceleration Structures in D3D12](https://alextardif.com/Compaction.html)

---

# Suites données — 10 septembre 2026

Sept des huit constats traités, chacun mesuré sur la même image de plateau
(28 547 triangles) pour que les chiffres soient comparables entre eux.

| | avant | après |
|---|---|---|
| tracé | 8,72 ms | **3,50 ms** |
| construction | 1,23 ms | **0,92 ms** |
| mémoire BLAS | 1 874 Ko | **1 590 Ko** |
| total par frame | ≈ 9,95 ms | **≈ 4,42 ms** |

Sur un budget de 16,7 ms, la charge passe de 60 % à 26 %. L'image est
indistinguable à 2× d'agrandissement.

## Ce qui a payé

**§1 — mesure.** Requêtes d'horodatage GPU autour du dispatch. Le chiffre CPU
que l'ancien `traceMs` rapportait est conservé sous `submitMs` : 0,07 ms, ce qui
est bien son ordre de grandeur.

**§2 — demi-résolution.** 1280 × 960 au lieu de 1920 × 1440 : 8,72 → 3,80 ms.
La composition reconstruit le tampon en bilinéaire à la main, puisque les cibles
sont `unfilterable-float` et qu'aucun échantillonneur ne peut les toucher. Sans
ça, un tampon en résolution réduite se voit comme tel.

**Drapeau de construction.** `PREFER_FAST_BUILD` sur les BLAS : construction
−24 %, mémoire −15 %, tracé inchangé. La structure est jetée trop souvent pour
amortir ce que coûte `PREFER_FAST_TRACE` à fabriquer. Le TLAS garde
`PREFER_FAST_TRACE`, conformément à la recommandation.

## Ce qui n'a rien payé, et il faut le dire

**§3 — tampons en mémoire vidéo.** Mise en scène par tampon de transit vers un
tas `DEFAULT`, comme le demandent toutes les recommandations publiées. Résultat
sur cette machine : **3,72 ms avant, 3,72 ms après**. Soit le cache absorbait
déjà les lectures, soit la traversée domine. Conservé quand même — la forme
précédente créait une dépendance par rayon vers la mémoire système, qui coûterait
ailleurs — mais l'audit citait ce point comme candidat au coût de l'occlusion
ambiante, et il ne l'est pas.

**Groupes 16 × 8.** Recommandé pour un noyau RayQuery. Mesure : aucun écart.
Gardé parce que c'est la forme recommandée, pas parce que ça a rapporté.

**§4 — drapeaux de traversée.** `RAY_FLAG_FORCE_OPAQUE |
RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES` en drapeaux de compilation : aucun gain
mesurable, mais le `Proceed()` unique devient correct par construction au lieu de
correct par accident.

## Corrigés sans mesure associée

**§5 — LUID.** Dawn expose son propre périphérique D3D12 ; quand il est sur ce
backend, `GetAdapterLuid` donne la réponse exacte et `EnumAdapterByLuid` est
l'appel prévu pour être apparié avec. L'ancienne recherche par vendeur et modèle
reste en repli.

**§6 — décalage proportionnel.** `max(0.01, t × 1e-3)` le long de la normale
géométrique, au lieu d'une constante de 0,05 contre des scènes allant de 1 476 à
22 600 unités.

**§7 — graine figée.** Non modifié, mais désormais justifié dans le code : faire
varier le motif par frame ne vaut qu'accompagné d'une accumulation temporelle
contre laquelle converger. Sans elle, ce serait du scintillement. Le vrai
correctif est un filtre temporel, pas une autre graine.

**Points mineurs.** Une seule barrière UAV globale avant le TLAS ; le commentaire
du débruiteur ne prétend plus que l'exposant monte par passe alors que le code le
tient à 12 ; la liaison en UAV de la source du filtre est maintenant expliquée
(le ping-pong l'échange avec la cible, un descripteur ne peut pas être les deux).

## Reste ouvert

**§8 — les réflexions.** Toujours 0,33 ms pour 0,0 % d'écart mesurable. Le terme
ne peut pas être rendu visible tel qu'il est conçu : la seule couleur disponible
au point d'impact est la moyenne d'une texture entière, donc monter le mélange
produirait des aplats faux plutôt qu'un reflet. Le rendre juste demande
d'échantillonner la vraie texture à l'impact, ce qui suppose des UV par sommet
dans les tampons et un accès bindless aux textures. C'est un chantier, pas un
réglage. La décision — investir ou retirer — appartient à qui a demandé la
fonctionnalité.

**Tampons d'indices.** Toujours trois sommets uniques par triangle. La source GX
est souvent déjà indexée ; les réutiliser réduirait les tampons et le coût de
construction. Non tenté.

---

# Balayage sur plateaux réels — 11 septembre 2026

20 mesures, 3 plateaux, via `tools/test_raytracing.ps1` et `tools/sweep_raytracing.ps1`.
Aucune erreur, aucun retrait de périphérique, aucun plantage.

| plateau | n | tracé min | max | médiane | découpe | env. mappé |
|---|---|---|---|---|---|---|
| `w01Dll` | 3 | 4,90 ms | 6,18 ms | 5,86 ms | 28,8–31,2 % | 28–31 % |
| `w04Dll` | 3 | 1,84 ms | 4,74 ms | 1,89 ms | 6,9–13,4 % | ~1 % |
| `w05Dll` | 14 | 1,83 ms | 6,61 ms | 6,00 ms | 24,5–30,4 % | ~0 % |
| **toutes** | **20** | **1,83 ms** | **6,61 ms** | **5,85 ms** | | |

## Ce que ça dit

**Le coût dépend surtout de la caméra, pas du plateau.** `w05Dll` seul couvre
1,83 à 6,61 ms — un facteur 3,6, aussi large que l'écart entre plateaux. Une
première lecture de trois mesures m'avait fait écrire que `w04Dll` coûtait 3 fois
moins que `w01Dll` « à cause de la couverture écran plutôt que du nombre de
triangles » ; avec vingt mesures, c'était de la variance intra-plateau lue comme
un signal inter-plateau. Le nombre de triangles ne prédit rien, ça reste vrai ;
mais l'identité du plateau non plus.

**La médiane réelle est 5,85 ms, pas 3,49 ms.** Le défaut demi-résolution a été
validé sur une seule mesure de `w10Dll` à 3,49 ms. Sur un plateau ordinaire le
tracé prend donc plutôt **35 % d'une frame à 60 Hz que 21 %**. Le réglage n'est
pas mauvais, mais la marge annoncée venait de l'échantillon le plus favorable.

**Les propriétés de matériau, elles, sont stables par plateau** et très
différentes entre eux : `w01Dll` marque 28–31 % de draws en découpe et autant en
environment mapping, `w04Dll` 7–13 % et 1 %, `w05Dll` 25–30 % et 0 %. La
géométrie découpée est tracée comme opaque ; les 0,31 % de pixels qui justifiaient
de l'ignorer avaient été mesurés sur un menu, pas sur un plateau à végétation.

## Ce que ce balayage ne dit pas

**Le masque 2D n'y est jamais sollicité.** Les vingt captures affichent 0 % de
couverture avec 0 draw 2D borné : l'outil s'arrête à la première image du
plateau, qui est toujours le survol d'introduction, sans interface. Le correctif
du HUD reste validé uniquement sur `w10Dll`. Pour l'exercer il faut jouer un tour,
pas seulement charger le plateau.

**Trois plateaux sur neuf.** Les autres sont verrouillés dans cette sauvegarde :
Mario Party 4 débloque ses plateaux par le mode Histoire, et
`game.unlockBowsersGnarlyParty` n'en ouvre pas d'autre sur ce chemin de menu.
C'est une limite de la sauvegarde, pas de l'outil.

**Et `w10Dll`, la référence de tout l'audit, n'a pas été retrouvée.** Le chemin de
menu que prend l'outil actuel n'y mène jamais ; l'ancienne séquence y arrivait.
Quel écran c'est exactement reste inconnu.

---

# Balayage sur mini-jeux — 11 septembre 2026

Six mini-jeux distincts, atteints par `-MinigameIndex`. Aucune erreur, aucun
retrait de périphérique.

| scène | triangles | tracé | masque 2D | draws 2D | projections | découpe | env. mappé |
|---|---|---|---|---|---|---|---|
| `m416Dll` | 25 620 | 1,53 ms | 7,4 % | 6 | 2 | 38,0 % | 0 % |
| `m410Dll` | 29 975 | 2,16 ms | 7,4 % | 6 | 2 | 40,5 % | 0 % |
| `m406Dll` | 21 947 | 2,70 ms | 0 % | 0 | 1 | **52,8 %** | 0 % |
| `m401Dll` | 29 019 | 2,87 ms | 8,3 % | 20 | 2 | 35,2 % | 1,3 % |
| `m403Dll` | 19 638 | 4,10 ms | 0 % | 0 | 2 | 32,2 % | 0,9 % |
| `m414Dll` | 21 100 | 5,79 ms | 13,4 % | 28 | 1 | 17,0 % | 0 % |

## Ce que ça change par rapport aux plateaux

**Les mini-jeux coûtent moins cher.** Médiane 2,8 ms contre 5,85 ms sur les
plateaux, pour un intervalle comparable (1,5–5,8 contre 1,8–6,6). Le réglage
« High » y tient largement dans une frame.

**La géométrie découpée y domine** : 17 à 53 % des draws, contre 6 à 31 % sur
les plateaux. `m406Dll` pousse à **plus d'un draw sur deux**. C'est là que la
pondération de l'occultation par la fraction solide compte le plus — et c'est
une population que les mesures faites sur les menus n'avaient aucune chance de
représenter.

**Les réflexions n'existent pas en mini-jeu.** Entre 0 et 1,3 % de draws
« environment mapped », contre 29 % sur `w01Dll`. Ajouté aux 1–4 % de pixels
mesurés sur un plateau, le terme ne fait rien du tout ici. Cela confirme, sur
une deuxième population, que la moitié chère du chantier des réflexions n'aurait
rien rapporté.

**Deux projections par frame sur quatre mini-jeux sur six**, contre une seule
sur tous les plateaux mesurés. Le choix de la projection couvrant le plus de
triangles, plutôt que celle du dernier draw, sert donc surtout ici.

**Le masque 2D s'engage sur quatre des six**, avec 6 à 28 draws 2D bornés. Les
deux à 0 % ont 0 draw borné : rien à protéger à cet instant, pas un défaut.

## Portée

Treize scènes distinctes mesurées en tout — trois plateaux, six mini-jeux plus
`m405Dll` atteint séparément, et `w10Dll` d'où venaient toutes les mesures de
l'audit. Sur soixante-et-un mini-jeux et neuf plateaux. Les plateaux restants
sont verrouillés par la progression de la sauvegarde ; les mini-jeux, eux, sont
tous accessibles, `-MinigameIndex` suffit.

---

# D'où viennent les 5 ms (11 septembre 2026)

Question posée : ces latences paraissent hautes pour un si petit décor. Elles le
sont, et la décomposition dit exactement pourquoi. Même plateau, `w01Dll`,
1280 × 960, RTX 5090.

| configuration | tracé |
|---|---|
| défaut : 48 AO, 16 ombres, débruiteur actif | 5,02 ms |
| débruiteur désactivé | 5,33 ms |
| 1 échantillon d'AO, 16 ombres | 1,50 ms |
| 1 AO, 1 ombre, sans débruiteur | **0,30 ms** |

## Le débruiteur ne coûte rien

5,33 ms sans lui contre 5,02 ms avec : l'écart est dans le bruit, et il va dans
le mauvais sens. Les quatre passes à-trous sont gratuites à cette échelle.
J'avais supposé qu'une partie des 5 ms pouvait leur revenir, puisque les
horodatages les englobent. Non.

## Tout est dans le nombre d'échantillons

Le socle — visibilité primaire, tampon guide, coût de lancement — vaut **0,30 ms**.
Le reste est linéaire en rayons : passer l'AO de 48 à 1 échantillon fait tomber
la passe de 5,02 à 1,50 ms, soit environ **0,078 ms par échantillon et par frame**
à cette résolution. Cela fait 1,23 M rayons en 0,078 ms, autour de **16 Grayons/s** :
un débit ordinaire pour cette carte. Le GPU n'est pas en cause.

Ce qui est en cause, c'est qu'on lance **64 rayons par pixel** là où un jeu qui
sort en lance **1 ou 2**, et rattrape la qualité par accumulation temporelle
entre les frames. À 2 rayons par pixel, la même passe coûterait environ
0,30 + 2 × 0,078 ≈ **0,46 ms**.

Le faible nombre de triangles ne sauve rien : le coût d'un rayon croît à peu près
comme le logarithme de la géométrie, donc 25 000 triangles ne valent pas cent
fois moins cher que 2,5 millions.

**L'accumulation temporelle vaut donc un facteur dix sur cette passe**, et c'est
le même chantier qui réglerait le bruit verrouillé à l'écran du constat §7.
Réserve : les scènes diffèrent un peu d'un lancement à l'autre (14 239 à 16 845
triangles ici), ce qui vaut environ ±0,3 ms de bruit sur ces chiffres.

---

# Accumulation temporelle (11 septembre 2026)

La décomposition ci-dessus disait que tout le coût était dans le nombre
d'échantillons : 0,30 ms de socle, puis 0,078 ms par échantillon et par frame.
Un jeu qui sort lance un ou deux rayons par pixel et converge entre les frames ;
on en lançait soixante-quatre.

## Sans vecteurs de mouvement

La géométrie est cuite relativement à la matrice de chaque draw et placée par des
transformations d'instance renouvelées à chaque frame : il n'existe pas d'espace
stable où reprojeter, et la découpe en groupes peut changer, ce qui casserait
toute correspondance d'instance à instance. Les vecteurs de mouvement sont donc
un chantier à part entière.

Le test est le tampon guide lui-même : **même pixel, même normale, même distance
d'impact** veut dire même surface, donc on mélange. Les deux moitiés comptent —
la distance seule laisse une silhouette glisser sur un fond à la même profondeur,
la normale seule laisse un mur hériter du sol qu'il rejoint.

Caméra immobile : ça converge. Mouvement : le test échoue à ce pixel et on
retombe sur la frame seule, jamais pire qu'avant. Mario Party passe l'essentiel
de son temps immobile.

Deux guides et deux historiques alternent selon la parité de frame. Les sorties
anticipées — pixel masqué, rayon primaire dans le vide, projection dégénérée —
écrivent l'historique aussi, sinon une valeur périmée survit derrière elles.

## Ce que ça donne

| configuration | tracé |
|---|---|
| 48 AO / 16 ombres, sans accumulation | ~5,0 ms |
| 48 / 16, avec accumulation | 3,9 ms |
| **8 / 4, avec accumulation** | **0,84 ms** en jeu |
| 8 / 4, caméra en mouvement constant | 1,27 ms |

La luminosité moyenne de 8/4 accumulé et de 48/16 accumulé s'accordent à 0,5 %
près. Le survol d'introduction, où la caméra bouge sans arrêt et où
l'accumulation est rejetée en continu, reste propre.

Les niveaux de qualité sont donc recoupés, puisqu'ils étaient dimensionnés pour
un monde sans accumulation :

| niveau | résolution | rayons | tracé |
|---|---|---|---|
| Low | 640 × 480 | 4 / 2 | — |
| Medium | 896 × 672 | 6 / 3 | — |
| **High** (défaut) | 1280 × 960 | 8 / 4 | **0,89 ms** |
| Ultra | 1920 × 1440 | 16 / 8 | 4,71 ms |

## Le chemin complet

| étape | tracé |
|---|---|
| ce que l'ancien compteur affichait | « 0,05 ms » |
| mesure réelle, pleine résolution, 48/16 | 8,72 ms |
| demi-résolution | 3,80 ms |
| accumulation + 8/4 | **0,89 ms** |

Soit un facteur **dix** entre le coût réel de départ et celui d'aujourd'hui, à
qualité comparable — et le point de départ de tout cela était de découvrir que le
chiffre affiché mesurait la soumission CPU et pas le GPU.

## Ce qui reste ouvert

`AURORA_RT_TEMPORAL=1` désactive l'accumulation, et les nouveaux compteurs
paraîtront alors aussi bruités qu'ils le sont. De vrais vecteurs de mouvement
feraient converger aussi pendant les déplacements, ce que ce test par guide ne
fait pas.
