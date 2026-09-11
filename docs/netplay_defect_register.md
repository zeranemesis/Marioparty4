# Registre des défauts trouvés et non encore corrigés

Un défaut est inscrit ici dès qu'il est identifié, avec sa classification
technique exacte et ce qui reste à prouver. Un défaut n'est retiré du registre
que lorsqu'un test déterministe démontre sa correction.

La règle de classification : on ne relie jamais deux défauts sans preuve. Un
défaut trouvé sur le chemin d'un crash n'est pas pour autant la cause de ce
crash.

---

## D1 — `BoardSpaceCornerPosGet` : index de coin non borné

**Classification : confirmed unbounded array index, producing an out-of-bounds
read of an 8-byte stack array.**

**Statut : non corrigé. Détection instrumentée, correction différée.**

### Le code

`src/game/board/space.c:165` :

```c
void BoardSpaceCornerPosGet(s32 index, s32 corner, Vec *pos)
{
    Vec corner_ofs;
    Vec rot;
    s8 corner_pos[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    BoardSpaceRotGet(0, index, &rot);
    BoardSpacePosGet(0, index, pos);
    corner_ofs.x = corner_pos[corner][0] * 80.0f;
    corner_ofs.y = 0;
    corner_ofs.z = corner_pos[corner][1] * 80.0f;
    ...
}
```

`corner_pos` est un tableau local de 4 × 2 octets, soit **8 octets sur la pile**.
`corner` n'est borné **ni vérifié nulle part**.

### L'appelant

`src/REL/w04Dll/boo_event.c:1060`, dans `fn_1_52A0` :

```c
var_r27 = 0;
for (i = 0; i < 4; i++) {
    ...
    sp8[i] = GWPlayer[i].space_curr;
    if (lbl_1_bss_B0 == sp8[i]) {
        var_r27 += 1;            /* 0..4 */
    }
}
for (i = 0; i < 4; i++) {
    var_r30 = fn_1_2FBC(i);
    if (var_r30->unk08 != -1 && var_r30->unk08 != GWSystem.player_curr) {
        BoardSpaceCornerPosGet(lbl_1_bss_B0, var_r27++, &arg1[var_r30->unk08]);
    }
}
```

`var_r27` part du **nombre de joueurs déjà présents sur la case cible** (0 à 4),
puis s'incrémente une fois par itération qui passe le garde.

Borne atteignable :

| Condition | Valeur maximale de `corner` |
|---|---|
| 4 joueurs sur la case cible, 4 emplacements qualifiants | **7** |
| 4 joueurs sur la case, 3 joueurs distincts non courants | **6** |
| Hors-bornes dès que | **`corner >= 4`** |

Condition nécessaire : `joueurs_sur_la_case + emplacements_qualifiants > 4`.

### Ce que cela fait, et ce que cela ne fait pas

**Ce que cela fait.** Pour `corner = 7`, la fonction lit
`((s8*)corner_pos)[14]` et `[15]`, soit **6 et 7 octets au-delà** d'un tableau de
8 octets sur la pile. Les valeurs lues sont multipliées par `80.0f` et
deviennent un décalage de position : un modèle de joueur placé à une coordonnée
arbitraire.

**Ce que cela ne fait pas.**

- Ce n'est **pas** la corruption de tas de la session Big Boo. C'est une
  **lecture**, et de la pile, pas du tas. Une lecture ne peut pas lever
  `STATUS_HEAP_CORRUPTION` (`0xC0000374`).
- Ce n'est **pas** une cause de désynchronisation entre pairs. Les octets
  adjacents lus dépendent de la disposition de pile choisie par le compilateur,
  identique pour un même binaire, et de l'exécution antérieure, qui est
  déterministe. Les deux pairs liraient donc les mêmes octets.
- L'écriture `&arg1[var_r30->unk08]` est, elle, **dans les bornes** :
  `arg1` est `lbl_1_bss_80[4]` (`boo_event.c:87`) et `unk08` est un index de
  joueur 0..3 ou -1, le -1 étant exclu par le garde.

### Ce qui reste à prouver

Le chemin est-il réellement emprunté en jeu ? La condition exige au moins cinq
occupants cumulés, ce qui n'arrive pas dans une partie à deux joueurs où les
emplacements inactifs ne qualifient pas. **Une détection est donc instrumentée**
dans `BoardSpaceCornerPosGet` : elle signale tout appel avec `corner >= 4` dans
le fil d'événements du rapport de crash, sans modifier l'arithmétique. Une
session réelle dira si le cas se produit.

### Correction envisagée, volontairement différée

Borner `corner` dans `BoardSpaceCornerPosGet`, et corriger le comptage dans
`fn_1_52A0` pour qu'il ne puisse pas dépasser le nombre de coins. Ce correctif
appartient à un commit distinct du rapporteur de crash et de l'instrumentation
de pile, et ne doit pas être appliqué avant que la détection ait dit si le
chemin est vivant.

---

## D2 — `fn_1_52A0` voisin : emplacement écrasé quand les quatre sont occupés

**Classification : logic defect, no memory error.**

**Statut : non corrigé, observé en lisant D1, non instrumenté.**

`src/REL/w04Dll/boo_event.c:956-962` :

```c
for (i = 0; i < 4; i++) {
    var_r29 = fn_1_2FBC(i);
    if (var_r29->unk08 == -1) {
        break;
    }
}
var_r29->unk08 = temp_r30;
```

Si les quatre emplacements ont déjà `unk08 != -1`, la boucle se termine sans
`break` et `var_r29` conserve le dernier emplacement visité, `fn_1_2FBC(3)`, dont
l'affectation est alors **écrasée**. Il n'y a pas d'accès hors-bornes — `var_r29`
pointe toujours sur un emplacement valide — mais un joueur perd son association.

Aucune preuve que ce cas se produise. Inscrit pour ne pas le redécouvrir.

---

## D3 — Banque audio libérée sous une voix encore en lecture — **PROUVÉ**

**Classification : use-after-free of a MusyX sample allocation, game thread frees
while the audio thread still reads. Deterministic; its crash is not.**

**Statut : cause prouvée, correction en cours. Preuve complète dans
[`docs/d3_audio_bank_lifetime.md`](d3_audio_bank_lifetime.md).**

### Ce qui est établi

Au déchargement de l'overlay 84 — `resultDll`, l'écran de résultats de mini-jeu,
d'après la table `include/ovl_table.h` — le jeu libère les banques audio du jeu
d'overlays sortant alors que des voix DSP en lisent encore les échantillons.

Mesuré à la frame **18141** et à la frame **27744**, sur les deux pairs, avec les
mêmes banques, les mêmes échantillons, les mêmes positions de lecture :

```
BANK_RELEASE_REQUEST            frame=18141 bank=19 group=112 samples=22
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1300 voice=37 voice_state=2 pos=2680
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1300 voice=39 voice_state=2 pos=4867
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1191 voice=12 voice_state=2 pos=1534
BANK_FREE                       frame=18141 bank=19
AUDIO_STALE_SAMPLE_READ         frame=18142 voice=12 sample=1191 freed_frame=18141
```

La frame 27744 est exactement celle des deux crashes historiques, et la lecture
invalide `ensureADPCMBlockDecoded+0xa4` (`hw_pc.c:711`) en est la conséquence
visible.

### Les deux causes, dans le code

1. **`hwBreak` ne fait qu'une demande.** `sndPopGroup` tue les voix par
   `synthKillVoicesByMacroReferences` → `voiceKill` → `hwBreak`, et `hwBreak`
   (`hardware.c:215`) se contente de lever le bit `0x20` que le thread audio
   lira à son prochain rendu. La voix continue de lire jusque-là.
2. **La libération se fait hors du verrou audio.** `sndPopGroup` rend
   `globalMutex` avant `RemoveSamples`, et le `free` réel — celui de
   `hwRemoveSample` sur la copie ARAM par échantillon — n'est protégé par rien.

S'y ajoute que `synthKillVoicesBySampleReferences` est **exclue à la
compilation** : `extern/musyx/CMakeLists.txt` fixe la version MusyX à 1.5.4 et le
garde dans `s_data.c` demande 2.0.1 ou plus. Une voix référençant un échantillon
du groupe sans macro correspondante n'est donc même pas prévenue.

### Ce que cela dit du correctif C3

`HuAudSndGrpWait` a été rendu déterministe par un nombre fixe d'itérations. La
trace montre `BANK_AUDIO_DRAIN_BEGIN irq=60725` et `BANK_AUDIO_DRAIN_END
irq_total=60725` : **le compteur de callbacks audio n'a pas avancé d'une seule
unité pendant le drain**. Le drain garantit le déterminisme et **rien** de la
durée de vie. Augmenter le nombre d'itérations ne changerait pas ce zéro.

### Correction retenue

Une barrière au point exact de la libération : dans `hwRemoveSample`, sous
`globalMutex`, détacher toute voix DSP pointant encore dans l'allocation, puis
libérer. Sous le verrou le thread audio n'est pas dans `salCtrlDsp`, et après le
détachement il ne peut plus y entrer sur cette adresse. Aucun délai, aucun
`Sleep`, aucun nombre de frames arbitraire.

---

## D4 — Débordement de pile de coroutine HuPrc — **CORRIGÉ**

**Classification : coroutine stack exhaustion, confirmed at instruction level.**

**Statut : corrigé par `875a1ef7`, vérifié deux fois.**

Conservé ici parce que le chemin qui a mené à sa découverte est réutilisable.

`fn_1_30A4` (`src/REL/w04Dll/boo_event.c:343`) demandait la constante `0x1000`,
doublée à 8192 octets sur PC, pour un besoin mesuré de **8200 octets**. Il
débordait de **huit octets**, exactement l'adresse de retour d'un `call`.

Deux manifestations selon la présence d'une page de garde :

| Build | Pile de coroutine | Symptôme |
|---|---|---|
| `bc63c93c` | bloc `malloc` nu | écrase l'en-tête de tas voisin → `0xC0000374` |
| avec page de garde | réservation gardée | faute sur la page de garde → `0xC0000005` |

**Pourquoi il a résisté si longtemps.** Ce mode de défaillance détruit ses
propres preuves. Le `call` qui déborde ne peut pas empiler son adresse de
retour ; le noyau ne peut pas davantage empiler un cadre d'exception ; aucun
gestionnaire en mode utilisateur ne s'exécute. De plus, le compteur de marque
d'eau ne mesure que les piles retirées ou vivantes au moment d'un rapport : la
pile qui déborde tue le processus à cet instant et n'est **jamais** mesurée.
Tous les pics jamais imprimés étaient des survivants, ce qui a fait passer
l'hypothèse pour réfutée avec un maximum rassurant de 27 %.

La sortie a été la sémantique `PAGE_GUARD` de Windows, qui lève une exception
*et* débloque la page dans le même geste, laissant assez de pile pour rapporter.
