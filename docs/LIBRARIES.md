# Bibliothèques Sony (PsyQ) dans le programme principal

Phase 2 : séparer le code de la bibliothèque Sony (remplacé par du code PC natif en phase 4) du
code du jeu (réécrit en C en phase 3).

## Méthode

`tools/psyq_sigs.py` compare le code du jeu aux signatures publiques des bibliothèques PsyQ
([lab313ru/psx_psyq_signatures](https://github.com/lab313ru/psx_psyq_signatures) : pour chaque
version du kit de développement, chaque fichier objet de chaque bibliothèque, ses octets avec les
adresses masquées et ses étiquettes). Les signatures sont clonées **à côté** du dépôt, pas dedans :

    git clone --depth 1 https://github.com/lab313ru/psx_psyq_signatures ../psx_psyq_signatures
    python3 tools/psyq_sigs.py            # écrit symbols/psyq_names.txt et symbols/psyq_objects.json
    python3 tools/build_symbols.py        # ajoute les noms (couche entre lib_names et manual)
    python -m splat split config/main.yaml

Règles : recherche sur des positions alignées sur 4 octets, dans le programme principal, les
modules `GAME`/`EVENT` et les modules de code de `PACK1` ; au moins 24 octets fixes par signature ;
un objet n'est retenu que s'il apparaît **une seule fois** et ne chevauche pas un objet plus long ;
pour chaque bibliothèque, on garde la version du kit qui couvre le plus d'octets (les jeux mélangent
souvent les versions). Durée : une vingtaine de secondes pour les 16 versions.

## Résultat

**149 fichiers objets reconnus, 547 fonctions, 414 noms**, tous dans le programme principal.
Aucun code de bibliothèque dans `GAME00-03`, `EVENT00` ni les modules de `PACK1` : ces modules sont
entièrement du code du jeu.

| Bibliothèque | Version du kit | Objets | Fonctions | Octets | Zone principale |
|---|---|---|---|---|---|
| libc2 (mémoire, chaînes, `rand`, `printf`) | 4.5 | 23 | 28 | 6 352 | `0x80031258`-`0x80032A94` |
| libpress (décodeur vidéo MDEC) | 4.5 | 3 | 19 | 2 800 | `0x80032AE8`-`0x800335D8` |
| libgpu (graphismes) | 4.5 | 21 | 76 | 13 504 | `0x800335D8`-`0x80036AA8` |
| libgte (géométrie 3D) | 4.5 | 35 | 78 | 9 024 | `0x80036AA8`-`0x80038DF8` |
| libcd (lecteur CD) | 4.5 | 12 | 49 | 11 840 | `0x80038DF8`-`0x8003B13C`, `0x80043F88`-`0x800448D0` |
| libetc (`VSync`, interruptions, manette de base) | 4.7 | 5 | 28 | 3 264 | `0x8003B1C8`-`0x8003BEF8` |
| libapi (noyau : événements, fichiers, appels BIOS) | 4.5 | 5 | 17 | 1 744 | `0x8003BAB8`-`0x8003FBD8` |
| libmath (calcul en virgule flottante logicielle) | 4.7 | 13 | 14 | 4 176 | `0x8003BEF8`-`0x8003CF48` |
| libsn (démarrage : `__SN_ENTRY_POINT` = point d'entrée `0x8003CFCC`) | 4.6 | 6 | 8 | 532 | `0x8003CF58`-`0x8003D318` |
| libmcrd + libcard (cartes mémoire) | 4.5 / 4.7 | 9 | 63 | 13 216 | `0x8003D318`-`0x80040EF8` |
| libds (lecteur CD, couche « Ds ») | 4.5 | 9 | 102 | 16 352 | `0x80040F08`-`0x800459D8` |
| libpad (manettes) | 4.5 | 7 | 64 | 12 448 | `0x800459D8`-`0x80048A78` |
| libsnd (`SsGetNck` seulement) | 4.0 | 1 | 1 | 36 | `0x8004EF18` |

Le kit est donc **PsyQ 4.5 environ** (quelques objets plus récents), cohérent avec la chaîne
`sys.c,v 1.140 1998/01/12` de libgpu.

### Contrôle

Sur les 29 fonctions de bibliothèque déjà nommées à la main ou par les messages de débogage,
24 portent le même nom ; 2 noms devinés sont corrigés (`SetGraphQue` → `SetGraphQueue`,
`mcrd_event_overflow` → `UserFuncOpen`) ; 3 sont des fonctions internes (`static`) que les signatures
ne nomment pas (`MDEC_rest`, `v_wait`, `DS_newmedia`) et gardent leur nom. Le point d'entrée du
programme tombe exactement sur `__SN_ENTRY_POINT`.

## Carte du programme principal

| Zone | Fonctions | Nature |
|---|---|---|
| `0x80017000`-`0x80019910` | - | données (chaînes, tables) |
| `0x80019910`-`0x80031248` | 411 | **code du jeu** |
| `0x80031258`-`0x80048A78` | 589 | bibliothèques PsyQ (547 reconnues ; le reste : appels BIOS, fonctions internes, petites fonctions sous le seuil de 24 octets) |
| `0x80048A78`-`0x800521E0` | 105 | **pilote son**, non reconnu (voir ci-dessous) |

L'éditeur de liens place le code du jeu d'abord, puis les bibliothèques : tout ce qui est au-delà
du premier objet reconnu (`0x80031258`) est compté comme bibliothèque.

### Le pilote son

La fin du programme (105 fonctions) n'est ni libspu ni libsnd, quelle que soit la version du kit
(même découpées fonction par fonction, les signatures ne trouvent que `SsGetNck`). 25 de ces fonctions
écrivent directement dans la puce son (pointeur `D_8008335C` vers les registres SPU), et le
gestionnaire de sons du jeu (`0x8002B54C`-`0x8002E4F8`, messages « Se Error », « BgmVabNo ») les
appelle. C'est un pilote son propre au jeu, ou une bibliothèque hors du kit standard. Il est classé
avec les bibliothèques (couche 2) : il pilote le matériel, il sera remplacé par le moteur son natif
de la phase 4, après étude de son format (banques et séquences propres au jeu, voir
`docs/DATA_FORMATS.md`).

## Bilan

`python3 tools/progress.py` compte maintenant par catégorie :

| | Fonctions |
|---|---|
| Code du jeu, à réécrire (phase 3) | **2 378** (programme 411, `GAME00` 1 264, `EVENT00` 204, `GAME01-03` 134, `PACK1` 365) |
| Bibliothèques, à remplacer par du code natif (phase 4) | **694** (547 reconnues par signature) |

Pour la phase 4, les sources C reconstituées de ces bibliothèques sont un projet public
([sozud/psy-q-decomp](https://github.com/sozud/psy-q-decomp)) : utiles pour comprendre leur
comportement exact, mais le but reste de remplacer leur accès au matériel par du code PC.
