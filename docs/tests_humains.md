# Ce que seul un humain peut tester — et dans quel ordre

Écrit le 2026-09-13 au matin, après une nuit de campagnes automatiques qui a mené
neuf parties jusqu'à leur dernier tour et nommé neuf défauts.

Le principe qui ordonne tout ce qui suit : **le temps humain est la ressource la
plus rare du projet.** Il ne doit pas servir à chercher ce qu'une machine trouve
toute seule la nuit — il doit servir à ce qu'aucune machine ne peut faire.

## Ce que les campagnes ne verront jamais

- **Les défauts de rendu.** Le hachage canonique exclut la présentation par
  construction (`netplay_canonical.hpp` le dit en toutes lettres). Une ombre
  manquante, un HUD décalé, un feu d'artifice qui bave : aucune campagne ne peut
  les voir. Les neuf de `defauts_graphiques.md` viennent tous de toi.
- **La jouabilité.** Un `DETERMINISM: PASS` et un `STABILITY: PASS` sont
  parfaitement compatibles avec un jeu injouable. Seul quelqu'un qui tient la
  manette sait si l'entrée répond.
- **Le vrai réseau.** Tout ce qui a été mesuré l'a été sur `127.0.0.1`. Zéro
  session entre deux machines à ce jour.

---

## 1. Une partie complète à deux machines

**C'est le critère d'acceptation du projet, et il n'a jamais été démontré une
seule fois.** Tout le reste passe après.

Sur la machine hôte :

```
powershell -File tools/record_board_session.ps1 `
    -DiscPath "...\Mario Party 4 (USA) (Rev 1).iso" `
    -Role Host -Port 7000 `
    -BinaryDirectory build/d24/RelWithDebInfo `
    -Label session1
```

Sur l'autre machine :

```
powershell -File tools/record_board_session.ps1 `
    -DiscPath "...\Mario Party 4 (USA) (Rev 1).iso" `
    -Role Join -JoinAddress <ip-hote>:7000 `
    -BinaryDirectory build/d24/RelWithDebInfo `
    -Label session1
```

Puis, une fois les deux dossiers rassemblés :

```
powershell -File tools/merge_session.ps1 <dossier-hote> <dossier-client>
```

**Passer par le recorder plutôt que lancer le jeu à la main change tout** : une
session enregistrée produit un artefact permanent — verdict, chemin d'overlays,
empreintes d'état, et un scénario rejouable. Sans lui, deux heures de jeu ne
laissent qu'une anecdote.

Si la partie doit être courte, ajoute `--netplay-max-turns=10` aux deux côtés :
dix tours suffisent à traverser tout le jeu, fin comprise.

---

## 2. L'expérience de G6, qui ne prend que la fin d'une partie

C'est le défaut que tu as rapporté — *« la fin est buggée avec le jeu qui
continue malgré être le gagnant »* — et il est maintenant reproduit trois fois
en campagne automatique.

La fonction qui bloque est une attente sans délai de sortie sur **une seule
manette** (`mstory3Dll/main.c:509`). L'hypothèse est qu'elle attend la manette du
gagnant, et que si le gagnant est un joueur contrôlé par l'ordinateur, personne
n'appuiera jamais.

**Ce qu'il faut observer, et c'est tout :** joue une partie jusqu'au classement,
et note **qui gagne**.

- Un humain gagne et la fin se déroule → l'hypothèse tient.
- Un CPU gagne et le jeu se fige → l'hypothèse est confirmée.
- Un CPU gagne et la fin se déroule quand même → l'hypothèse tombe, et c'est
  aussi utile.

Une partie de cinq tours suffit.

---

## 3. Le contenu que la machine n'atteint pas

Un humain traverse un menu en dix secondes là où le marcheur automatique y passe
une heure. Ces quatre-là valent le détour :

| contenu | comment |
|---|---|
| **`w06Dll`** Bowser's Gnarly Party | ajouter `-UnlockBowser` au recorder, **des deux côtés** |
| **`w20Dll`**, **`w21Dll`** | entrées 3 et 4 du menu des modes |
| **Les mini-jeux de la salle Extra** (455 à 462) | mode mini-jeu ; ils ne sortent jamais au tirage d'un plateau |
| **`w05Dll`** | deux crans à droite dans la liste des plateaux |

Sur `-UnlockBowser` : le réglage est **par machine** et **n'entre pas dans le
hachage canonique**. Deux machines qui ne sont pas d'accord verraient donc des
menus différents avec des états déclarés identiques. Les deux doivent passer le
drapeau, ou aucune.

---

## 4. Les défauts de rendu

Rejoue les cinq mini-jeux de la liste d'hier — Slime Time, Avalanche!, Right Oar
Left, Para-sailing, Trace Race — et dis si les défauts sont toujours là.

Un en particulier mérite un œil neuf : **l'explosion de la tête de Bowser en
accéléré**, la première moitié de G6. C'est la signature d'une animation pilotée
par le nombre d'images affichées plutôt que par les ticks de simulation, et c'est
exactement la famille du défaut corrigé cette nuit (la carte son qui décidait de
la fin des vidéos). **Il se peut qu'elle ait changé.**

---

## Ce sur quoi il ne faut PAS dépenser une session

Chercher des désynchronisations en jouant au hasard. La machine en a trouvé neuf
en une nuit, gratuitement, pendant que tu dormais. Une session à deux personnes
et deux machines vaut trop cher pour servir à ça.

Et si une session tombe quand même sur une divergence : **ne relance pas pour
voir si ça passe**. Le rapport est l'artefact ; un `FAIL` enregistré vaut plus
qu'un `PASS` obtenu au deuxième essai.
