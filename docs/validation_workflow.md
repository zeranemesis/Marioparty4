# Comment on valide PartyBoard en ligne

Une page, pour savoir quoi lancer et dans quel ordre. Le détail de chaque outil
est dans son propre en-tête ; ici on ne dit que ce qui sert à décider.

## La règle qui gouverne tout le reste

**Un défaut découvert devient un test, et ce test reste dans la campagne.**
C'est la seule chose qui empêche un bug corrigé de revenir en silence. Corollaire
pratique : rien n'est marqué `PASS` sans qu'un test l'ait exercé, et un `FAIL`
n'est jamais rejoué jusqu'à obtenir un `PASS` par chance.

## Les portes, de la plus rapide à la plus lente

### 1. Secondes — les auto-tests sans le jeu

```bash
tools\test_crash_pipeline.ps1
```
```bash
tools\test_board_corner_index.ps1
```

Ces deux-là ne dépendent d'aucun code du jeu : ils compilent leurs propres
binaires et tournent même pendant qu'une campagne verrouille `dol.dll`. Le
premier couvre la confidentialité et le consentement des rapports de crash, le
second l'arithmétique du défaut D1.

### 2. Une minute — les auto-tests du binaire

```bash
build\aexp\RelWithDebInfo\partyboard.exe --netplay-self-test
```

Rollback, transport, PAD, hash canonique, rapporteur de crash, piles de
coroutine, durée de vie des banques audio, manifeste et uploader. Aucun jeu
graphique n'est lancé ; ce sont des tests de mécanique.

### 3. Deux minutes — la porte d'entrée

```bash
tools\netplay_campaign.ps1 -DiscPath "<iso>" -Scenario w04-boot-smoke
```

Deux vraies instances, boot, menus, entrée sur le plateau. À passer avant de
lancer quoi que ce soit de long.

### 4. Quinze minutes par run — la campagne

```bash
tools\netplay_campaign.ps1 -DiscPath "<iso>" -Scenario w04-results-unload -Repeat 20
```

Un résultat par run, et un seul : `PASS`, `CRASH`, `DESYNC`, `TIMEOUT`,
`ABNORMAL_EXIT`, `HARNESS_FAILURE`. Tout est conservé dans
`work/netplay-campaigns/<horodatage>/`. `-Resume <dossier>` reprend sans rejouer
ce qui est déjà `PASS` ; `-RerunFailures` doit être demandé explicitement.

Plusieurs campagnes peuvent tourner en parallèle avec `-Label` :

```bash
tools\netplay_campaign.ps1 -DiscPath "<iso>" -Scenario w04-results-unload -Repeat 7 -Label a
```

### 5. À la demande — le rollback local

```bash
tools\netplay_campaign.ps1 -DiscPath "<iso>" -Scenario w04-boot-smoke -ForceRollback 300
```

Toutes les 300 frames : sauvegarde, avance, restaure, rejoue, compare. L'écart
de retour tourne sur l'échelle 1, 2, 4, 8, 15, 30, 60, 120. Un échec écrit
`rollback-failure-*.txt` nommant le premier sous-système et le premier champ
divergents, et **arrête la session** — rien n'est réparé ni resynchronisé.

Aujourd'hui cela échoue dès la première frame, sur `ANIMATION`. C'est le défaut
D5 ; tant qu'il est là, le reste de la campagne rollback ne peut rien dire.

### 6. Humain — la session enregistrée

```bash
tools\record_board_session.ps1 -DiscPath "<iso>"
```

C'est la seule chose qu'aucun script ne peut produire : une partie jouée à la
main jusqu'à ce que le joueur décide lui-même de quitter. Le superviseur conclut
sur quatre lignes, et les quatre doivent être vertes :

```
DETERMINISM: PASS
STABILITY: PASS
USER_TERMINATED: YES
OVERALL: PASS
```

Une session réussie devient un enregistrement, l'enregistrement devient un
scénario, et le scénario rejoint la campagne. C'est ainsi que la couverture
grandit.

## Les enregistrements

```bash
tools\verify_replays.ps1
```

Les fichiers sont trop gros pour être versionnés ; leurs empreintes le sont.
Un enregistrement dont le `sha256` a changé est un autre enregistrement, et tout
résultat obtenu avec lui appartient à cet autre enregistrement.

## Où lire un résultat

| question | fichier |
|---|---|
| qu'est-ce qui est validé, et à quel point | `docs/netplay_validation_matrix.md` |
| quels défauts sont connus, et lesquels sont prouvés | `docs/netplay_defect_register.md` |
| où en sont les sessions humaines | `docs/netplay_100_percent_validation.md` |
| pourquoi telle cause a été retenue | `docs/NETPLAY_DETERMINISM_AUDIT.md` |
| ce qu'un joueur enverra en cas de crash | `docs/crash_report_user_pipeline.md` |

## Les variables d'environnement

| variable | effet |
|---|---|
| `PARTYBOARD_AUDIO_DIAGNOSTICS` | `1` trace banques et violations, `2` ajoute chaque voix |
| `PARTYBOARD_FORCE_ROLLBACK` | `<période>[:<distances>]` arme le probe de rollback local |
| `PARTYBOARD_MEM_DIAGNOSTICS` | `1` arme la vérification d'intégrité des blocs HuMem |
| `PARTYBOARD_CRASH_DIR` | dossier des rapports ; sa présence signifie « session supervisée » |
| `PARTYBOARD_CRASH_QUEUE` | remplace la file locale des incidents, pour les tests |

La campagne écrit les deux premières **explicitement** dans l'environnement des
pairs plutôt que de les hériter : une variable oubliée dans un shell ne doit pas
pouvoir changer en silence ce qu'une campagne a mesuré.
