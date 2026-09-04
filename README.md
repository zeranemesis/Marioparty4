# Mario Party 4 / PartyBoard — sources expérimentales

Copie des sources de travail au 4 septembre 2026, avec les modifications locales
audio, rendu, contrôleurs et netplay. Le projet se trouve dans **[partyboard](partyboard/)**.

## Télécharger sur un autre PC

Utiliser **Code → Download ZIP**, puis extraire l'archive, ou :

```powershell
git clone https://github.com/zeranemesis/Marioparty4.git
cd Marioparty4/partyboard
```

Les sources modifiées d'Aurora, MusyX et libco sont incluses comme dossiers
ordinaires : le ZIP contient donc aussi ces dépendances, sans initialisation
de sous-modules. Les références d'origine sont dans
[SOURCE_REVISIONS.json](partyboard/SOURCE_REVISIONS.json).

**Ce téléchargement contient les sources, pas une version prête à lancer.**
Il faut installer les outils C++/CMake décrits dans
[building.md](partyboard/building.md), puis compiler. Les autres dépendances
sont obtenues par la configuration CMake. Utiliser cette copie, pas la commande
de clonage du dépôt amont figurant dans son ancien guide.

Sous Windows, le preset `windows-msvc-relwithdebinfo` est disponible pour une
invite développeur Visual Studio disposant de CMake et Ninja. La configuration
d'un PC vierge n'a pas encore été validée pour cet instantané.

Aucune ISO/RVZ, aucun fichier du disque extrait, aucune sauvegarde ni ROM DSP
n'est ajouté à cette copie des sources. Il faut fournir sa propre image du jeu.
Les ressources d'interface déjà présentes dans le projet amont sont conservées.

## État réel des fonctionnalités

- Audio : corrections locales et tests de régression inclus ; pas de garantie
  que toute la bande-son est parfaite dans tous les modes.
- Netplay du moteur : expérimental, deux joueurs en lockstep.
- Salons : composant de gestion de quatre places testé, pas encore raccordé au menu.
- Rollback : noyau et tests à quatre sessions simulées ; pas encore actif dans le jeu.
- Connexion sans serveur externe : diagnostic local inclus ; ouverture automatique
  des ports, invitations sécurisées et connexion Internet restent à intégrer.

Voir [l'état détaillé](partyboard/docs/NETPLAY_LOBBY.md).
Les scripts de test se trouvent dans `partyboard/tools/`.

## Origine et notices

Projet amont : [MarioPartyRD/partyboard](https://github.com/mariopartyrd/partyboard).
Les sources et notices des dépendances sont conservées dans `partyboard/extern/`.
Voir aussi [la notice de référence Dolphin AX](partyboard/DOLPHIN_AX_NOTICE.md).
Cet instantané ne constitue pas une validation juridique globale des droits de
redistribution de tous les composants et ressources amont.

Les anciens fichiers déjà présents à la racine et dans `outputs/` ne sont pas
la nouvelle version du jeu et n'ont pas été supprimés lors de cet ajout.
