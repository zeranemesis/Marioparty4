# Rooms en ligne et LAN : test entre deux PC

Ce qui a été vérifié sur un seul PC : le lobby, l'annuaire du relais déployé, le TURN de Cloudflare, CubeShelf (profil de test) et le menu F1 du jeu. Ce qui ne peut l'être qu'avec deux machines est ici, dans l'ordre.

## À préparer

- **Deux PC Windows**, chacun avec le **même fichier disque** (Mario Party 4 USA révision 1 ou européen). Un fichier différent ne voit pas les rooms de l'autre : c'est voulu.
- **PartyBoard** de la branche `claude/relay` sur les deux : l'artefact Windows du dernier run vert de GitHub Actions (onglet Actions, workflow Build, branche `claude/relay`).
- **CubeShelf** de la branche `claude/online-rooms` sur les deux, avec Mario Party 4 pointant vers ce PartyBoard (page du jeu).
- Pour le test « Amis » : chaque CubeShelf a une identité (Mon profil) et l'autre en ami.
- Pour un vrai test Internet : un des deux PC sur un **partage de connexion 4G** (c'est un CGNAT, le cas que le relais existe pour couvrir).

Après chaque test, sur les deux PC : bouton **Exporter diagnostic** de la fenêtre PartyBoard (ou `%LOCALAPPDATA%\PartyBoard\Diagnostics`, dossier le plus récent).

## 1. LAN (même maison, même box)

1. PC A : CubeShelf, **En ligne**, section **Réseau local**, **Créer une partie LAN**. Windows demande une autorisation : accepter.
2. PC B : CubeShelf, **En ligne**. La partie de A apparaît sous **Réseau local** en 10 s au plus. **Rejoindre**.

Attendu : les deux fenêtres PartyBoard montrent les deux joueurs, disques « Identique », ping de quelques ms. Le diagnostic de B dit `path=lan`. Rien ne passe par Internet (on peut débrancher la box d'Internet : ça marche encore).

À noter si ça rate : un pare-feu tiers (antivirus) peut bloquer la question envoyée sur le réseau (UDP 47474).

## 2. Room privée entre amis, deux réseaux différents

1. PC B sur la 4G. PC A sur la box.
2. PC A : **En ligne**, **Privée (amis invités)**, **Créer**.
3. PC B : page **Amis** ou **En ligne** : l'invitation de A apparaît (jusqu'à deux minutes, le temps de la présence). **Rejoindre**.

Attendu : connexion, diagnostic `path=direct` si la box de A s'est ouverte, sinon `path=relay`. Ping noté.

Puis lancer une partie (au moins un tour de plateau et un mini-jeu) : pas de désynchronisation.

## 3. Room publique

1. PC A : **En ligne**, **Publique**, **Créer**. La carte **Ta room** dit « publique · En attente d'un joueur ».
2. PC B : **En ligne** : la room de A est dans **Disponibles maintenant**, avec le pseudo de A, la langue, le disque, « Tu as les mêmes mods ». **Rejoindre**.

Attendu : `path=relay` sur les deux (une room publique passe toujours par le relais : l'adresse de A n'est publiée nulle part). La room passe dans **En jeu** (complète) sur les deux CubeShelf, avec les deux pseudos.

3. PC A : **Rendre privée** avant que B rejoigne : la room disparaît de la liste de B au rafraîchissement suivant. **Rendre publique** : elle revient.

## 4. Mods : « Installer et rejoindre »

1. PC A : activer un mod GameBanana (page Mods), créer une room publique.
2. PC B, sans ce mod : la carte dit « Il te manque 1 mod(s) » et le bouton **Installer et rejoindre**. Le cliquer, accepter la confirmation d'installation.

Attendu : le mod s'installe, B entre, et les deux lobbies montrent les mêmes mods. Après la partie, sur B, le mod est **désactivé** dans la page Mods (il n'a été activé que pour la room).

Si la version sur GameBanana a changé depuis l'installation de A, CubeShelf le dit et n'entre pas : c'est attendu.

## 5. Le menu F1 du jeu

1. PC A : lancer Mario Party 4 depuis CubeShelf, appuyer sur F1, onglet **Amis**.
2. PC B : créer une room publique.

Attendu sur A : la room de B dans **Rooms publiques** (20 s au plus), avec **Rejoindre** si les mods sont les mêmes. Le cliquer : le jeu prévient qu'il va se fermer, puis le salon s'ouvre.

## 6. Deux joueurs sur la même room

PC B et un troisième PC (ou un second CubeShelf avec `CUBESHELF_DATA_DIR`) cliquent **Rejoindre** sur la même room publique de A au même moment.

Attendu : un seul entre ; l'autre lit « Cette room vient d'être prise par un autre joueur » tout de suite, sans attendre.

## Ce qu'il faut me renvoyer

Pour chaque test : réussi ou non, ce qui a été vu à l'écran, et les deux diagnostics exportés. Rien d'autre : les diagnostics ne contiennent ni adresse ni invitation.
