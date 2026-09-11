# Ce dont le hash canonique ne peut rien dire

**Date : 2026-09-11. Lecture de code seule, aucune mesure. Chaque entrée dit
explicitement ce qui est établi et ce qui ne l'est pas.**

## Pourquoi ce document existe

Le hash canonique est l'instrument principal du projet : deux pairs qui
produisent le même hash à la même frame ont calculé le même état. Toute la
campagne repose dessus.

Mais `include/port/netplay_canonical.hpp` le dit lui-même :

> Coverage is deliberately explicit: only values whose meaning is identical on
> two machines are hashed.

C'est correct, et c'est aussi une limite : **tout ce qui est exclu du hash est
exactement ce dont le hash ne peut rien dire.** Une divergence dans un champ
non haché ne produit aucun `mismatch`. Elle se manifeste plus tard, ailleurs,
sous une forme qui ne ressemble pas à sa cause — ou pas du tout, jusqu'au jour
où elle atteint un champ haché.

Ce document lit cette liste face aux chemins non testés. Il ne coûte aucune
minute machine, et c'est pour cela qu'il passe avant les chantiers qui en
coûtent.

## Méthode

Pour chaque champ exclu : qui l'écrit, qui le lit, et un lecteur peut-il changer
la simulation ? Trois verdicts seulement.

| verdict | sens |
|---|---|
| **SÛR** | l'exclusion est correcte et la raison est établie par lecture |
| **NEUTRALISÉ** | le champ pourrait diverger, mais un bridage explicite l'empêche en ligne — et ce bridage est alors porteur, comme celui de D6 |
| **CANDIDAT** | un chemin existe du champ vers la simulation, et rien ne prouve qu'il soit pris ou qu'il ne le soit pas |

Aucun champ n'est déclaré sûr parce qu'il « a l'air » d'une préférence.

---

## `GameStat` — champs non hachés

Les champs hachés sont `language`, `total_stars`, `customPackEnable`,
`veryHardUnlock`, `open_w06`, `party_continue`, `story_continue`, `mg_custom`
et `mg_record`. Les suivants ne le sont pas.

### `create_time` — **CANDIDAT**

`src/game/saveload.c:384` :

```c
create_time = OSGetTime();
GWGameStat.create_time = create_time;
```

`OSGetTime()` est l'horloge murale. Les deux pairs l'appellent au même moment
logique et obtiennent **deux valeurs différentes**, par construction. L'exclure
du hash est donc nécessaire : l'y inclure ferait échouer toute partie qui
sauvegarde.

Mais l'exclusion ne fait pas disparaître la divergence, elle la rend invisible.
Deux conséquences lues :

1. `SLCommonSet` copie `GWGameStat` entier dans le tampon de sauvegarde. **Les
   deux machines écrivent donc des fichiers de sauvegarde différents**, sur huit
   octets au moins.
2. `src/REL/modeseldll/filesel.c:1994` relit ce champ par emplacement :
   `lbl_1_bss_D0[boxno] = GWGameStat.create_time;` — l'écran de sélection de
   fichier affiche et compare des valeurs qui diffèrent entre les deux machines.

**Aucune session n'a jamais atteint ce chemin.** La fin de partie le traverse.
C'est la raison pour laquelle ce champ est cité en premier : W3 y enverra deux
humains, et c'est le premier endroit où deux fichiers de sauvegarde divergents
deviendront visibles.

À instrumenter avant W3, pas après.

### `rumble` — **CANDIDAT**

Il faut distinguer deux choses qui portent le même nom.

**`RumbleBit`, l'état matériel — NEUTRALISÉ.** `src/game/pad.c:298` le règle
depuis le masque que le moteur lui passe, et sous netplay
`src/port/netplay_runtime.cpp:1577` force :

```cpp
*rumble = PAD_CHAN0_BIT | PAD_CHAN1_BIT;
```

Une constante, identique sur les deux pairs, quels que soient les manettes
réellement branchées. `RumbleBit` fait partie de l'instantané de rollback
(`padSnapshotRegions`) sans être haché, et ce bridage est ce qui rend cette
absence inoffensive. **C'est exactement la structure de D6** : une propriété de
sûreté porteuse qui ressemble à un détail. Si quelqu'un fait dépendre ce masque
des manettes réelles, l'absence de `RumbleBit` dans le hash redevient dangereuse
et rien ne le signalera.

**`GWGameStat.rumble`, la préférence sauvegardée — CANDIDAT.** Elle n'est bridée
par rien. Six modules de mini-jeu la lisent :

| module | ligne |
|---|---|
| `m428Dll` | `main.c:122` |
| `m442Dll` | `main.c:2282` |
| `m455Dll` | `main.c:817` |
| `m456Dll` | `main.c:471` |
| `m459dll` | `main.c:282` |
| `option/rumble.c` | `51` |

Dans `m442Dll`, la valeur retournée par `fn_1_90FC()` est écrite dans un champ
**par joueur** (`main.c:517`), pas seulement utilisée pour décider d'un appel au
moteur. Si ce champ entre dans la logique du mini-jeu, deux pairs dont les
préférences de vibration diffèrent divergent, et la divergence apparaîtra dans
un hash de mini-jeu sans que rien n'en désigne la cause.

**Ce qui est établi** : la préférence est locale, non bridée, et atteint une
structure par joueur. **Ce qui ne l'est pas** : si ce champ change le résultat.
Le dire exigerait de lire beaucoup plus de ce module décompilé, ou de le mesurer.

Aucun de ces six mini-jeux n'est dans la matrice. Ils seront atteints par le
balayage des 58 mini-jeux de W2, et c'est là qu'il faudra regarder.

### `sound_mode` — **SÛR**

Écrit depuis `msmSysGetOutputMode()` (mono/stéréo/surround) et lu par
`src/REL/option/sound.c:496` pour positionner un curseur de menu. Aucun lecteur
hors du menu des options, et le menu des options n'est pas un contexte en ligne.

### `mg_avail` — **NEUTRALISÉ, et déjà prouvé**

Les bits de déblocage des mini-jeux. `src/game/gamework.c:462` les consulte dans
`GWMGAvailGet`, qui **décide réellement** de la disponibilité — un champ qui
gouverne la logique et n'est pas haché serait normalement grave.

Il ne l'est pas, parce que sous netplay tout est disponible quels que soient les
bits, et **c'est déjà couvert par un test** : le sous-test
`minigame-availability` de `--netplay-self-test` exécute 768 vérifications avec
les bits sauvegardés et le réglage de triche délibérément différents, et exige
`(online || cheat || savedAvailable)`. En ligne, `online` est vrai : la valeur
sauvegardée ne peut pas changer la réponse.

C'est le modèle de ce qu'il faut faire des autres entrées de ce document.

### `board_win_count`, `board_play_count`, `board_max_stars`, `board_max_coins` — **CANDIDAT faible**

Statistiques, écrites en fin de partie par `src/game/gamework.c:393-427`. Elles
divergeront entre les deux machines dès la première partie terminée, puisque
chacune écrit son propre fichier.

Un seul lecteur sort des statistiques : `src/REL/option/sound.c:519` —
`musicPageOn[i + 3] = (GWGameStat.board_play_count[i] != 0)`, qui débloque des
pistes dans le juke-box. Hors contexte en ligne.

**Ce qui n'est pas établi** : si un déblocage dépendant de ces compteurs peut
changer quelque chose *pendant* une partie. Aucune session n'a jamais terminé
une partie, donc aucune n'a jamais écrit ces champs.

### `present[60]` — **CANDIDAT faible**

Cadeaux débloqués, écrits par une douzaine de modules de mini-jeu
(`m405Dll:2224`, `m407dll:326`, `m427Dll:293`, `m432Dll:3450`, `m443Dll:388`,
`m451Dll:1398`, …) lorsqu'une condition est remplie **pendant** un mini-jeu.

C'est la seule entrée de cette liste écrite *en cours de partie* plutôt qu'à la
sauvegarde. Deux pairs qui exécutent le même mini-jeu devraient écrire la même
chose au même moment ; si l'écriture dépend d'un état local, ils ne le feront
pas, et rien ne le dira. À vérifier lors du balayage des mini-jeux de W2.

### `musicAllF` — **SÛR**

Écrit une seule fois par `mstory2Dll/ending.c:300`, lu uniquement par le
juke-box.

### `story_pause` / `party_pause` — **CANDIDAT**

Ce sont les seuls champs de cette liste dont un chemin **vers des champs hachés**
est établi par lecture.

`src/game/board/pause.c:154-165` recopie cinq réglages dans ces sauvegardes :

```c
GWGameStat.story_pause.explain_mg = GWMGExplainGet();
GWGameStat.story_pause.show_com_mg = GWMGShowComGet();
GWGameStat.story_pause.mg_list = GWMGListGet();
GWGameStat.story_pause.mess_speed = GWMessSpeedGet();
GWGameStat.story_pause.save_mode = GWSaveModeGet();
```

et `src/REL/modeseldll/main.c:212-221` les réinjecte :

```c
GWMGExplainSet(GWGameStat.party_pause.explain_mg);
```

Or `GWSystem.explain_mg`, `show_com_mg`, `mg_list`, `mess_speed` et `save_mode`
**sont hachés** (`netplay_canonical.hpp:42-45`). Le chemin complet est donc :
réglage local → champ non haché → retour dans un champ haché.

**Ce qui est établi** : le chemin existe, dans les deux sens, et les champs
d'arrivée sont hachés. **Ce qui ne l'est pas** : si `modeseldll` s'exécute dans
un contexte en ligne. Si oui, deux joueurs dont les réglages de pause diffèrent
produiraient un `mismatch` sur le sous-système `Gamework` sans qu'aucune action
de jeu ne l'explique — le pire genre de divergence, parce qu'elle ne ressemble
pas à sa cause.

C'est l'entrée la plus urgente de ce document après `create_time`.

---

## Ce que ce document n'a pas couvert

Il ne traite que `GameStat`. Les autres sous-systèmes ont leurs propres
exclusions — pointeurs, poignées, tampons de présentation, état audio physique —
dont le préambule dit qu'ils ne sont « jamais exportés ». Cette affirmation est
juste pour un pointeur, dont la valeur est une adresse. Elle mérite d'être relue
pour l'audio : `PartyBoard_RollbackAudioSelfTest` existe, mais la question « quel
état audio logique influence la simulation » n'a pas été posée dans ce sens.

À faire, toujours sans temps machine.

## Règle qui découle de tout ceci

Quand un bridage rend inoffensive l'absence d'un champ dans le hash — le taux
d'images pour D6, le masque de vibration pour `RumbleBit` — **ce bridage est une
propriété de sûreté, pas une préférence**, et il doit être épinglé par un test.
Sinon son retrait produit, des mois plus tard, une désynchronisation que personne
ne saura relier à un réglage.

`frame-rate-clamp` fait cela pour D6, dans `--netplay-self-test`. Le masque de
vibration est epingle a son tour, dans la sonde PAD (`--netplay-pad-probe`,
exercee par `tools/test_netplay_pad.ps1`) : la sonde tourne avec le netplay
reellement actif, donc elle peut verifier la valeur que le chemin en ligne
produit vraiment, ce qu'un auto-test en processus ne pourrait pas faire. Un
masque redevenu dependant des manettes fait rougir la sonde immediatement.

Les deux autres candidats de ce document — `create_time` et
`story_pause`/`party_pause` — n'ont pas de bridage a epingler : ils ont un
chemin ouvert et non mesure. C'est une difference importante, et c'est pourquoi
ils sont classes CANDIDAT et non NEUTRALISE.
