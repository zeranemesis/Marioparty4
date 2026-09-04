# Salons 1–4 joueurs — état de développement

## Ce qui est implémenté

`include/port/netplay_lobby.hpp` contient une machine à états indépendante,
destinée à l'hôte ou au service de salons. Elle ne remplace pas encore le netplay.

- L'hôte occupe la place 1 ; les nouvelles connexions prennent la première place libre.
- Un départ ne renumérote pas les joueurs restants.
- L'hôte peut déplacer un invité vers une place libre, avant le démarrage.
- Une modification des places annule les validations « prêt » précédentes.
- Les identités exactes du protocole, du build et du jeu doivent correspondre.
- Le démarrage nécessite tous les joueurs prêts, puis un accusé de préparation
  de chacun pour la tentative actuelle. Les anciens accusés sont refusés.
- Le départ d'un invité pendant la préparation annule le démarrage.
- Le départ de l'hôte, ou d'un participant pendant la partie, ferme la session.
  La migration d'hôte et la reconnexion en cours de partie ne sont pas implémentées.
- La vérification d'appartenance d'une entrée distingue l'identité de connexion
  de la place annoncée ; un participant ne peut pas s'attribuer une autre place.
- Le routage distingue le port de manette physique local du numéro de joueur
  dans la partie : la première manette d'un invité peut être affectée au joueur 4.

Les identités de connexion sont des valeurs **fournies par un transport déjà
authentifié**, jamais des identifiants déclarés librement par un client. Elles
doivent être liées au salon courant et renouvelées après une reconnexion.
Cette bibliothèque ne vérifie aucun mot de passe et n'authentifie aucun paquet.

## Vérification réalisée

Depuis la racine du projet :

```powershell
./tools/test_netplay_lobby.ps1
```

Compilation C++20 MSVC avec avertissements traités comme erreurs (`/W4 /WX`).
Résultat du 4 septembre 2026 : **269 assertions réussies**, couvrant notamment
les effectifs de 1 à 4, compatibilités incorrectes, salon plein, autorisations,
changements de places, accusés périmés et déconnexions.

Il s'agit de tests unitaires, sans socket, sans launcher et sans moteur de jeu.
Aucun exécutable installé n'a été remplacé pour cette étape.

## Ce qui reste à intégrer avant un mode en ligne utilisable

1. Construire le transport direct et l'invitation contenant les coordonnées
   de l'hôte : aucun service de résolution des salons ni relais externe ne
   sera utilisé. Un code court universel n'est pas prévu dans cette architecture.
2. Ajouter l'authentification du salon, avec connexion chiffrée, limitation des
   tentatives, expiration des sessions et secrets absents des journaux et
   arguments de lancement. Ne pas réutiliser le simple hash de contrôle UDP
   existant comme preuve d'identité ou protection par mot de passe.
3. Intégrer créer/rejoindre/quitter, identifiant, saisie masquée du mot de passe,
   quatre places et état prêt dans le launcher. Afficher les erreurs et délais
   d'attente sans bloquer son interface.
4. Remplacer la logique de simulation à deux historiques et un pair par les
   entrées des 1–4 places réellement occupées. Le composant de salon ne fait
   pas ce changement à lui seul.
5. Relier le routage aux contrôleurs virtuels du jeu, neutraliser les places
   inoccupées et configurer explicitement joueurs humains/CPU et vibrations.
6. Synchroniser l'état initial et le lancement effectif sur tous les clients.
   L'état local `Running` n'est pas à lui seul une barrière réseau distribuée.
7. Tester deux, puis quatre processus et plusieurs PC : lancement, menus,
   plateau, mini-jeux, latence, pertes réseau, fermeture et incompatibilités.

Le netplay actuel reste le mode expérimental à deux joueurs en lockstep.
Cette étape n'ajoute pas de rollback en jeu et ne valide pas un jeu en ligne
complet. Les corrections audio existantes ne sont pas modifiées.

## Orientation retenue : connexion directe, aucun serveur externe

L'utilisateur a écarté Epic, Photon et tout serveur externe. Le PC créateur
du salon hébergera directement la partie dans PartyBoard. Aucun compte chez
un fournisseur de salons ni SDK de service hébergé n'est désormais requis.
Les pistes précédentes de service hébergé sont abandonnées, sans intégration
ni création de compte effectuée.

L'objectif est une invitation à copier-coller contenant les coordonnées de
connexion et l'identité cryptographique attendue, sans IP à saisir. Elle ne
cache pas l'IP aux participants. Le transport chiffré et l'authentification
restent à implémenter avant d'exposer le runtime expérimental à Internet.

Une demande **optionnelle et consentie** de redirection temporaire à la box
est envisagée via PCP/NAT-PMP ou UPnP. Elle évite une configuration manuelle,
pas la création d'un accès entrant. Pas de désactivation de pare-feu, de DMZ,
de modification automatique du VPN ou d'ouverture permanente en secours.
Le refus de la box, un NAT amont ou le réseau de l'opérateur peuvent empêcher
le jeu direct ; sans relais, aucune compatibilité universelle n'est promise.

### Diagnostic en lecture seule ajouté

```powershell
./tools/test_direct_connection.ps1 -SelfTest
./tools/test_direct_connection.ps1
```

Le script Windows examine d'abord les routes actives. Il s'arrête lorsque la
route sélectionnée est virtuelle/VPN, ou que le choix est ambigu. Il ne change
aucun réglage. Sans ce blocage, il envoie uniquement des requêtes de découverte
PCP ANNOUNCE, NAT-PMP d'adresse externe et SSDP UPnP sur le réseau local.
Aucune requête MAP/AddPortMapping n'est implémentée dans ce diagnostic.
Il ne suit pas les URLs LOCATION annoncées par SSDP et ne contacte pas de
serveur public de découverte d'adresse. L'adresse WAN éventuelle n'est pas
affichée. Une annonce de compatibilité n'est pas une preuve d'accès Internet.

Validation : **15 contrôles de parseurs réussis**, sans accès réseau.
Si le diagnostic retourne `Stopped:VpnOrVirtualRoute`, une route VPN ou virtuelle
empêche le test automatique de la box. Aucune ouverture de port ni modification
du VPN n'est alors effectuée. L'utilisateur doit choisir lui-même de désactiver
temporairement son VPN pour reprendre le test de sa connexion directe. Ce statut
ne permet pas de conclure que la box ou l'opérateur sont incompatibles, ni de
diagnostiquer un CGNAT. Les détails du réseau de la machine de développement
ne sont pas inclus dans cet instantané public.

L'ouverture automatique et son bouton dans le launcher ne sont **pas encore
implémentés**. La prochaine étape est de vérifier quel protocole la box annonce,
puis de tester un bail temporaire borné sur un port de diagnostic, sans exposer
le netplay non authentifié existant. Les délais, renouvellements, suppressions
et expirations devront être vérifiés avant d'intégrer cela au cycle du salon.

Références : [PCP, RFC 6887](https://www.rfc-editor.org/rfc/rfc6887.html),
[NAT-PMP, RFC 6886](https://www.rfc-editor.org/rfc/rfc6886.html).

## Renforcement du noyau de rollback

La suite `tools/test_rollback_network.ps1` compile le véritable `rollback.cpp`
et libco, avec uniquement le journal OSReport remplacé par un stub.

Régression reproduite avant correction : **8 échecs**. Le noyau acceptait une
modification d'entrée déjà confirmée, des numéros de tick arbitrairement futurs
pouvant écraser l'historique, et dépassait sa fenêtre de prédiction sans attendre
les confirmations. Une arrivée tardive devenait alors impossible à corriger.

Corrections :

- Entrées confirmées immuables ; une répétition identique reste acceptée.
- Fenêtres passée et future bornées sans addition susceptible de déborder.
- Arrêt temporaire de la progression à la limite de prédiction, sans rendre la
  session invalide ; reprise possible après réception des entrées attendues.
- Refus de laisser le compteur de ticks boucler sur zéro.

Résultat après correction : **49 220 assertions, zéro échec**. Le scénario
principal utilise quatre sessions pendant 1 200 ticks, avec retards distincts,
réordonnancement, doublons et livraisons différées simulant des retransmissions.
Après réception des entrées restantes, les quatre états finaux correspondent
à une simulation de référence sans retard. Les snapshots de mémoire et de
coroutines sont également testés.

La compilation complète `RelWithDebInfo` réussit. L'exécutable compilé passe
`--netplay-self-test` : noyau rollback, snapshots mémoire/coroutines, boucle UDP
locale et tests du runtime. Aucun binaire installé chez l'utilisateur n'a été
remplacé lors de cette étape.

**Limite essentielle :** les quatre sessions simulent un état de test, pas
Mario Party 4. Aucun test Internet n'a été effectué. Le runtime du jeu utilise
toujours le lockstep à deux joueurs ; ne pas activer la resimulation réelle
avant de couvrir tout l'état du jeu et d'intégrer les effets irréversibles
(audio, vibrations, sauvegardes et succès).
