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

### 1bis. Cinq minutes — toute la suite

```bash
tools\run_all_tests.ps1 -DiscPath "<iso>"
```

Treize scripts `test_*.ps1` existent. `validate_netplay.ps1` en appelait trois ;
`test_audio_wait.ps1` et `test_rollback_effects.ps1` n'étaient référencés par
rien du tout. Un test que personne n'exécute est un commentaire qui prend du
temps à compiler.

La découverte se fait par glob, jamais depuis une liste tenue à l'intérieur du
lanceur : un nouveau test est pris en compte le jour où il est ajouté. Un script
que l'invocation ne peut pas faire tourner est rapporté `SKIPPED` avec sa
raison, jamais omis — « douze réussis » ne veut rien dire si on ignore que le
treizième n'a pas tourné.

Chaque script tourne dans **son propre processus**, et le code de sortie de ce
processus est le résultat. L'écriture évidente — `& $script; $LASTEXITCODE` —
est fausse, et fausse dans le sens qui cache les échecs : `$LASTEXITCODE` n'est
écrit que par une commande native ou un `exit` explicite, et neuf des treize
scripts se terminent simplement. Un tel script laisse ce que le **précédent** y
avait mis.

État au 2026-09-11 : **12 réussis, 0 échec, 1 non applicable, 317 s.** Le non
applicable est `test_direct_connection`, qui sort en 2 quand un VPN détient la
route prioritaire — il refuse alors de sonder la box ou d'ouvrir un port, ce qui
est le bon comportement et ne doit pas compter comme un échec.

La CI exécute cette suite entre les auto-tests du moteur et l'empaquetage.
`test_netplay_boot` en est exclu faute de disque sur un runner ;
`test_direct_connection` en est exclu **délibérément et définitivement**, parce
qu'il sonde le routeur et peut ouvrir un port, ce qui ne doit jamais arriver sans
surveillance sur une machine partagée.

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
sur **cinq** lignes, et les cinq doivent être vertes :

```
DETERMINISM: PASS
STABILITY: PASS
USER_TERMINATED: YES
PLAYABILITY: PASS
OVERALL: PASS
```

`PLAYABILITY` existe parce que les quatre autres peuvent toutes être vertes sur
une partie qu'aucun humain n'accepterait de terminer. Le modèle livré est le
lockstep : un tick n'avance que lorsque l'entrée distante est arrivée. En boucle
locale cette attente est nulle, et c'est pourquoi son absence est passée
inaperçue si longtemps. Les seuils sont dans `docs/playability_thresholds.md`,
fixés **avant** la première session à deux machines.

Une session réussie devient un enregistrement, l'enregistrement devient un
scénario, et le scénario rejoint la campagne. C'est ainsi que la couverture
grandit.

### 7. Humain, deux machines — la seule preuve qui compte

Aucune exécution en boucle locale ne peut établir que deux machines sur deux
réseaux restent synchronisées. Jusqu'ici l'outil ne savait pas l'enregistrer : il
codait `127.0.0.1` en dur et supervisait les deux processus lui-même.

**Machine A**, celle qui héberge :

```bash
tools\record_board_session.ps1 -DiscPath "<iso>" -Role Host
```

Elle affiche le port UDP et la ligne de commande à transmettre. Sur Internet,
c'est l'adresse publique de A qu'il faut, avec ce port UDP redirigé vers elle.

**Machine B**, celle qui rejoint :

```bash
tools\record_board_session.ps1 -DiscPath "<iso>" -Role Join -JoinAddress <A>:<port>
```

Chaque machine supervise **son** processus, classe **sa** terminaison, et écrit
son propre dossier de session avec un `session.json` lisible par machine. Aucune
des deux ne prétend savoir ce qui s'est passé sur l'autre : chacune conclut
`DETERMINISM: DEFERRED` et `OVERALL: PENDING_MERGE`, parce que la question « les
deux machines ont-elles calculé la même chose » exige les deux moitiés au même
endroit.

Ensuite, les deux dossiers réunis sur une seule machine :

```bash
tools\merge_session.ps1 -HostSession <dossier-A> -JoinSession <dossier-B> -ScenarioId <nom>
```

La fusion compare les deux enregistrements ligne par ligne et **nomme la première
ligne divergente**, compare les chemins d'overlays frame par frame, compare les
empreintes d'état finales quand les deux côtés se sont arrêtés à la même frame,
et refuse deux moitiés issues de commits différents avant toute autre
comparaison. Elle ne répare jamais, ne réconcilie jamais, ne désigne jamais un
gagnant. Sur un succès elle écrit l'enregistrement fusionné et, si on le demande,
un fragment de scénario marqué `coverage_source: HUMAN`.

`tools\test_merge_session.ps1` exerce cette fusion sur seize cas de fixtures,
parce que la manière naturelle de l'exercer coûte deux personnes, deux machines
et deux heures, et qu'y découvrir un défaut de l'outil gâcherait la soirée sans
rien produire.

### Ce qu'un run a réellement touché

Chaque mécanique de plateau se signale la première fois qu'elle s'exécute :

```
COVERAGE> SHOP first reached at frame 18422
```

La campagne et l'enregistreur relisent ces lignes et les inscrivent dans
`run.json` et `session.json`. Une ligne de la matrice ne passe de `UNTESTED` à
`PARTIAL` que si son marqueur apparaît dans un résultat. C'est ce qui empêche la
matrice d'être mise à jour de mémoire.

## Les enregistrements

```bash
tools\verify_replays.ps1
```

Les fichiers sont trop gros pour être versionnés ; leurs empreintes le sont.
Un enregistrement dont le `sha256` a changé est un autre enregistrement, et tout
résultat obtenu avec lui appartient à cet autre enregistrement.

**La campagne appelle cette vérification elle-même**, avant son premier run, et
refuse de démarrer si une empreinte ne correspond plus. La matrice promettait
cette vérification depuis qu'elle est écrite ; rien ne l'appelait.
`-SkipReplayVerification` la contourne, et l'inscrit dans chaque résultat de la
campagne : le contournement ne peut pas être silencieux.

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
| `PARTYBOARD_ROLLBACK_GATE_SURVEY` | `1` évalue la porte de capture **à chaque frame** et histogramme les séries de frames ouvertes. Mesure, pas politique : ne change ni quand on tente une capture ni ce qu'on accepte. Coûteux ; un run de relevé est un run de relevé |
| `PARTYBOARD_MEM_DIAGNOSTICS` | `1` arme la vérification d'intégrité des blocs HuMem. **Armée par défaut** par la campagne et par l'enregistreur de session : S1 est morte de `STATUS_HEAP_CORRUPTION` et aucun script ne la posait |
| `PARTYBOARD_CRASH_DIR` | dossier des rapports ; sa présence signifie « session supervisée » |
| `PARTYBOARD_CRASH_QUEUE` | remplace la file locale des incidents, pour les tests |

La campagne écrit les trois premières **explicitement** dans l'environnement des
pairs plutôt que de les hériter : une variable oubliée dans un shell ne doit pas
pouvoir changer en silence ce qu'une campagne a mesuré.

Et la règle symétrique, appliquée aux deux détecteurs : **un détecteur demandé
sans preuve qu'il a tourné vaut `HARNESS_FAILURE`, jamais `PASS`.** La preuve est
ce que le détecteur dit lui-même — les libérations de banque pour l'audio, les
enregistrements de tas pour la mémoire — et non le fait que la variable ait été
posée.
