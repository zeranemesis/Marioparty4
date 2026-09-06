# Salon en ligne : démarrage réservé à l’hôte

Le compagnon affiche désormais un pseudo par joueur, les rôles, la comparaison du disque et le ping aller-retour mesuré toutes les deux secondes sur la connexion TLS. Le salon reste limité à deux joueurs.

Seul l’hôte peut demander le chargement et donner le départ. Cette règle est vérifiée dans le protocole, en plus du bouton désactivé chez l’invité. Les commandes prématurées, périmées ou envoyées par le mauvais rôle sont refusées. Le jeu sélectionné démarre automatiquement sur les deux machines ; une barrière native attend leur initialisation avant le premier tick de simulation. Quitter pendant le chargement annule le départ.

Chaque utilisateur choisit son fichier avant de créer ou rejoindre le salon. Le lecteur natif vérifie Mario Party 4 USA Rev 1, puis le compagnon calcule le SHA-256 de tout le fichier et compare également sa taille. Un verrou de lecture Windows interdit sa modification ou son remplacement pendant la session. Le jeu utilise exactement ce chemin, sans deuxième sélection. Les chemins locaux ne sont pas transmis au joueur distant.

La comparaison porte sur les octets du fichier, pas sur une image décompressée : deux noms différents sont acceptés, mais ISO et RVZ, ou deux compressions différentes, sont refusés. Les sauvegardes et l’état initial ne sont pas encore intégralement comparés.

Validation : compilation native et .NET réussie ; 79 contrôles locaux réussis, dont deux vrais exécutables en mode sonde traversant la barrière native puis 1 200 ticks via TLS. Contrôles négatifs : invité tentant de lancer, départ prématuré, disque modifié d’un octet, taille différente, fichier non compatible, mauvais certificat/secret/version. Annulation pendant le chargement : le processus de test sort avec le code attendu. Le RVZ configuré sur ce PC passe aussi la vérification native et le calcul complet. Interface inspectée par rendu WinForms avec joueurs et ping fictifs clairement indiqués.

Ces tests ne constituent pas une partie graphique complète entre deux PC et deux réseaux. Le message « No compatible peer after 30 seconds » de la capture ne suffit pas à établir sa cause. Le nouveau départ coordonné évite de commencer la simulation pendant que l’autre PC attend encore une action de lancement. L’accès réel à travers les box reste à valider ; aucun pare-feu ou routeur n’a été modifié par les tests.

Implémentation : tools/online/Lobby.cs, LobbyForm.cs, Connection.cs, Program.cs, Tests.cs ; src/port/iso_validate.cpp, entry.cpp, portmain.cpp, src/game/main.c, include/port/netplay_runtime.h et dol.def.
