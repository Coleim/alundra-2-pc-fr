# Alundra 2 — version PC native (français)

Ce programme fait tourner Alundra 2 (version française, SLES-02601) directement sur votre
ordinateur : le code du jeu a été traduit en C et compilé pour Windows et macOS. Il n'y a
ni émulateur ni BIOS de console.

Le programme ne contient **aucun fichier du jeu**. Au premier lancement, il vous demande
l'image de **votre propre disque** et en copie les fichiers. Ensuite, l'image n'est plus
nécessaire.

## Ce qu'il vous faut

- Le disque original **Alundra 2, version française** (référence SLES-02601).
- Une image de ce disque au format **.cue + .bin** (mode RAW, secteurs de 2352 octets),
  faite avec votre lecteur de CD et un logiciel de copie de CD (ImgBurn sous Windows,
  par exemple : « Créer une image à partir du disque », format BIN/CUE).
- Windows 10 ou 11 (64 bits), ou macOS 11 ou plus récent (Mac Intel ou Apple Silicon).
- Environ 600 Mo d'espace disque.

## Windows

1. Décompressez `alundra2-fr-…-windows-x64.zip` où vous voulez.
2. Lancez `alundra2.exe`. Si Windows affiche « Windows a protégé votre ordinateur »,
   cliquez sur « Informations complémentaires » puis « Exécuter quand même ».
3. Au premier lancement : appuyez sur une touche, puis choisissez le fichier `.cue` de
   votre image. La copie prend environ une minute, puis le jeu démarre.

Les fichiers du jeu sont copiés dans `%APPDATA%\alundra2-fr\data` et les cartes mémoire
sont dans `%APPDATA%\alundra2-fr\memcards`.

## macOS

1. Décompressez `alundra2-fr-…-macos.zip` et glissez « Alundra 2 » dans le dossier
   Applications.
2. La première fois : clic droit (ou Ctrl-clic) sur l'application, « Ouvrir », puis
   « Ouvrir » à nouveau (l'application n'est pas signée par Apple).
3. Appuyez sur une touche et choisissez le fichier `.cue` de votre image.

Les fichiers du jeu sont copiés dans `~/Library/Application Support/alundra2-fr/data` et
les cartes mémoire sont dans `~/Library/Application Support/alundra2-fr/memcards`.

## Commandes

Une manette est reconnue automatiquement (Xbox, PlayStation, Switch Pro, etc.). Les
boutons suivent la position de ceux d'une manette PlayStation : le bouton du bas est ✕, celui
de droite ○, celui de gauche □ et celui du haut △. Le jeu voit une DualShock en mode
analogique (sticks et vibrations).

| PlayStation | Clavier |
|---|---|
| Croix directionnelle | Flèches |
| ✕ | X ou Espace |
| ○ | C |
| □ | Z |
| △ | S |
| L1 / R1 | Q / E |
| L2 / R2 | 1 / 3 |
| START | Entrée |
| SELECT | Retour arrière ou Maj droite |

- **F11** ou **Alt+Entrée** : plein écran ; **Échap** quitte le plein écran.
- Fermer la fenêtre quitte le jeu (pensez à sauvegarder dans le jeu avant).

## Sauvegardes

Le jeu sauvegarde sur une carte mémoire virtuelle (`card1.mcd`, 128 Ko, au format standard
des cartes PlayStation). Ces fichiers sont compatibles avec les émulateurs qui utilisent le
format `.mcd` : vous pouvez copier une carte d'un émulateur dans le dossier `memcards`
(sous le nom `card1.mcd`) et inversement. Une carte `card2.mcd` ajoutée dans ce dossier
apparaît dans la fente 2.

Les cartes de la version 0.2 sont copiées automatiquement au premier lancement.

## Options

On peut lancer le programme avec des options (raccourci Windows, ou Terminal sur macOS :
`"/Applications/Alundra 2.app/Contents/MacOS/alundra2" --fullscreen`) :

| Option | Effet |
|---|---|
| `--fullscreen` | démarrer en plein écran |
| `--scale N` | taille de la fenêtre (N × 320×240, 3 par défaut) |
| `--smooth` | image lissée au lieu de pixels nets |
| `--vsync` | synchronisation verticale de l'écran |
| `--import image.cue` | (re)copier les fichiers depuis une image |
| `--data DOSSIER` | dossier des fichiers du jeu |
| `--saves DOSSIER` | dossier des cartes mémoire |

## En cas de problème

- « Ce n'est pas Alundra 2 (France, SLES-02601) » ou « SLES_026.01 ne correspond pas » :
  l'image n'est pas celle de la version française du jeu.
- « pas une image MODE2/2352 » : refaites l'image en mode RAW (BIN/CUE), pas en ISO.
- Pour recommencer la copie des fichiers : supprimez le dossier `data` indiqué plus haut et
  relancez le jeu.
- Les messages du programme sont écrits dans `alundra2.log`, dans le même dossier que `data`
  (`%APPDATA%\alundra2-fr` ou `~/Library/Application Support/alundra2-fr`) : joignez-le à
  tout signalement de problème.

## À savoir

Ce programme est fait pour jouer avec votre propre exemplaire du jeu. Il est dérivé du code
du jeu : ne le diffusez pas.
