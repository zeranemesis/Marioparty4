# Rapports de crash côté joueur — format, file locale, consentement

Ce document fixe le format et les règles. Il ne décrit pas un backend, et rien
dans la validation du jeu ne doit attendre qu'un backend existe.

Le principe de bout en bout :

```
PartyBoard         plante
                   -> écrit un rapport texte + un manifeste JSON
                   -> les met dans une file locale
prochain lancement -> voit la file, demande au joueur
uploader séparé    -> envoie ce que le joueur a accepté
backend            -> reçoit, dédoublonne
GitHub App         -> ouvre l'issue, ou ajoute une occurrence
```

Trois frontières sont volontaires :

- **L'uploader est un programme distinct.** `partyboard.exe` n'envoie rien.
- **Aucun secret dans le jeu.** Pas de token GitHub, pas de PAT, pas de clé
  d'API, ni dans le binaire ni dans un fichier à côté. Le jeu écrit des
  fichiers ; c'est tout ce qu'il sait faire.
- **Rien ne part sans un oui.** Le consentement est par envoi, pas une case
  cochée une fois pour toutes.

---

## 1. Ce que le joueur voit

Un joueur ne doit jamais avoir à chercher un fichier journal. Au lancement
suivant un crash :

> PartyBoard s'est fermé de façon inattendue pendant votre dernière partie
> (plateau w04, tour 3, mini-jeu m416).
> Envoyer un rapport aide à corriger ce problème.
> [ Voir ce qui sera envoyé ]  [ Envoyer ]  [ Ne pas envoyer ]  [ Ne plus demander ]

« Voir ce qui sera envoyé » ouvre le rapport texte tel quel. Rien n'est envoyé
avant que le joueur ait cliqué sur Envoyer, et un joueur qui refuse n'est pas
redemandé pour le même incident.

Champ facultatif, une seule ligne : **« Que faisiez-vous au moment du crash ? »**
Il est vide par défaut et n'est jamais pré-rempli.

---

## 2. Le manifeste

Un fichier JSON par incident, à côté du rapport texte. C'est ce que l'uploader
lit ; il n'a jamais à interpréter le rapport.

```json
{
  "manifest_version": 1,
  "fingerprint": "AUDIO_UAF:overlay84:ensureADPCMBlockDecoded@hw_pc.c:711",
  "defect_class": "AUDIO_UAF",
  "occurred_at": "2026-09-11T12:53:30Z",
  "partyboard_version": "0.15.6",
  "build": {
    "revision": "a985b4fca3813f7894a97a005ecbad12fb3fd4ed",
    "branch": "audio-local",
    "type": "RelWithDebInfo",
    "stamp": "Sep 11 2026 11:02:39",
    "arch": "x86_64"
  },
  "os": { "name": "Windows", "version": "10.0.26200", "locale": "fr-FR" },
  "exception": {
    "code": "0xC0000005",
    "name": "EXCEPTION_ACCESS_VIOLATION",
    "operation": "read",
    "module": "dol.dll",
    "module_offset": "0x4e5d14",
    "symbol": "ensureADPCMBlockDecoded",
    "source": "hw_pc.c:711"
  },
  "game": {
    "board": "w04Dll",
    "overlay": 84,
    "minigame": -1,
    "turn": 3,
    "frame": 27744,
    "owning_process": "omWatchOverlayProc"
  },
  "netplay": {
    "enabled": true,
    "role": "host",
    "peers": 2,
    "mismatch": 0,
    "rng_sync": true,
    "repaired": 0,
    "send_errors": 0
  },
  "events": [
    "OVERLAY overlay 84 unloaded at frame 27744",
    "AUDIO sample 1191 of bank 19 freed while voice 12 still reads it"
  ],
  "attachments": [
    { "kind": "report", "file": "crash-report-2026-09-11_130114.txt", "bytes": 18422 },
    { "kind": "minidump", "file": "crash-2026-09-11_130114.dmp", "bytes": 100352,
      "sensitivity": "high" }
  ],
  "user_note": "",
  "consent": { "report": false, "minidump": false, "asked": false }
}
```

Deux règles sur ce document :

- **`consent` est faux tant que le joueur n'a pas répondu**, et le rapport et le
  minidump ont chacun leur propre consentement. Un joueur peut vouloir envoyer
  le texte sans le dump.
- **Le nom de fichier d'une pièce jointe est relatif au dossier de l'incident.**
  Aucun chemin absolu n'apparaît dans un manifeste.

---

## 3. Le fingerprint

Le même que celui de la campagne interne, produit par le même calcul, pour que
les deux mondes parlent des mêmes bugs.

Il se construit **uniquement** à partir de :

- le code d'exception et la classe de défaut,
- le module et son offset relatif,
- le symbole et le fichier source avec sa ligne, quand ils sont résolus,
- l'overlay et le contexte de jeu,
- le processus HuPrc propriétaire, pour un débordement de pile,
- les frames de pile fiables.

Il ne contient **jamais** un identifiant de processus, une adresse absolue
soumise à l'ASLR, un horodatage ni une valeur de handle. Deux joueurs qui
rencontrent le même défaut sur deux machines différentes doivent produire la
même chaîne, sinon la déduplication ne sert à rien.

Forme : `<CLASSE>:<contexte>:<site>`, par exemple

```
STACK_OVERFLOW:overlay92:fn_1_30A4
AUDIO_UAF:overlay84:ensureADPCMBlockDecoded@hw_pc.c:711
```

Le fichier source est préféré à l'offset partout où il est connu : un offset
change à chaque compilation, et une signature qui change à chaque compilation ne
regroupe rien.

---

## 4. Confidentialité

Ne sont **jamais** collectés automatiquement :

- le nom d'utilisateur Windows,
- un chemin personnel non nettoyé,
- l'adresse IP publique,
- un contenu arbitraire de la mémoire,
- les sauvegardes du joueur,
- quoi que ce soit de personnel qui ne serve pas au diagnostic.

**Nettoyage des chemins.** Avant écriture du manifeste, tout chemin est
réécrit :

| avant | après |
|---|---|
| `C:\Users\Valentin\Documents\...` | `C:\Users\<user>\Documents\...` |
| `C:\Users\Valentin\AppData\...` | `%LOCALAPPDATA%\...` |
| le dossier d'installation du jeu | `<install>\...` |

Le nettoyage s'applique au manifeste **et** au rapport texte, y compris à la
ligne de commande, qui contient le chemin de l'ISO.

**Le minidump est plus sensible que le rapport.** Il contient les piles de tous
les threads et la mémoire qu'elles référencent. Il a son propre consentement,
il est présenté comme tel au joueur, et il n'est jamais envoyé par défaut.

---

## 5. La file locale

```
%LOCALAPPDATA%\PartyBoard\crashes\
    <fingerprint-hash>-<compteur>\
        manifest.json
        crash-report-....txt
        crash-....dmp          (éventuellement)
    queue.json
```

`queue.json` liste les incidents en attente avec leur état :

```
PENDING     écrit, le joueur n'a pas encore été interrogé
DECLINED    le joueur a refusé ; conservé localement, jamais renvoyé
READY       le joueur a accepté ; l'uploader peut l'envoyer
SENT        envoyé, avec l'identifiant rendu par le backend
FAILED      envoi tenté et échoué ; réessayable
```

Bornes, pour qu'une boucle de crash ne remplisse pas le disque : au plus
**20 incidents** et **200 Mo** conservés, les plus anciens `SENT` puis
`DECLINED` étant retirés en premier. Un incident `READY` n'est jamais supprimé
pour faire de la place.

---

## 6. Déduplication

Cent joueurs qui rencontrent le même défaut doivent produire **un bug avec cent
occurrences**, pas cent issues.

- Fingerprint déjà connu du backend → une occurrence de plus sur l'issue
  existante : compteur, versions touchées, plateaux touchés.
- Fingerprint inconnu → nouvelle issue.
- Localement aussi : un même fingerprint rencontré plusieurs fois par le même
  joueur n'écrit qu'un dossier, avec un compteur d'occurrences et la date de la
  dernière.

---

## 7. Ce qui est à faire, et dans quel ordre

| étape | statut |
|---|---|
| format du manifeste | ce document |
| fingerprint partagé avec la campagne | **fait**, `tools/netplay_session.ps1` |
| nettoyage des chemins | à faire |
| écriture du manifeste par le jeu | à faire |
| file locale et ses bornes | à faire |
| interface de consentement au lancement | à faire |
| abstraction d'uploader (une interface, zéro réseau) | à faire |
| backend et GitHub App | hors périmètre pour l'instant |

Rien de tout cela ne doit retarder la validation du jeu.

---

## État réel au 2026-09-16 — ce que ce document décrivait de travers

Le tableau du §7 laissait entendre que le jeu n'écrit encore rien. **C'est
faux, et cette erreur a directement coûté du temps sur
[l'issue #3](https://github.com/zeranemesis/Marioparty4/issues/3)** : un
testeur a rapporté deux crashes qu'il ne pouvait pas reproduire, alors que les
preuves étaient déjà sur son disque et que personne ne savait où regarder.

### Ce qui fonctionne aujourd'hui

`entry.cpp:30` appelle `PartyBoard_CrashReportInit(nullptr, -1, nullptr)` à
**chaque** lancement, en ligne comme hors ligne. Le gestionnaire est donc armé
en permanence (`AddVectoredExceptionHandler` + `SetUnhandledExceptionFilter`),
et `writeCrashArtifacts()` produit **trois fichiers** :

```
crash-report-pid-<PID>-<horodatage>.txt     identité, exception, simulation,
                                            piles de coroutines, trace, fil d'événements
crash-pid-<PID>-<horodatage>.dmp            minidump (si le contexte existe)
stack-usage-pid-<PID>-<horodatage>.txt      consommation de pile par coroutine
```

Le rapport texte est **écrit deux fois** : une version partielle avant la
`StackWalk64`, puis la version complète si la marche des piles survit. Le
faulting address, l'offset de module et la frame ne sont donc jamais perdus,
même si le reste échoue. C'est déjà largement exploitable.

### Où ils atterrissent réellement

**Pas** dans `%LOCALAPPDATA%\PartyBoard\crashes\`. `buildPath()` ne préfixe un
dossier que si `gIdentity.sessionDir` est renseigné, ce qui n'arrive qu'avec
`PARTYBOARD_CRASH_DIR`, `PARTYBOARD_NET_DIAGNOSTIC`, ou un `sessionDir` passé
par le lanceur. Un joueur ordinaire n'a aucun des trois : le chemin est alors un
**nom de fichier nu**, donc relatif au **répertoire courant du processus** —
à côté de `partyboard.exe`, ou là où le lanceur l'a placé.

C'est la ligne la plus importante de cette page pour quiconque demande des
preuves à un joueur.

### Ce qui existe mais n'est branché sur rien

`crash_manifest.cpp` implémente **entièrement** le manifeste, le nettoyage des
chemins (`PartyBoard_CrashSanitizeText`), le fingerprint, la file et ses bornes.
`crash_uploader.cpp` implémente l'abstraction d'uploader.

Mais `PartyBoard_CrashWriteManifest()` et `PartyBoard_CrashQueueAdd()` **ne sont
appelés que depuis l'auto-test** de `crash_uploader.cpp`. `writeCrashArtifacts()`
ne les appelle pas. Conséquences, toutes vérifiables :

- aucun `manifest.json` n'est jamais écrit par le jeu ;
- `%LOCALAPPDATA%\PartyBoard\crashes\` reste **vide** — le dossier n'est créé
  que par le code de file, que rien n'appelle ;
- **le nettoyage des chemins ne s'applique donc à rien.** Le rapport texte
  contient la ligne de commande telle quelle, chemin de l'ISO compris, et le
  chemin de l'exécutable. Ce n'est pas un problème tant que le joueur envoie le
  fichier lui-même en connaissance de cause, mais cela doit être dit avant de
  lui demander : **la promesse d'anonymisation du §4 n'est pas tenue
  aujourd'hui.**

### Tableau du §7, corrigé

| étape | statut réel |
|---|---|
| format du manifeste | ce document |
| fingerprint partagé avec la campagne | fait, `tools/netplay_session.ps1` |
| écriture du rapport texte et du minidump par le jeu | **fait**, non documenté jusqu'ici |
| nettoyage des chemins | **code écrit, jamais exécuté** |
| écriture du manifeste par le jeu | **code écrit, non branché** |
| file locale et ses bornes | **code écrit, non branché** |
| interface de consentement au lancement | à faire — rien n'existe |
| abstraction d'uploader | **code écrit**, aucun transport |
| backend et GitHub App | hors périmètre |

### Ce qu'il faut demander à un joueur, en attendant

Le `.txt` seul suffit à nommer un défaut : code d'exception, module et offset,
symbole, overlay, frame de simulation, et le fil d'événements qui précède. Le
`.dmp` contient les piles de tous les threads et la mémoire qu'elles
référencent — il ne doit être demandé qu'explicitement, et jamais par défaut.

### Où regarder quand le rapport de l'issue #3 arrivera

Deux choses ont été vérifiées en cherchant la cause à l'aveugle, et méritent
d'être notées pour ne pas être refaites :

**Ce n'est pas le cycle de vie des séquences de fin.** `MGSeqMain()` contient
une condition inversée — `if (!work->data) { HuMemDirectFree(work->data); }`,
qui ne libère que lorsque le pointeur est déjà nul. C'est une **fuite bornée**,
pas un plantage : `HuMemMemoryFree()` rejette `NULL` d'entrée de jeu, et
`CreateSeq()` libère la donnée périmée à la réutilisation du créneau. Surtout,
`game/minigame_seq.c` est un objet **`Matching`** dans `configure.py` : ce
comportement est celui du jeu d'origine, et le corriger casserait le build de
correspondance. **À ne pas « réparer ».**

**Le premier endroit à regarder est le drain audio.** `HuAudSndGrpWait()`
(`game/audio.c`) attend hors ligne que `msmMusGetNumPlay` et `msmSeGetNumPlay`
retombent à zéro, mais **abandonne au bout de 500 ms** (`SNDGRP_TIMEOUT`) en se
contentant d'un `OSReport("Timed Out! …")` — puis `HuAudSndGrpSetSet()` charge
le nouveau jeu de banques, ce qui fait `msmSysDelGroupAll()` et libère les
échantillons. C'est **mot pour mot** la classe de défaut que le §3 de ce
document donne en exemple :

    AUDIO_UAF:overlay84:ensureADPCMBlockDecoded@hw_pc.c:711
    "AUDIO sample 1191 of bank 19 freed while voice 12 still reads it"

Les deux mini-jeux incriminés lancent une fanfare juste avant de rendre la main,
et le changement d'overlay recharge les banques dans la foulée. Ce n'est **pas**
une preuve : rien ne dit que le drain expire, et `HuAudFadeOut()` peut très bien
avoir tout arrêté à temps. Mais si le rapport dit `EXCEPTION_ACCESS_VIOLATION`
dans le code MusyX, c'est là qu'il faut commencer, et la ligne `Timed Out!` du
fil d'événements le confirmera ou l'infirmera immédiatement.
