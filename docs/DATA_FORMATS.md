# Formats de données (inventaire, phase 1)

Résumé de ce qui a été mesuré sur le disque français SLES-02601. Tout ce qui vient du disque reste dans
`extracted/` (ignoré par Git). Outils : `tools/extract_disc.py`, `tools/inventory_data.py`.

## Fichiers du disque

| Fichier | Taille | Format |
|---|---|---|
| `SLES_026.01` | 284 Ko | exécutable PS-X EXE, texte à `0x80017000` (`0x46800` octets) |
| `GAME00.BIN` | 787 Ko | module de code + données, chargé à `0x80084ED0` (moteur) |
| `EVENT00.BIN` | 476 Ko | idem, cinématiques (remplace GAME00 à la même adresse) |
| `GAME01/02/03.BIN` | 450 / 473 / 383 Ko | idem (menus, pause, etc.), même adresse ; code limité à 3-35 Ko chacun |
| `DATA/PACK0.PAK` | 16 Mo | archive à table d'offsets (768 emplacements), 746 entrées |
| `DATA/PACK1.PAK` | 0,7 Mo | archive à table d'offsets, 36 entrées, **contient du code** (voir ci-dessous) |
| `DATA/PAKVCAST.PAK` | 71 Mo | archive à table d'offsets, 291 entrées (cinématiques temps réel ?) |
| `DATA/PAKOMAKE.PAK` | 4 Mo | archive à table d'offsets, 116 entrées |
| `DATA/GRP_FR.GP` | 197 Mo | archive à table d'offsets (512 emplacements), graphismes en langue française |
| `STEREO.XA` | 190 Mo | secteurs XA bruts (2352 octets) : musiques/voix stéréo |
| `MONO.XA` | 26 Mo | secteurs XA bruts : voix/sons mono, 16 canaux |
| `A2STR.STR` | 18 Mo | vidéo STR (MDEC), secteurs bruts |

## Archives `PAK` / `GP`

Début du fichier : table de mots 32 bits = positions (en octets) des entrées ; un emplacement à 0 est vide.
Dans `PACK1`, la table contient aussi de petits blocs de 4 octets (identifiants de type) intercalés entre
les vraies entrées.

### `GRP_FR.GP` (512 entrées, 2 identiques deux à deux par endroits)
Chaque entrée a le même conteneur : `[0]=0`, `[1]=taille section A`, `[4]=taille A`, `[5]=taille B`,
`[6]=offset section suivante`, `[7]=taille section C`. Contenu : textures 16 bits (108 images TIM 16 bits
trouvées dans le fichier), palettes, modèles (1 697 candidats à la signature TMD, à confirmer : cette
signature est courte et donne des faux positifs).

### `PACK1.PAK` : modules de code
- 17 entrées sont du code MIPS (de 4 à 45 Ko). **Toutes se chargent à `0x80149AE8`** (modules interchangeables,
  comme `GAME00/EVENT00` à `0x80084ED0`) : vérifié par les 481 appels internes qui retombent tous dans
  le programme principal, `GAME00` ou le module lui-même.
- Les entrées 34 et 35 sont confirmées par les photos de code prises pendant une partie
  (adresse = position dans `PACK1` + `0x80119A4C` pour l'entrée 35, `+0x8011B988` pour l'entrée 34).
- Début d'entrée : un identifiant de type, puis soit une table de pointeurs (`0x8014Axxx`), soit du code.

### `PACK0`, `PAKVCAST`, `PAKOMAKE`
Entrées de données (images TIM 16 et 24 bits dans `PAKVCAST`, nombreux candidats TMD, 4 flux MPEG).
Pas de code MIPS détecté à l'inventaire.

## Son

- **Aucune signature VAB (`pBAV`) ni SEQ (`pQES`) trouvée** dans les fichiers : le jeu utilise un format de
  banque et de séquence qui lui est propre (le moteur sonore est dans le programme ; messages `Vab Error`,
  `Se Error`, `BgmVabNo` dans le programme principal).
- XA : `STEREO.XA` expose au moins 8 canaux, `MONO.XA` 16 (canaux lus dans les sous-en-têtes de secteur).

## Vidéo

- `A2STR.STR` : flux STR classique (secteurs bruts), à décoder par le décodeur MDEC.
- Quelques signatures MPEG dans `GRP_FR.GP` et `PAKVCAST.PAK` (6 et 4), à examiner.

## À faire (suite de l'inventaire, phase 3)

- Décoder les en-têtes de section de `GRP_FR` et classer les entrées (texture / modèle / animation / carte).
- Identifier le format des banques sonores et des séquences (via le code du moteur sonore).
- Retrouver la correspondance entre entrées de `PAK` et zones/événements du jeu (via le chargeur).
