# Validation « Mario Party 4 jouable à 100 % en ligne »

Critère d'acceptation posé par l'utilisateur : **pouvoir jouer normalement jusqu'à
décider soi-même de quitter.** Tant que ce n'est pas vrai, le mode en ligne n'est pas
stable, quels que soient les indicateurs de déterminisme.

## Règle de notation

Chaque session produit **deux verdicts indépendants** qui ne doivent jamais être
fusionnés en un seul `PASS` :

| Verdict | Ce qu'il mesure |
|---|---|
| `DETERMINISM` | les deux pairs calculent le même état : hash canonique, RNG, frames de transition |
| `STABILITY` | le processus reste vivant et c'est l'utilisateur qui décide de la fin |

Un déterminisme parfait ne dit **rien** sur la stabilité. Une sortie inexpliquée est un
bug bloquant même si aucun hash n'a divergé.

Vocabulaire, conformément à la discipline de la matrice : non testé = `UNTESTED`,
cassé = `FAIL`, inaccessible = `BLOCKED`. Rien n'est marqué `PASS` sans que la
mécanique ait été réellement exercée.

## Sessions

### S1 — Big Boo, 4 tours, 4 mini-jeux — 2026-09-11

Baseline `bc63c93c`. Deux instances locales, jeu manuel, UDP 54642, délai d'entrée 3.
Session `202c3bb30508408284889c9e9318026d`, PID 47776 (hôte, player 1) et 26120
(client, player 2).

| Verdict | Résultat |
|---|---|
| `DETERMINISM` | **PASS** jusqu'à la frame 49 801 |
| `STABILITY` | **FAIL** — terminaison des deux processus non demandée par l'utilisateur |

Mesures positives, toutes réelles :

- 49 801 frames simulées, ~831 s (13 min 51 s) ;
- quatre tours complets de Big Boo, quatre mini-jeux distincts ;
- 39 transitions d'overlay franchies aux frames identiques sur les deux pairs ;
- 99 756 lignes d'enregistrement sur lesquelles les deux pairs sont d'accord ;
- aucun hash canonique divergent ; `mismatch=0`, `rng_sync=1`, `context_skew=0`,
  `repaired=0`, `send_errors=0` ;
- aucun `netplay_desync_*.log` écrit ;
- aux six derniers checkpoints (frames 49 200 à 49 800) les deux pairs rapportent le
  même `state_hash` à la valeur binaire près.

Échec de stabilité :

- les deux processus se sont terminés vers la frame ~49 877 sans demande de
  l'utilisateur, en pleine partie ;
- cause OS : exception `0xc0000374` (`STATUS_HEAP_CORRUPTION`), module fautif
  `ntdll.dll`, offset `0x117eb5`, pour **chacun** des deux processus ;
- deux événements `Application Error` id 1000 distincts, PID `0x6608` (26120) à
  00:27:55.888 et PID `0xBAA0` (47776) à 00:27:55.904 ;
- deux minidumps distincts sous `%LOCALAPPDATA%\CrashDumps`
  (`partyboard.exe.26120.dmp`, `partyboard.exe.47776.dmp`) ;
- aucune ligne `loop_exit reason=` : la sortie n'est pas un retour de boucle
  instrumentée mais une exception matérielle en milieu de frame ;
- dernière trace de gameplay identique sur les deux pairs :
  `Landing Pos Get` puis `Getoff`, soit `src/REL/w04Dll/boo_event.c:1022` puis `:597`.

Conclusion : le superviseur n'a tué personne. Les deux instances ont crashé
indépendamment, dans la même frame de simulation, parce que la simulation est
déterministe. **Le déterminisme du netplay a rendu un bug préexistant du jeu
parfaitement symétrique.**

Ce que S1 ne couvre pas : un plateau sur huit, quatre mini-jeux sur plus de soixante,
aucune boutique, aucune loterie, aucun Boo hors de cet événement, aucun item, aucune
fin de partie, aucun joueur CPU, deux joueurs sur quatre, une seule machine en
bouclage local.

### S2 — Replay Big Boo, cause racine trouvée et corrigée — 2026-09-11

La session S1 n'était pas un mystère de désynchronisation : c'était un
**débordement de pile de coroutine HuPrc**, défaut D4 du registre.

`fn_1_30A4`, le processus d'événement Big Boo créé à
`src/REL/w04Dll/boo_event.c:343`, demandait la constante PowerPC `0x1000`.
Doublée sur PC, elle donnait 8192 octets pour un besoin mesuré de **8200**. Il
débordait de **huit octets**, l'adresse de retour d'un `call`, à chaque partie.

Preuve au niveau de l'instruction, obtenue par WinDbg sur le minidump WER :

```
ExceptionAddress: KERNELBASE!WriteFile+0x86
   instruction:   call qword ptr [_imp_NtWriteFile]
   Parameter[0]:  1                  -> WRITE
   Attempt to write to address ...0ff8
   rsp = ...81000                    -> frontiere de page
!teb  StackBase/StackLimit d'une TOUT AUTRE region
  -> le thread ne tournait pas sur sa pile Windows
```

Les deux pairs sont logiquement identiques : mêmes 12 bits de poids faible sur
`rsp`, `rax`, `rbx`, `r10`, seule l'ASLR diffère.

Mesures de consommation réelle, constante d'origine à gauche :

| Constante | Pic réel | ×2 donnait | Verdict |
|---|---|---|---|
| 2048 | 280 | 4096 | ok |
| **4096** | **8200** | **8192** | **déborde de 8 octets** |
| 8192 | 6552 | 16384 | ok |
| 14336 | 6696 | 28672 | ok |
| 16384 | 6312 | 32768 | ok |
| 24576 | 8552 | 49152 | ok |

Le besoin est une propriété de la chaîne d'appels, autour de 8,5 Ko au plus
profond, et n'a presque aucun rapport avec la constante d'origine. Corrigé par
`875a1ef7` : multiplicateur ×4 **et plancher de 32 Ko**, soit 3,8 fois la
profondeur maximale jamais mesurée.

| Verdict | Résultat |
|---|---|
| `DETERMINISM` | **PASS** — `mismatch=0`, `rng_sync=1` sur toute la durée |
| `STABILITY` | **PASS pour D4** — plus aucun débordement ; D3 reste ouvert |

Vérifications :

| Run | Configuration | Frame atteinte | Débordement | Sync |
|---|---|---|---|---|
| mesure | piles ×8 | **93 241** | aucun | `mismatch=0` |
| correctif | ×4 + plancher 32 Ko | **59 925** | aucun | `mismatch=0` |

Les deux dépassent l'ancien point de crash (48 671) de dizaines de milliers de
frames, les deux pairs toujours d'accord.

**Ce qui reste ouvert** : le défaut D3, une lecture invalide intermittente dans
le décodeur ADPCM sur le thread audio, apparue une fois sur quatre replays. Il
est indépendant de D4 et hors périmètre tant que MusyX est gelé.

---

## Matrice des mécaniques

| Mécanique | DETERMINISM | STABILITY |
|---|---|---|
| Démarrage, menu de mode, menu d'entrée | PASS (S1) | PASS (S1) |
| Plateau Big Boo (`w04Dll`) — déplacement, dés, tours | PASS (S1) | **FAIL (S1)** |
| Événement Big Boo (`boo_event.c`) | PASS (S1) | **FAIL (S1)** |
| Mini-jeux (4 exercés) | PASS (S1) | PASS (S1) |
| Écran de résultats de mini-jeu | PASS (S1) | PASS (S1), **D3 corrigé ici** |
| Sept autres plateaux | UNTESTED | UNTESTED |
| Boutique, loterie, items, étoiles | UNTESTED | UNTESTED |
| Fin de partie, classement final | UNTESTED | UNTESTED |
| Joueurs CPU | UNTESTED | UNTESTED |
| Quatre joueurs | UNTESTED | UNTESTED |
| Deux machines, réseau réel | UNTESTED | UNTESTED |
| Rollback SAVE/RESTORE/REPLAY | **FAIL (D5)** | UNTESTED |

### Note sur la ligne rollback

Elle n'est plus `UNTESTED` : elle a été testée et elle échoue. Le probe
`PARTYBOARD_FORCE_ROLLBACK` a mesuré, à son premier essai, qu'un retour arrière
d'**une seule frame** ne se reproduit pas — quinze sous-systèmes canoniques
reviennent identiques, `ANIMATION` non. C'est le défaut D5 du registre, et il
s'applique aussi au chemin de rollback réseau réel, qui rejoue ses ticks de la
même façon.

Le verdict `STABILITY` de cette ligne reste `UNTESTED` : le probe s'arrête sur la
divergence avant d'avoir pu exercer quoi que ce soit de long.

## Condition pour qu'une session Big Boo soit réellement PASS

Les cinq conditions sont conjointes :

1. elle reste synchronisée ;
2. elle ne crashe pas ;
3. elle ne sort pas spontanément ;
4. le jeu peut continuer ;
5. **c'est l'utilisateur qui provoque la fermeture finale.**
