# Réécrire une fonction du jeu (phase 3)

Deux voies, qui coexistent :

1. **Traduction automatique exacte** (`tools/a2lift.py`) : chaque fonction du jeu est traduite du
   MIPS vers du C, instruction par instruction, dans `src/game/lifted/`. Le C garde la sémantique
   exacte de l'original (registres, mémoire, chargements différés, multiplication/division, GTE)
   et le modèle de temps du framework, pour que les rejeux restent identiques au bit près. C'est
   ce qui met **tout** le code du jeu en C.
2. **Réécriture à la main** (`src/game/*.c`, `A2_REWRITE`) : version lisible d'une fonction,
   nommée et commentée, contrôlée contre l'originale. Elle remplace la version traduite.

Méthode suivie pour chaque fonction du jeu réécrite en C. Le jeu reste jouable à chaque étape :
la fonction réécrite remplace l'originale par le mécanisme `func_override` du framework, et chaque
remplacement peut être désactivé.

## Traduction automatique (`tools/a2lift.py`)

    python3 tools/a2lift.py            # régénère src/game/lifted/ (tous les modules)

- Une fonction C par fonction du jeu, avec les registres nommés comme en assembleur (`v0`, `a0`,
  `s0`…), les adresses d'origine en commentaire et les symboles (`D_8006B208`).
- Temps : le programme principal suit le modèle du code compilé du framework (une charge par bloc,
  `A2_SPAN`) ; les modules chargés (`GAME`, `EVENT`, `PACK1`, `AL22`) suivent celui de
  l'interpréteur qui les exécutait (coût par instruction : cache d'instructions, verrouillages de
  chargement). Une vérification d'interruption a lieu à chaque transfert de contrôle (`A2_EDGE`),
  comme dans le code compilé.
- Appels : par le répartiteur (`a2_jal`), donc vers la version originale, réécrite à la main ou
  traduite de l'appelé.
- Retard de chargement : la valeur d'un chargement n'est visible qu'une instruction plus tard ;
  quand le cas ne se règle pas dans le bloc (chargement dans le délai d'un branchement dont une
  destination lit le registre), la valeur passe par la file de chargement du framework
  (`psx_load_value_arm`, puis `A2_SYNC` aux destinations).
- Modules superposés (`GAME00-03`/`EVENT00` à `0x80084ED0`, modules de `PACK1` à `0x80149AE8`,
  bibliothèques `AL22` à `0x80010000`) : un seul remplacement par adresse, qui exécute la version
  traduite présente en mémoire. Chacune vérifie ses 4 premiers mots, plus, si une autre version à
  la même adresse commence pareil, jusqu'à 4 mots plus loin qui les distinguent.
- Code assembleur écrit à la main (`AL22`) : des routines sautent dans le corps d'autres routines
  et en reviennent. Les fonctions liées par de tels sauts (composantes fortement connexes du graphe
  des sauts) forment une seule unité C, avec une entrée par adresse d'appel ; un saut dans une
  fonction ajoute une entrée à son unité.
- Fonctions atteintes seulement par pointeur (tables de modes, rappels, points d'entrée des
  modules) : `splat` les collait à la fonction précédente ; `tools/find_hidden_functions.py` les
  retrouve (le code qu'une fonction n'atteint pas depuis son entrée, après sa dernière instruction
  atteinte) et les ajoute aux symboles (77 fonctions). Un mot de données qui pointe dans une
  fonction (hors tables de sauts) devient aussi une entrée.
- Les fonctions réécrites à la main gardent leur version traduite, enregistrée seulement quand la
  version à la main est coupée (`A2_DISABLE`) : on peut comparer les deux.
- `A2_LIFTED=0` coupe toutes les fonctions traduites ; `A2_LIFTED=main` garde celles du programme
  principal ; `A2_DISABLE=f1,f2` en coupe certaines ; `A2_LIFT_VERIFY=f1` contrôle une fonction
  traduite contre l'originale ; `A2_LIFT_COVERAGE=1` liste les fonctions du jeu appelées qui ne
  sont pas en C.
- Bibliothèque Sony exclue (phase 4).

## Outils

| Outil | Rôle |
|---|---|
| `tools/decompile.py func_XXXXXXXX` | brouillon C avec m2c (`pip install git+https://github.com/matt-kempster/m2c.git` dans `.venv-decomp`), à partir de l'assembleur splat et des tables de sauts du module |
| `src/a2_rewrite.h` / `.c` | couche commune : accès mémoire (`a2_lw`, `a2_sh`…), appels (`a2_call0..4`), enregistrement (`A2_REWRITE`, `A2_REWRITE_OVERLAY`), contrôle fantôme, coût en cycles |
| `tools/check_calls.py` | vérifie que chaque appel écrit en C correspond à un `jal` de l'original (adresse et cible), et que chaque fonction réécrite commence bien à un début de fonction |
| `A2_VERIFY=f1,f2` | contrôle fantôme (fonctions `A2_PURE`) : l'originale s'exécute pour de vrai, la version réécrite rejoue le même appel, résultats et écritures en RAM sont comparés, l'état de l'originale est gardé ; le coût en cycles de l'originale est mesuré |
| `tools/replay_check.py`, `tools/replay_diff.py` | rejeu de la session de référence (voir `docs/VERIFICATION.md`) |
| `tools/progress.py` | fonctions du jeu réécrites à la main / traduites |
| `tools/block_costs.py [--module m] func_X` | coût en cycles de chaque bloc de l'originale, pour facturer le chemin suivi |
| `tools/callgraph.py callers|callees|tree` | qui appelle quoi (appels directs) |

## Étapes

1. **Brouillon** : `python3 tools/decompile.py func_XXXXXXXX` ; relire l'assembleur (`decomp/asm/...`)
   pour les détails que m2c rend mal (tables de sauts, extensions de signe, variables locales).
2. **Écrire la fonction** dans `src/game/<sous-système>.c`, nommée, commentée (rôle, structures) :
   - arguments dans `cpu->gpr[4..7]`, résultat dans `cpu->gpr[2]`, `return 1` ;
   - mémoire du jeu uniquement par `a2_lw`/`a2_lh`/`a2_lhu`/`a2_lb`/`a2_lbu`/`a2_sw`/`a2_sh`/`a2_sb` ;
   - appels uniquement par `a2_callN(cpu, adresse_du_jal_d_origine, cible, ...)` : l'appelé peut être
     original ou réécrit, le répartiteur choisit ;
   - une attente active de l'original (sur un drapeau changé par une interruption) n'est pas
     reproduite en C : la fonction réécrite rend la main à l'originale (`return 0`) dans ce cas ;
   - les variables locales que l'original lit sans les avoir écrites (pile) sont lues au même
     endroit de la pile invitée ;
   - drapeaux : `A2_PURE` (RAM seulement, contrôle fantôme possible), `A2_VOID` (pas de résultat).
3. **Contrôle statique** : `python3 tools/check_calls.py` (0 problème).
4. **Contrôle fantôme**, par niveau d'appel : pendant le contrôle d'une fonction, ses appelés
   s'exécutent dans leur version originale ; une fonction appelée seulement par d'autres fonctions
   réécrites se contrôle donc dans une passe à part, ses appelants tournant en version réécrite.
   Critère : `mismatches=0` sur toute la session.
5. **Coût** : la fonction facture les blocs de l'originale qu'elle traverse (`a2_charge`, valeurs
   de `tools/block_costs.py`), avant chaque appel et à la fin (`A2_COST_SELF`). Le contrôle fantôme
   compare à chaque appel ce coût au coût propre mesuré de l'originale (`cost_mismatches`). Un coût
   fixe (dernier argument de `A2_REWRITE`) convient si la fonction n'a qu'un chemin. Pour le code
   des modules chargés, que l'original exécutait dans l'interpréteur, `a2_charge_interp` applique
   au compte des blocs le rapport mesuré sur `func_8008B044` (environ 3, à cause du cache
   d'instructions) : c'est une estimation.
6. **Scène** : rejeu de la session de référence avec toutes les fonctions réécrites ; puis
   `replay_diff` original / réécrit : journal GPU, VRAM et RAM (hors zones de bruit) identiques.

## Lot 1 : couche manette (programme principal)

| Fonction | Rôle | Contrôle |
|---|---|---|
| `func_8001BF70` | enregistrement d'entrée du jeu à partir d'un enregistrement de manette | |
| `func_8001C038` / `func_8001C0A0` | reconstruit les deux enregistrements, avec ou sans les fronts de l'image | 13 845 / 8 appels, 0 différence |
| `func_8002AD6C` / `func_8002ADB4` | fige / efface les fronts des deux manettes | |
| `func_8002A5E0` | active ou coupe la manette | 5 appels, 0 différence |
| `func_8001BF68` | fonction vide | |
| `func_8002A47C` | mise à jour des manettes à chaque retour vertical | |
| `func_8002A5F0` | lecture libpad, détection pas à pas, boutons, sticks, répétition, double appui | |
| `func_8002AFB0` | vibrations : motifs, niveaux des moteurs | |
| `func_8001C230` | sticks analogiques vers boutons | |
| `func_8002ADF0` | vibration autorisée (toujours oui) | |
| `func_8002AE74` | arrêt d'un bloc de vibration | |
| `func_8002A4D4` / `func_8001BEA4` | démarrage de libpad et de la couche d'entrée | |

## Lot 2 : système (programme principal, `src/game/system.c`)

| Fonction | Rôle |
|---|---|
| `func_8001A148` | rappel du retour vertical : manettes, son, temps de jeu, indicateur de chargement |
| `func_8001A028` | indicateur de chargement (animation de 10 images en VRAM) |
| `func_80022B4C` | temps de jeu (3 000 images par minute, arrêt à 99:59) |

## Lot 3 : cadence et listes d'affichage (`GAME00`, `src/game/frame.c`)

| Fonction | Rôle |
|---|---|
| `func_8008A390` | fin d'image : fermeture des listes, flux CD, nombre aléatoire de l'image, attente du GPU, palettes en attente, `VSync(mode)`, échange des tampons, envoi de l'image au GPU ; pause image par image sur la manette 2 (R1 / R2) |
| `func_8008A330` | échange des deux tampons d'affichage |
| `func_8008A228` | mise en place des tampons d'affichage (320x232, réglage de position de l'écran) |
| `func_8008A60C` / `func_8008A660` | tables d'ordre : deux jeux, six couches chaînées de l'arrière vers l'avant (fond, 3D triée en profondeur sur 2 049 niveaux, effets, interface) |
| `func_8008A774` | envoi des tables au GPU, échange des jeux de tables et des tampons de paquets |

Le jeu en exploration demande une image tous les deux retours verticaux (`VSync(2)`, variable
`0x8005D218` passée par la boucle principale de `GAME00`, `func_80089AE8`) : 25 images par
seconde en PAL.

## Lot 4 : caméra (`GAME00`, `src/game/camera.c`)

| Fonction | Rôle |
|---|---|
| `func_8008BE90` | mise à jour de la caméra : gestionnaire du mode courant (table `0x800EEB10`) |
| `func_8008BDF8` / `func_8008BE44` | modes de suivi : suivi, puis vue reconstruite si la caméra a bougé |
| `func_8008B044` | suivi : zone morte et rattrapage, rail (approche progressive ou bornée), orbite autour de la cible, déplacement de l'œil vers une seconde cible, zoom |

Structure de la caméra (`0x8011BAF8`) : voir l'en-tête de `src/game/camera.c`.
