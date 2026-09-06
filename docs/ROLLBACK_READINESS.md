# Rollback : avancement du moteur

Le jeu en ligne reste en lockstep. Le noyau de rollback sait prédire, restaurer et rejouer un état fourni par des callbacks, mais les snapshots du moteur ne couvrent pas encore une partie complète. Ne pas interpréter une sonde de capture réussie comme une validation du rollback en jeu.

## Incrément actuel

- Noyau : les erreurs ou exceptions de sauvegarde, restauration et simulation rendent la session terminale. Une sauvegarde partiellement écrite ne peut pas être restaurée. Le signal de resimulation couvre aussi la restauration, est restauré automatiquement à la sortie et isolé par thread.
- Manettes : snapshot explicite de 30 régions, dont boutons précédents, répétitions, anciennes directions, sticks, gâchettes, erreurs, compteurs et état logique des vibrations. Les handles SDL et callbacks de périphériques sont exclus. La restauration n'émet aucune commande matérielle. Elle doit être effectuée à une frontière de tick sans polling concurrent.
- Aléatoire et compteur : capture/restauration des graines frand et rand8 et de GlobalCounter dans une structure versionnée distincte.
- Sonde native : ajoute le PAD et les graines/compteur aux captures existantes des heaps, coroutines et overlay. Le message précise que le snapshot reste partiel et le rollback complet désactivé.

## Travail indispensable avant activation

1. Couvrir explicitement les globals du moteur : objets, séquences de mini-jeu, modèles, motions, caméras et sprites. Ne pas copier en bloc le processus ou les sections du moteur contenant SDL, audio, fichiers et sockets.
2. Isoler un tick déterministe complet. HuPrcCall et MGSeqMain ne suffisent pas : Hu3DExec avance aussi Hu3DMotionNext, HuSprFinish et Hu3DAnimExec hors de la boucle de simulation actuelle.
3. Gérer les durées de vie des allocations, coroutines et modules. Une adresse réutilisée ne prouve pas que l'objet est le même ; la prévalidation d'un composant ne rend pas l'ensemble des snapshots atomique.
4. Gérer les effets externes : audio, vibrations, sauvegardes et succès. Le signal de resimulation est disponible mais il n'est pas encore consommé par ces sous-systèmes.
5. Vérifier une vraie restauration puis resimulation sur le moteur et comparer l'état obtenu à une exécution sans prédiction, avant toute activation dans le salon.

Les tests de noyau utilisent un état synthétique. Les tests PAD/horloge vérifient de vrais globals du jeu en mode headless, sans partie graphique. Aucune de ces validations ne prouve à elle seule le déterminisme du jeu complet.

## Validation de cet incrément

Le noyau passe 49 334 contrôles (tools/test_rollback_network.ps1). Les nouveaux tests couvrent les erreurs et exceptions des callbacks ainsi que l'isolation du signal de resimulation entre threads.

Les restaurations de coroutines et d'overlay prévalident désormais toute la structure avant écriture. Elles refusent les topologies différentes, pointeurs non reconnus, doublons, tailles invalides et buffers aliasant la destination. tools/test_snapshot_restore.ps1 passe 25 contrôles en compilant les véritables process.c et objdll.c, avec buffers de coroutine et module PE synthétiques ainsi que des services périphériques remplacés. Ces tests ne valident pas une reprise réelle de coroutine. La validation reste locale à chaque composant, sans transaction atomique globale.

Le test natif --netplay-self-test inclut maintenant les aller-retour PAD et graines/compteur, ainsi que le refus de format/version/taille incorrects avant mutation. Il restaure les valeurs initiales après chaque essai.

Compilation native et compagnon réussie. --netplay-self-test : PASS pour noyau, snapshots de coroutine/mémoire, transport et runtime incluant les nouveaux états PAD/horloge. Les avertissements de compilation concernent des fichiers REL non modifiés par cet incrément.

