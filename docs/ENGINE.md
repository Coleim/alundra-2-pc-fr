# Le moteur du jeu (ce qui est compris)

Notes sur la structure du jeu, établies en réécrivant le code (phase 3). Elles servent aux
améliorations de la phase 7 (60 images/s, écran large, caméra libre). Adresses de la version
française (SLES-02601).

## Modules en mémoire

| Zone | Contenu |
|---|---|
| `0x80010000` | bibliothèque `AL22` (assembleur écrit à la main) : 20 points d'entrée à `0x80010008 + 8n` (matrices et vecteurs, tests de collision, préparation de la projection) ; quatre variantes (`PACK0.PAK` entrées 106 à 109) |
| `0x80017000` | programme principal : démarrage, chargeur de fichiers, manettes, pilote son, bibliothèque Sony |
| `0x80084ED0` | module du mode courant : `GAME00` (exploration et combats), `EVENT00` (cinématiques), `GAME01-03` |
| `0x80149AE8` | un des 17 modules de code de `PACK1.PAK` (lieux, mini-jeux...) |

Le programme principal lance un mode par la table `0x800521F0` (nom du fichier, point d'entrée) :
`GAME00` démarre à `0x80089AE8`, `EVENT00` à `0x80087848`.

## Une image de jeu (`GAME00`, `func_80089AE8`)

La boucle principale appelle chaque image, dans l'ordre : la logique (acteurs, collisions,
caméra : `func_8008BE90`), puis les passes de dessin qui remplissent les tables d'ordre
(`func_800AD2B0`, `func_8001F224`, `func_800220EC`, `func_800B1C94`, `func_800AD564`, chacune
reçoit le pointeur de paquets et une couche), puis la fin d'image `func_8008A390(mode)`.

La fin d'image (`src/game/frame.c`) : ferme les listes, avance le flux CD (`func_800899CC`), tire
le nombre aléatoire de l'image (`0x8012BDE0 = 0x8012BDE0 * 154811`, 16 bits), attend le GPU,
envoie les palettes en attente, attend le retour vertical avec `VSync(mode)`, affiche l'image et
lance son dessin.

- **Cadence** : `mode` vaut `0x8005D218` = 2 en exploration : une image tous les deux retours
  verticaux, **25 images par seconde** en PAL. Toute la logique avance d'un pas par image : passer
  à 50 ou 60 images/s demande de diviser les vitesses et les minuteries (ou d'interpoler l'affichage).
- **Double tampon** : deux zones de 320x232 en VRAM (y = 0 et y = 256), affichées en 256x232
  décalées par le réglage de position de l'écran (`0x80066150`).
- **Tables d'ordre** : deux jeux (`0x801179A0`, `0x80119A40`), chacun avec six couches chaînées
  de l'arrière vers l'avant : fond (2 entrées), 3D triée en profondeur (2 049 entrées), trois
  couches d'effets et d'interface, puis l'avant (cachée si `0x800FA83C`), plus une liste à part
  (9 entrées) envoyée après.
- **Paquets** : deux tampons (`0x80112988` + `0x5000`/`0x5004`), écriture à partir de `+0xC000`,
  limite à `+0xE7CE` ; un dépassement arrête le jeu avec un message.
- **Débogage** laissé dans le jeu : si `0x8005D21C` vaut 0, R1 tenu sur la manette 2 fige le jeu
  et R2 avance d'une image ; un compteur de performance (`func_80087E34`).

## Caméra (`src/game/camera.c`)

Structure à `0x8011BAF8` : le point suivi `pos` rattrape sa cible (un acteur, plus un décalage)
avec une zone morte et un rattrapage, l'œil est à `eye` (`eye_ofs` = `eye` - `pos`). Les modes
sont dans la table `0x800EEB10` (indice `+0x00`). Variantes : rail (`+0x108` = 2 : approche
progressive ou bornée), orbite autour de la cible (`+0x108` = 1 : lacet sur 512 pas, tables de
sinus `0x80085258` et cosinus `0x80085658`), déplacement de l'œil vers une seconde cible
(`+0x5C`), zoom le long de `eye_ofs` (`+0x54`). Une caméra libre de débogage existe
(`+0x3C`, `func_8008BED0`, pilotée à la manette). La matrice de vue est reconstruite par
`func_8008B8F4`.

## Projection

Le programme principal prépare le GTE pour une vue (`func_8002569C`, `func_80025A9C`,
`func_80025B90`) : matrice de rotation et de translation, `SetGeomOffset` (centre de l'écran) et
`SetGeomScreen` (distance de l'écran, champ de vision), brouillard (`SetFarColor`, `SetDQA`,
`SetDQB`). Leur second argument, quand il est non nul, double les valeurs verticales (sans doute
pour un affichage entrelacé sur 480 lignes). Pour l'écran large, c'est là que le champ de vision
horizontal se règle ; les tests qui écartent les objets hors de l'écran restent à trouver dans les
passes de dessin.

## Acteurs

Table des acteurs à `0x8011DCE0` ; position en `+0x18`, `+0x1C`, `+0x20` (20.12). Les
collisions et interactions passent par `func_800A71FC` (avec la bibliothèque `AL22`).
