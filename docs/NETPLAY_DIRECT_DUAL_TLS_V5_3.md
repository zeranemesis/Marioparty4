# Quatrième diagnostic : commande de préparation retenue, v5.3

Le fichier « Nouveau Document texte.txt » contenait seulement l’invitation PB2. Le vrai diagnostic client récent a ensuite été retrouvé sur le partage.

Les deux PC utilisent le même build 27C493…7B85, le même disque et établissent les deux canaux TLS avec un ping de 1–2 ms. À 18:29:38Z, l’hôte passe en préparation, lance son moteur et reçoit native_ready environ 630 ms plus tard. L’invité reste cependant en phase Waiting jusqu’à la fermeture : aucune ligne loading/native_ready, aucun processus natif et aucun paquet de jeu. L’hôte ne reçoit donc jamais sa confirmation et reste volontairement avant le premier tick. L’écran noir est la fenêtre native initialisée mais maintenue derrière la barrière de départ. Elle est fermée après environ 17 secondes, ce qui ferme ensuite le salon client.

La commande de préparation n’a pas quitté la file d’écriture introduite avec v5.1. La v5.2 ayant déjà séparé physiquement les deux sens TLS, cette file n’apporte plus de protection. La v5.3 la supprime. Tous les producteurs utilisent immédiatement le canal TLS sortant dédié sous le même verrou d’écriture ; la lecture se fait sur l’autre connexion. La commande de salon ne peut donc plus rester dans une tâche de file inactive, et une écriture ne partage toujours aucun SslStream avec la lecture.

Validation : 86 contrôles réussis. L’échange chiffré vérifie explicitement le profil, la commande host-only, le passage de l’invité en préparation, les deux signaux natifs READY, l’absence de simulation avant les deux confirmations, le départ commun et 1 200 ticks avec ping, pause de 12 secondes et retransmission. Une tentative réelle reste requise.
