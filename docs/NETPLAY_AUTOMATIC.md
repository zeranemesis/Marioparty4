# Connexion automatique PartyBoard — 4 septembre 2026

Un compagnon Windows `PartyBoardOnline.exe` permet de créer une partie et partager une invitation, ou de rejoindre par collage de cette invitation. Le menu natif propose « Jouer en ligne » ; un lanceur `Jouer en ligne.cmd` est également livré. La version est installée dans `C:\Users\valen\Videos\bon plan`. L’archive portable contient les mêmes binaires, sans disque de jeu ni sauvegarde.

## Comportement

- Création : détection de la route physique, préparation de l’autorisation Windows après son dialogue UAC, tentative PCP puis NAT-PMP puis UPnP pour un accès TCP temporaire. Aucun port ni adresse à saisir.
- Invitation : coordonnées de connexion, empreinte SHA-256 du certificat TLS, secret aléatoire de 256 bits, empreinte des exécutables/DLL et expiration à 30 minutes. Elle doit être partagée seulement avec l’invité. Aucun secret n’est placé dans les arguments du jeu ou les journaux.
- Connexion : TLS 1.2 fourni par Windows/.NET, certificat de session épinglé, contrôle du secret et des fichiers, un seul invité. Les erreurs d’invitation et de version sont affichées en français.
- Le jeu communique avec le compagnon seulement en UDP sur 127.0.0.1 (`--netplay-loopback`). Le protocole UDP expérimental n’est pas exposé par ce parcours. Le compagnon transporte les messages dans TLS/TCP, avec heartbeat et messages bornés.
- Netplay natif : cadence imposée à 60 FPS, turbo neutralisé, entrées en arrière-plan et absence de pause sur perte de focus au démarrage, sans réécrire les préférences hors ligne.
- Fermeture : demande de suppression du bail de box, qui possède aussi une expiration finie. Un processus Windows distinct retire la règle du pare-feu à la fermeture ou à la mort du lanceur. Une coupure complète du PC peut empêcher sa suppression ; la règle reste alors limitée à cet exécutable et au port de session.
- Aucun serveur externe, relais, désactivation de VPN, désactivation de pare-feu ou repli vers un bail permanent.

## Validation effectuée

Compilation native et compagnon réussies. Tests du compagnon depuis le dossier installé : trois essais successifs réussis, chacun avec 47 contrôles et 1 200 ticks des vrais exécutables à travers le canal TLS. Couverture : invitations invalides/expirées, adresses et URLs refusées, parsers PCP/NAT-PMP, création/renouvellement/suppression de baux simulés, refus de bail excessif, empreinte TLS incorrecte, mauvais secret, versions incompatibles et échanges PAD natifs avec changements de contexte.

Un premier essai installé a échoué sans capture des sorties enfants. Les sorties ont ensuite été ajoutées, et l’envoi du canal a été regroupé en un seul message TLS par paquet pour éviter les petits enregistrements d’en-tête. Les trois essais finaux passent en environ 10 secondes chacun. La cause exacte du premier échec n’a pas été établie ; ces répétitions ne remplacent pas un test prolongé en réseau réel.

L’interface WinForms a été inspectée via son rendu intégré. Le parcours interactif complet avec UAC n’a pas été exécuté. Les tests chiffrés ont nécessité les services cryptographiques du compte Windows réel ; le profil isolé ne pouvait pas importer la clé temporaire du certificat.

## Limites réelles

Le diagnostic actuel détecte une route VPN/virtuelle et refuse donc l’hébergement automatique sur ce PC. L’utilisateur peut mettre son VPN en pause, ou l’autre PC peut créer la partie. Aucun paramètre VPN n’a été changé.

La création, le renouvellement et la suppression sur une vraie box, ainsi que l’autorisation réelle du pare-feu, restent à valider. Aucun port n’a été ouvert sur la box pendant le développement. Sans relais, une box qui refuse les protocoles, un NAT amont ou un réseau opérateur incompatible peut empêcher la connexion directe ; l’application propose alors d’inverser les rôles.

Il reste deux joueurs et du lockstep, sans rollback complet. TCP peut augmenter l’attente en cas de perte réseau. Les fichiers du programme et le fichier disque complet sont comparés ; la sauvegarde et l’état initial ne le sont pas encore intégralement. Une partie réelle complète entre deux réseaux reste à valider.

## Fichiers de développement

`tools/online/Connection.cs`, `Gateway.cs`, `Program.cs`, `Tests.cs`, `PartyBoardOnline.exe.config` ; `tools/build_online.ps1` appelé par `tools/build_local.ps1` pour la compilation par défaut. Intégration native dans `netplay_transport`, `netplay_runtime`, `ui/prelaunch.cpp`, `portmain.cpp`, `imgui.cpp` et `game/main.c`.

## Références techniques

- [TLS avec .NET Framework](https://learn.microsoft.com/en-us/dotnet/framework/network-programming/tls).
- [NAT-PMP, RFC 6886](https://www.rfc-editor.org/info/rfc6886/).
- [PCP, RFC 6887](https://www.rfc-editor.org/rfc/rfc6887.html).
- [UPnP WANIPConnection:2](https://upnp.org/specs/gw/UPnP-gw-WANIPConnection-v2-Service.pdf).
- [Règles Windows INetFwRule](https://learn.microsoft.com/en-us/windows/win32/api/netfw/nn-netfw-inetfwrule).

## Mise à jour du salon

Voir NETPLAY_HOST_LOBBY.md : pseudos, ping mesuré, SHA-256 complet du disque et départ coordonné réservé à l’hôte. La suite actuelle passe 79 contrôles et 1 200 ticks natifs via TLS.

