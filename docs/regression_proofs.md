# Les correctifs vus échouer sans eux

**Un test de non-régression qu'on n'a jamais vu rougir n'est pas un test.**

D3, D4 et D5 ont chacun un scénario *censé* les couvrir. Jusqu'au 2026-09-11,
aucun n'avait jamais été observé en échec : ils étaient verts sur un binaire
corrigé, ce qui est compatible avec « le correctif tient » **et** avec « le
scénario ne touche pas le chemin ». Rien ne permettait de distinguer les deux.

Ce document enregistre l'expérience qui les distingue. Pour chaque défaut :
annuler son correctif seul, tout le reste identique, reconstruire, lancer le
scénario, et exiger qu'il rougisse — puis restaurer et exiger qu'il reverdisse.

Méthode : le fichier source est copié hors de l'arbre avant modification et
restauré par copie, jamais par `git checkout`, parce que deux de ces fichiers
portaient d'autres travaux non committés au moment de l'expérience. La
restauration est vérifiée par `md5sum` ou par `git status`, pas par confiance.

Les trois scénarios sont versionnés dans `tests/scenarios/regression-proofs.json`,
chacun avec le champ `revert` qui dit exactement quoi annuler. Ils sont tenus à
l'écart du manifeste de campagne parce qu'ils existent pour être lancés
délibérément, un correctif à la fois, contre un binaire cassé exprès :

```bash
tools\netplay_campaign.ps1 -DiscPath "<iso>" -Manifest tests/scenarios/regression-proofs.json -Scenario d3-first-unload -Repeat 1
```

---

## D5 — l'horloge d'animation dans un tick rejoué

**Correctif annulé** : `PartyBoard_AnimationAdvance();` retiré de
`PartyBoard_RollbackRunGameLogicTick` (`src/game/main.c`).

**Scénario** : `w04-boot-smoke`, sonde de rollback forcée à `600:1,2,4`.

| | correctif absent | correctif présent |
|---|---|---|
| résultat | **DESYNC** | **PASS** |
| frame atteinte | 602 | 7109 |
| empreinte | `ROLLBACK:ANIMATION:(model->motWork).time` | — |
| note | `2 rollback failure report(s), first divergence in ANIMATION at (model->motWork).time` | — |

La sonde échoue à sa **première** occasion, frame 602, et nomme exactement le
champ qui avait servi à découvrir le défaut. Le scénario couvre bien le chemin.

---

## D3 — banque audio libérée sous une voix encore en lecture

**Correctif annulé** : la barrière au moment de la libération. Un `return`
anticipé est inséré au début de `salDetachVoicesFromSample`
(`extern/musyx/src/musyx/runtime/hw_pc.c`). **Les détecteurs ne sont pas
touchés** : ce qui est annulé est le correctif, pas l'instrument.

**Scénario** : `d3-first-unload` — le replay `board-replay.txt` mené jusqu'à la
première libération de banque de l'écran de résultats, frame 18141, où la chaîne
causale avait été prouvée quatre fois sur quatre. 340 secondes suffisent ; il
n'est pas nécessaire de dérouler l'enregistrement entier.

| | correctif absent | correctif présent |
|---|---|---|
| résultat | **CRASH** | **PASS** |
| frame atteinte | 20225 | 20229 |
| violations | **8** | **0** |
| libérations de banque observées | 24 | 24 |
| empreinte | `AUDIO_UAF:detected-without-fault:audio_lifetime_detector` | — |

La reproduction est exacte, y compris les frames :

```
SAMPLE_STALE_REFERENCE_AT_FREE frame=18141 overlay=84 bank=19 sample=1299 voice=44 voice_state=2 ...
SAMPLE_STALE_REFERENCE_AT_FREE frame=18141 overlay=84 bank=19 sample=1299 voice=49 voice_state=2 ...
SAMPLE_STALE_REFERENCE_AT_FREE frame=18141 overlay=84 bank=19 sample=1191 voice=46 voice_state=2 ...
SAMPLE_STALE_REFERENCE_AT_FREE frame=18141 overlay=84 bank=19 sample=1128 voice=38 voice_state=2 ...
AUDIO_STALE_SAMPLE_READ        frame=18142 voice=49 bank=19 sample=1299 freed_frame=18141 ...
AUDIO_STALE_SAMPLE_READ        frame=18142 voice=44 bank=19 sample=1299 freed_frame=18141 ...
AUDIO_STALE_SAMPLE_READ        frame=18142 voice=38 bank=19 sample=1128 freed_frame=18141 ...
AUDIO_STALE_SAMPLE_READ        frame=18142 voice=46 bank=19 sample=1191 freed_frame=18141 ...
```

Quatre voix vivantes (`voice_state=2`) encore à l'intérieur de la banque au
moment du `BANK_FREE`, puis leur lecture après libération **une frame plus
tard**. C'est exactement le diagnostic d'origine, retrouvé sans le chercher.

Noter que le processus **n'a pas fauté** : il a continué jusqu'à la frame 20225.
C'est la règle de la campagne qui le classe `CRASH` — « une violation de durée de
vie est un défaut même si rien ne plante » — et c'est ce qui rend ce scénario
utile : sans le détecteur, ce run aurait été un `PASS`.

---

## D4 — débordement de pile de coroutine

**Correctif annulé** : `stack_size *= 4` et le plancher de 32 Ko ramenés à
`stack_size *= 2` (`src/game/process.c`), c'est-à-dire les 8192 octets alloués au
processus de l'événement Big Boo pour un besoin mesuré de 8200.

**Scénario** : `d4-big-boo` — `w04-board-replay`, 900 secondes, qui traverse
l'événement Big Boo à la frame 48671.

| | correctif absent | correctif présent |
|---|---|---|
| résultat | **CRASH** | **PASS** |
| frame atteinte | **48671** | 53490, soit 4819 au-delà |
| code de sortie, les deux pairs | `0xC0000005 EXCEPTION_ACCESS_VIOLATION` | `0x00000000` |
| classification | `PROCESS_CRASH` / `PROCESS_CRASH` | terminaison normale |
| mismatch / violations audio / corruption tas | — | 0 / 0 / aucune |

**Frame 48671 exactement** — la frame documentée dans l'entrée D4, atteinte sans
la chercher, sur les deux pairs, avec le code de sortie que le registre attribue
au cas « avec page de garde ». Le scénario couvre bien le chemin.

### Ce que cette direction a révélé en plus : D9

Le dossier de ce run **ne contient ni rapport de plantage, ni minidump, ni
fichier `stack-usage-*`**. Le rapporteur a été muet sur un vrai
`EXCEPTION_ACCESS_VIOLATION`, et le run a donc été classé `CRASH` **sans
empreinte** — impossible à regrouper avec une récidive.

D4 est corrigé, donc ce plantage précis ne peut pas arriver dans un binaire
livré. Mais le silence du rapporteur, lui, n'est pas corrigé : c'est D9 au
registre. Et cette expérience lui fournit ce qu'aucune session n'avait jamais
produit — **un plantage du moteur reproductible à la frame près et à volonté**,
qui est exactement le banc d'essai dont le rapporteur a besoin.

La campagne attribue désormais `NO_REPORT:<exception>:overlay<N>` à un plantage
muet, pour pouvoir au moins compter les occurrences. Ce n'est pas un correctif.

### Et une erreur de lecture, la mienne, qu'il vaut mieux écrire

Les marqueurs de couverture du run **tronqué** annonçaient sept mécaniques et pas
de `BOO`, ce que j'ai d'abord présenté comme une propriété de l'enregistrement.
C'était faux : le run mourait à la frame 48 671 et `BOO_HOUSE` et `BOO` arrivent
aux frames 50 176 et 50 780. Le run complet en signale neuf.

**Une liste de couverture n'est valable que pour le run qui l'a produite.** Un run
qui s'arrête tôt sous-déclare, et ne signale rien d'anormal en le faisant — il
faut donc lire la frame finale avant de lire la couverture. C'est corrigé dans
`docs/netplay_validation_matrix.md`, à l'endroit même où la règle s'applique.

---

## D13 — le pont du lanceur ne reconnaissait plus les paquets du jeu

**Trouvé le 2026-09-12, en préparant le paquet à transférer sur un second PC.**

Deux programmes doivent s'accorder sur la forme d'un datagramme.
`src/port/netplay_transport.cpp` l'émet ; le pont de `tools/online/Connection.cs`
le relaie entre les deux PC, et ne le relaie **que s'il le reconnaît** :

```csharp
internal static bool Packet(byte[] b,int player) {
    return b.Length==GameDatagram.Payload      // 88
        && b[0]==80 && b[1]==66 && b[2]==82 && b[3]==66
        && b[4]==0 && b[5]==6                  // protocole v6
        && b[6]>=1 && b[6]<=3 && b[7]==player;
}
```

Les deux nombres avaient été recopiés à la main depuis l'en-tête du moteur. Le
2026-09-10, `0e98fb1e` (« netplay: complete canonical deterministic state
hashing ») a porté le paquet de **88 à 152 octets** et le protocole de **6 à
7** — seize sous-systèmes de plus dans le hachage canonique, soit
`84 + 16 × 4 + 4 = 152`. La copie C# est restée à 88 et 6.

### Ce que cela produisait

À partir de ce commit, le pont rejetait **100 % du trafic de jeu**, sans
afficher la moindre erreur : `continue` sur chaque paquet. Les deux salons se
connectaient, les empreintes de disque concordaient, l'hôte lançait la partie,
les deux jeux démarraient — puis chacun attendait deux minutes et mourait sur

```
[NET] ERROR frame=0: Aucun joueur compatible apres 2 minutes.
```

`ProgressFailure::NoPeer` signifie exactement cela : aucun paquet de pair n'a
jamais été accepté.

### Pourquoi rien ne l'a dit pendant deux jours

Aucun test local ne traverse le pont. Les campagnes utilisent
`--netplay-host` / `--netplay-join` directement sur `127.0.0.1` : les deux jeux
se parlent en direct, le lanceur n'est pas dans le chemin. Plus de cent
sessions vertes cette nuit-là ne disaient rien du tout de ce défaut.

Le seul test qui le traversait est `Tests.Tls(...)` du lanceur, et il n'avait
pas tourné : la porte de publication qui l'exécute ne se déclenche plus que sur
une étiquette depuis W0.

### La preuve

`tools/test_wire_format.ps1`, vu rouge puis vert, `Connection.cs` remis dans
son état du 10 septembre puis restauré :

| état de `Connection.cs` | résultat |
|---|---|
| littéraux 88 / v6 (état du 10 septembre) | **FAIL (4)** — les quatre contrôles de provenance |
| constantes générées | PASS |

### Le correctif

Les deux nombres ne sont plus recopiés : `tools/build_online.ps1` les lit dans
`include/port/netplay_transport.hpp` et génère `WireFormat.generated.cs`, que
`Connection.cs` référence. La dérive n'est plus possible par transcription.
`test_wire_format.ps1` garde ce qui reste : supprimer le générateur, ou
réécrire un littéral à côté.

### Ce que ce défaut apprend

Une constante partagée entre deux langages sans mécanisme la liant est une
divergence en attente. Et un test qui ne tourne jamais protège exactement
autant qu'un test qui n'existe pas : celui-ci existait, était correct, et
aurait attrapé le défaut le jour même.

---

## Ce que cette page ne prouve pas

Elle prouve que **ces trois scénarios touchent ces trois chemins**. Elle ne
prouve pas que les correctifs soient complets : D3 est vérifié sur la libération
de banque de l'écran de résultats et non sur toute libération possible, et D5 sur
les distances 1, 2 et 4 et non sur toutes.

Elle ne dit rien non plus de D1, D2, D6, D7, D8 et D9, qui ne sont pas corrigés et
n'ont donc rien à annuler.

## À refaire, et quand

À chaque fois qu'un de ces trois correctifs est touché, et avant toute release.
Le coût total est d'environ quarante minutes de machine et deux reconstructions
par défaut. C'est le prix de la différence entre « le test est vert » et « le test
serait rouge si le défaut revenait ».
