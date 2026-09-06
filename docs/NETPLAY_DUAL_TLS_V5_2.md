# Troisième diagnostic : séparation physique des sens TLS, v5.2

Les deux PC utilisent le même build 23F0D3…B36C, le même disque et la même tentative. Le correctif v5.1 fonctionne dès le départ : les compteurs entrants et sortants progressent de façon symétrique, le ping reste à 1–2 ms, les graines concordent et les deux moteurs passent du contexte 1 au contexte 74.

Le blocage survient ensuite aux ticks 993/995. Les derniers paquets sont 992/994, les contextes observés sont identiques (74) et aucune configuration incompatible ou erreur d’envoi n’apparaît. Les paquets locaux continuent d’entrer dans les ponts, tandis que les compteurs TLS reçus se figent à 1 998/1 983. Cela exclut le disque, le départ, la graine, le contexte et la production d’entrées comme causes de cette tentative.

La v5.2 n’utilise plus une connexion TLS en duplex. Le client ouvre deux connexions authentifiées avec le même certificat épinglé, secret, expiration et empreinte de build. Le canal 0 porte uniquement client vers hôte ; le canal 1 uniquement hôte vers client. L’hôte refuse un canal dupliqué et n’ouvre le salon qu’après authentification des deux. Les deux utilisent le même port temporaire : aucune seconde règle de pare-feu ni second bail de box.

Le pont conserve une seule tâche d’écriture et une seule tâche propriétaire de la socket UDP locale. La séparation TCP/TLS supprime toute dépendance entre une écriture et le lecteur du sens opposé dans SslStream/Schannel.

Validation : compilation réussie ; 86 contrôles réussis. Deux connexions TLS distinctes sont authentifiées et testées, puis deux processus natifs terminent 1 200 ticks avec ping, transitions de contexte, pause asymétrique de 12 secondes et retransmission ciblée. Aucun test local ne remplace une nouvelle tentative entre les deux réseaux.
