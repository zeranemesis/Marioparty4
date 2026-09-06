# Netplay protocole 4 — chronologie continue (4 septembre 2026)

Le mode --netplay-full conserve désormais un compteur de ticks, les historiques d’entrées différées et un accord initial RNG pour toute la session. Un changement de module ne remet plus le réseau à zéro et ne permet plus de tick local de rattrapage. Le mode mini-jeux seul conserve son fonctionnement par contexte.

Les paquets v4 font 52 octets et transportent le contexte lors de la capture de l’entrée. Les retransmissions gardent le contexte original. À consommation d’une paire d’entrées, les deux contextes du même tick de capture sont comparés. Une divergence arrête la simulation ; ce contrôle ne constitue pas un hash complet de l’état du jeu. Avec un délai D, la détection intervient D ticks après la capture divergente.

Le protocole est incompatible avec v3 : utiliser le même build et les mêmes DLL des deux côtés. L’identifiant de session reste constant ; authentification, vérification build/disque/sauvegarde et salons à quatre joueurs restent à intégrer.

## Validation à exécuter

- tools/build_local.ps1
- build/aexp/RelWithDebInfo/partyboard.exe --netplay-self-test
- tools/test_netplay_pad.ps1

La sonde PAD utilise les contextes 0 → 7 → 3 → 7 aux ticks 100, 200 et 400, avec une pause de 250 ms sur le client à 200. Elle vérifie les entrées attendues et un compteur réseau continu pour les délais 0, 3, 8. Deux scénarios injectent une divergence au tick 100 avec délais 0 et 3 ; les deux pairs doivent détecter le contexte divergent. Le scénario de déconnexion reste exécuté.

Il s’agit de deux processus avec entrées et contextes synthétiques, sans chargement graphique de Mario Party 4. Une partie réelle peut encore diverger à cause d’attentes asynchrones du moteur. Dans ce cas, il faut corriger la source du décalage plutôt que réintroduire des ticks non synchronisés. Deux PC et une partie menus → plateau → mini-jeu restent à valider. Aucun rollback complet du jeu n’est activé.

## Résultats de cette compilation

Compilation complète réussie. Les cinq autotests intégrés passent, y compris le codec v4 (contexte couvert par le checksum et rejet v3). Régression PAD : 1 161 contrôles réussis. Suite interprocessus : six PASS de 600 ticks, quatre PASS de divergence (deux pairs, délais 0/3), un PASS de perte de pair et arrêt terminal. Deux avertissements C4244 préexistants à la compilation du test PAD.

Le premier essai de divergence à délai zéro a révélé qu’un pair pouvait fermer avant d’envoyer son entrée. La version finale envoie cette entrée avant l’arrêt. La perte de ce dernier paquet reste prise en charge par le watchdog distant ; il ne s’agit pas d’un protocole fiable de déconnexion coordonnée.
