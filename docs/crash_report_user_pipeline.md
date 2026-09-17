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
