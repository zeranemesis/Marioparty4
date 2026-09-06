# Diagnostic de synchronisation

Chaque Session crée son propre dossier sous LocalAppData/PartyBoard/Diagnostics : session.txt et native.txt. Exporter diagnostic rassemble ces deux fichiers en texte, plafonné à 1 Mio par composant. Le dernier rapport reste accessible après réouverture du compagnon. Rien n'est envoyé automatiquement.

Le compagnon enregistre rôle, empreinte du build, identifiant de tentative partagé, chargement/prêt/départ/fermeture, phase, comparaison du disque, ping, compteurs locaux/distants. Le moteur enregistre toutes les deux secondes UTC, tick, contexte, disponibilité des entrées locales/distantes, dernier tick reçu, paquets acceptés/refusés, réparations, erreurs d'envoi, âge du dernier paquet et accord RNG. Un arrêt terminal ajoute un état immédiat. Chaque journal est borné à 3 000 lignes. Le journal natif est distinct de stdout/stderr : aucun chemin personnel, secret d'invitation, endpoint réseau ou pseudo n'est collecté.

Validation : 85 contrôles réussis. Deux exécutables natifs via TLS ont écrit les diagnostics pendant une pause de 12 secondes et une entrée perdue ; le compteur de réparation non nul et les logs des deux côtés sont vérifiés. Export combiné et limite de taille en lignes testés. Les vrais réseaux des utilisateurs restent à diagnostiquer : aucune cause du blocage au logo n'est encore démontrée.
