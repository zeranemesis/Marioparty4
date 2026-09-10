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
