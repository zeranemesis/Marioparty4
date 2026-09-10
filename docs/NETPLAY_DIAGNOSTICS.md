# Diagnostic de synchronisation

Chaque Session crée son propre dossier sous LocalAppData/PartyBoard/Diagnostics : session.txt et native.txt. Exporter diagnostic rassemble ces deux fichiers en texte, plafonné à 1 Mio par composant. Le dernier rapport reste accessible après réouverture du compagnon. Rien n'est envoyé automatiquement.

Le compagnon enregistre rôle, empreinte du build, identifiant de tentative partagé, chargement/prêt/départ/fermeture, phase, comparaison du disque, ping, compteurs locaux/distants. Le moteur enregistre toutes les deux secondes UTC, tick, contexte, disponibilité des entrées locales/distantes, dernier tick reçu, paquets acceptés/refusés, réparations, erreurs d'envoi, âge du dernier paquet et accord RNG. Un arrêt terminal ajoute un état immédiat. Chaque journal est borné à 3 000 lignes. Le journal natif est distinct de stdout/stderr : aucun chemin personnel, secret d'invitation, endpoint réseau ou pseudo n'est collecté.

Validation : 85 contrôles réussis. Deux exécutables natifs via TLS ont écrit les diagnostics pendant une pause de 12 secondes et une entrée perdue ; le compteur de réparation non nul et les logs des deux côtés sont vérifiés. Export combiné et limite de taille en lignes testés. Les vrais réseaux des utilisateurs restent à diagnostiquer : aucune cause du blocage au logo n'est encore démontrée.

## Rapport de désynchronisation par pair (v1)

Depuis la version 2 du hash canonique, chaque pair écrit son propre rapport
lorsque le flux d'états cesse de correspondre. Le fichier s'appelle
`netplay_desync_<AAAA-MM-JJ>_<HHMMSS>_player<N>.log` et est placé dans
`<dossier de préférences>/netplay`, ou à défaut dans le répertoire courant.

Il contient, dans cet ordre : identité du build (describe, révision, branche,
type), version du protocole et du hash, identité du disque, paramètres de
session (joueur, input delay, mode plein jeu, rollback), statistiques réseau,
première frame divergente et dernière frame connue bonne, contexte/overlay/
mini-jeu, état et compteurs de consommation RNG, un tableau `SUBSYSTEM` local
contre distant pour les seize sous-systèmes, le relevé champ par champ de la
frame fautive (`FIELD <index> <SOUS-SYSTÈME> <nom> <valeur>`), le flux de
condensats `DIGEST` sur 120 frames avant et 30 après, puis l'historique
`INPUT` local et distant sur la même fenêtre.

Aucune adresse mémoire, aucun handle système et aucun chemin personnel n'y
figure : seules des valeurs canoniques comparables entre deux machines.

## Comparaison automatique

```
python tools/netplay_compare.py peer1.log peer2.log
```

L'outil refuse de comparer deux pairs qui n'ont pas le même build, le même
protocole, le même disque ou les mêmes paramètres de session. Sinon il affiche
la première frame divergente, la dernière frame identique, les sous-systèmes
concernés, puis le premier champ divergent :

```
FIRST DESYNC: frame 87
previous matching frame: 86
category: PLAYERS
FIRST DIFFERENT FIELD
  index:     238
  category:  PLAYERS
  field:     p.coins
  peer A:    0x0000014b
  peer B:    0x0000014c
```

Il signale aussi une entrée qui n'a pas été appliquée à la même frame chez les
deux pairs, et compare les compteurs d'appels RNG.

Validation : cet exemple est la sortie réelle de `tools/test_netplay_pad.ps1`
avec `--netplay-probe-desync`, qui injecte `++GWPlayer[0].coins` à la frame 87
sur le client uniquement. Les deux pairs ont indépendamment identifié la frame
87 et le champ `p.coins`.
