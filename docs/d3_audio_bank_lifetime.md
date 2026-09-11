# D3 — durée de vie des banques audio MusyX : la preuve

Ce document contient la preuve de D3, pas son correctif. Il est écrit avant
toute modification du comportement, pour que la correction puisse être jugée
sur ce qu'elle change réellement.

La question posée était précise :

> au moment où une banque est libérée, une voix audio possède-t-elle encore une
> référence valide vers cette banque ?

La réponse est **oui**, observée, datée, nommée, et identique sur les deux pairs.

---

## 1. Ce que le code fait réellement

Trois lectures ont été nécessaires avant de pouvoir instrumenter quoi que ce
soit, parce que la première hypothèse était fausse.

### 1.1 L'adresse qu'une voix lit n'est pas celle que l'on croit

Sur PC, une banque est copiée **deux fois** :

1. `sndPushGroup` (`extern/musyx/src/musyx/runtime/s_data.c:604`) copie le blob
   d'échantillons du disque dans un `malloc` intermédiaire (`sampleCopy`).
2. Puis, pour **chaque échantillon**, `hwSaveSample`
   (`extern/musyx/src/musyx/runtime/hardware.c:600`) appelle `aramStoreData`,
   qui fait **encore un `malloc` par échantillon**
   (`hw_aramdma.c:466`), y recopie les octets, et **réécrit `sdir->addr`** pour
   pointer sur cette seconde copie.

`dataGetSample` (`synthdata.c:673`) recopie ensuite cette adresse telle quelle
dans `newsmp->addr`, et c'est elle que la voix porte dans `vp->smp_info.addr`.
Donc **`vp->smp_info.addr` est exactement l'adresse d'une allocation ARAM
émulée par échantillon**, jamais un pointeur intérieur au blob intermédiaire.

Cela a été découvert par l'instrumentation elle-même : une première version
comparait les voix au blob intermédiaire et classait **7724 voix sur 7724**
comme n'appartenant à aucune banque. Un détecteur qui ne voit rien et un
détecteur qui regarde au mauvais endroit produisent le même silence ; seul le
compteur « non attribué » permet de les distinguer. Après correction,
**3443 voix sur 3449** sont attribuées, les six restantes passant par un autre
chemin (tampons de flux).

### 1.2 Le « kill » de voix n'arrête pas la voix

`sndPopGroup` appelle `synthKillVoicesByMacroReferences`, qui appelle
`voiceKill`, qui appelle `hwBreak` (`hardware.c:215`) :

```c
void hwBreak(s32 vid) {
  if (dspVoice[vid].state == 1 && salTimeOffset == 0) {
    dspVoice[vid].startupBreak = 1;
  }
  dspVoice[vid].changed[salTimeOffset] |= 0x20;
}
```

C'est **une demande, pas un arrêt**. Le bit `0x20` sera lu par le thread audio
lors de son prochain rendu ; d'ici là `vp->state` reste non nul, la voix reste
dans `stp->voiceRoot`, et elle **continue de lire son échantillon**.

### 1.3 Les voix référençant un échantillon sans macro correspondante ne sont même pas prévenues

```c
  synthKillVoicesByMacroReferences((u16*)((u8*)prj + g->macroOff));
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  synthKillVoicesBySampleReferences((u16*)((u8*)prj + g->sampleOff));
#endif
```

`extern/musyx/CMakeLists.txt` fixe `MUSY_VERSION_MAJOR=1`, `MINOR=5`,
`PATCH=4`, et `version.h` ne définit ses propres valeurs que sous `#ifndef`.
La version compilée est donc **1.5.4**, strictement inférieure à 2.0.1 :
**`synthKillVoicesBySampleReferences` est exclue à la compilation**. La fonction
existe (`synthvoice.c:870`) mais n'est jamais appelée.

### 1.4 La libération se fait hors du verrou audio

`hwDisableIrq()` et `hwEnableIrq()` sont `SDL_LockMutex`/`SDL_UnlockMutex` sur
`globalMutex` (`hw_pc.c:1891-1893`), et le thread audio tient ce même verrou
pendant tout `salCtrlDsp` (`hardware.c:52-56`). Or :

```c
bool sndPopGroup() {
  ...
  hwDisableIrq();                                  /* verrou pris          */
  synthKillVoicesByMacroReferences(...);           /* simple demande       */
  hwEnableIrq();                                   /* verrou rendu         */
  RemoveSamples((u16*)((u8*)prj + g->sampleOff), sdir);
    -> ScanIDListReverse(ref, NULL, 1, 1)          /* AUCUN verrou         */
       -> dataRemoveSampleReference(sid)
          -> hwRemoveSample(&sdir->header, sdir->addr)
             -> free(data)                         /* LA libération        */
```

Le `free` qui compte est celui de `hwRemoveSample` (`hardware.c:642`), et il
s'exécute **sans le verrou**, donc concurremment au rendu.

---

## 2. Ce que l'instrumentation a mesuré

Build `7eee9a4d`, replay enregistré `work/netplay-recordings/board-replay.txt`,
deux pairs réels, `PARTYBOARD_AUDIO_DIAGNOSTICS=1`.

Frame **18141** : l'overlay 84 (`resultDll`, l'écran de résultats de mini-jeu)
est déchargé et son jeu de banques remplacé. Trace du pair 0, dans l'ordre :

```
BANK_AUDIO_DRAIN_BEGIN          frame=18141 overlay=84 irq=60725 steps=30
BANK_AUDIO_DRAIN_END            frame=18141 overlay=84 irq=60725 irq_total=60725
BANK_RELEASE_REQUEST            frame=18141 bank=19 generation=626 group=112 samples=22
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 bank=19 generation=640 sample=1300 voice=37 voice_state=2 pos=2680  sample_length=5456
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 bank=19 generation=640 sample=1300 voice=39 voice_state=2 pos=4867  sample_length=5456
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 bank=19 generation=632 sample=1191 voice=12 voice_state=2 pos=1534  sample_length=1584
BANK_FREE                       frame=18141 bank=19 generation=626 group=112 samples=22
AUDIO_STALE_SAMPLE_READ         frame=18142 voice=12 bank=19 generation=632 sample=1191 freed_frame=18141
```

Et sur le pair 1, à la même frame, sur les mêmes banques, les mêmes
échantillons et les mêmes générations :

```
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1300 voice=16 pos=2680
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1300 voice=37 pos=4867
SAMPLE_STALE_REFERENCE_AT_FREE  frame=18141 sample=1191 voice=38 pos=1534
AUDIO_STALE_SAMPLE_READ         frame=18142 voice=38 sample=1191 freed_frame=18141
```

Seuls les **numéros de slot de voix** diffèrent entre les deux pairs ; les
échantillons, les positions de lecture et les frames sont identiques. Le défaut
est donc déterministe, et sa visibilité (le crash) ne l'est pas.

La chaîne demandée est complète :

| maillon | preuve |
|---|---|
| `overlay unload` | `frame=18141 overlay=84`, l'overlay 84 est déchargé à cette frame |
| `bank lifetime` | `BANK_RELEASE_REQUEST` puis `BANK_FREE` pour la banque 19, groupe 112 |
| `voice still references bank/sample` | trois voix en `voice_state=2`, positions 2680, 4867 et 1534 **à l'intérieur** de leurs échantillons |
| `audio thread access` | `AUDIO_STALE_SAMPLE_READ` à la frame 18142, une frame après la libération |
| `crash` | `AUDIO_FAULT:overlay84:ensureADPCMBlockDecoded@hw_pc.c:711`, même instruction, quand la mémoire libérée est devenue illisible |

---

## 3. Ce que cela dit du correctif C3, et donc de mon propre travail

Deux lignes de la trace suffisent :

```
BANK_AUDIO_DRAIN_BEGIN  irq=60725  steps=30
BANK_AUDIO_DRAIN_END    irq_total=60725
```

`irq` est le compteur d'interruptions audio, incrémenté une fois par
`snd_handle_irq`, c'est-à-dire une fois par rendu réel. **Il n'a pas bougé
pendant les trente itérations du drain.** Le drain C3 fait avancer l'état msm
côté thread de jeu et n'attend rien du thread audio, parce que le thread audio
est un thread SDL cadencé par le périphérique, pas par `msmSysRegularProc`.

Donc, très précisément :

- **Déterminisme : le drain fait ce qu'il promet.** Un nombre fixe d'itérations
  garantit que les deux pairs décident la transition à la même frame de
  simulation, ce que la borne en temps mur ne garantissait pas. Les deux
  traces ci-dessus, frame pour frame identiques, le confirment.
- **Sûreté de durée de vie : le drain n'y contribue pas du tout.** Mesuré, pas
  supposé : zéro callback audio pendant le drain.

Augmenter `SNDGRP_DRAIN_STEPS` ne changerait rien, puisque le compteur
n'avancerait pas davantage. C'est l'illustration exacte de la règle : *un gros
timeout n'est pas une preuve de durée de vie correcte*. Un mécanisme qui
**décide** est nécessaire, pas un mécanisme qui **attend**.

---

## 4. Les deux trous à fermer

1. **`hwBreak` est asynchrone.** Une voix à qui l'on a demandé de s'arrêter
   continue de lire son échantillon jusqu'à ce que le thread audio traite la
   demande. Le correctif doit obtenir un arrêt **effectif**, pas une demande.
2. **La libération se fait hors du verrou audio.** Même une voix correctement
   arrêtée ne suffit pas si le `free` peut s'exécuter pendant que le thread
   audio est à l'intérieur de `salCtrlDsp`.

Un correctif qui ne ferme que l'un des deux laisse le défaut ouvert.

---

## 5. Comment reproduire cette mesure

```
tools\netplay_campaign.ps1 -DiscPath <iso> -Scenario w04-results-unload -Repeat 1
```

ou directement :

```
$env:PARTYBOARD_AUDIO_DIAGNOSTICS = '1'
tools\test_netplay_boot.ps1 -DiscPath <iso> `
    -ReplayInput work\netplay-recordings\board-replay.txt `
    -OutputDirectory work\d3 -DurationSeconds 400
```

puis :

```
Select-String -Path work\d3\*\audio-lifetime-peer-*.txt -Pattern 'STALE|RETIRED'
```

La première violation arrive à la frame 18141, soit environ 305 secondes de jeu.
