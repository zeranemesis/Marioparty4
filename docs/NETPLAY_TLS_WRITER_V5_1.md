# Deuxième diagnostic : interblocage des écritures TLS

La correction v5 du pont UDP fonctionne. Les deux processus utilisent le même build 208297…08E2, la même tentative et le disque est identique. Les entrées circulent dans les deux sens : l’hôte atteint le tick 2213 et le client 2215, avec un dernier paquet âgé de 16–17 ms pendant environ 37 secondes. Les graines et contextes correspondent, aucune erreur d’envoi ou configuration incompatible n’est signalée.

Vers 18:10:09Z, les compteurs TLS entrants se figent des deux côtés (4 442 chez l’hôte, 4 419 chez le client), tandis que les jeux continuent à fournir des paquets locaux jusqu’à environ 5 200. Les deux moteurs attendent leur entrée courante, ticks 2213 et 2215. L’hôte enregistre ensuite une SocketException après la fermeture du transport.

La cause se trouve dans l’ordonnancement des écritures SslStream : le trafic du jeu, le timer de heartbeat et la réponse au ping partageaient une écriture verrouillée. La tâche de lecture répondait elle-même au ping ; si cette réponse attendait une écriture bloquée, elle cessait de lire. Quand la situation se produisait sur les deux pairs, aucune lecture ne pouvait libérer les écritures opposées.

V5.1 ajoute une file sortante et une seule tâche propriétaire des écritures TLS. Le pont UDP, le heartbeat, le salon et le lecteur TLS produisent des messages sans effectuer eux-mêmes l’écriture. La tâche de lecture peut donc continuer à vider le flux pendant qu’une écriture attend. La sérialisation conserve exactement une trame TLS par message et l’ordre du protocole.

Validation : compilation réussie, 85 contrôles réussis, deux processus natifs terminent 1 200 ticks via TLS avec heartbeat actif, pause asymétrique de 12 secondes, perte et retransmission explicite. Le pont UDP reste possédé par une seule tâche. Aucun routeur ou pare-feu n’est modifié par les tests. Une nouvelle tentative entre les deux réseaux doit confirmer la correction.
