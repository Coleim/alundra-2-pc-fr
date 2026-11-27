# Vérification 1:1

Comment on prouve qu'une fonction réécrite se comporte exactement comme l'originale. Ces règles
s'appliquent à chaque fonction de la phase 3 et à chaque module natif de la phase 4.

## Les outils

| Outil | Rôle |
|---|---|
| `tools/record_session.py` | enregistre une session de référence depuis la mise sous tension (format PSXRTI3 du framework : boutons par image, cartes mémoire, empreintes de l'état toutes les 60 images) |
| `tools/replay_check.py` | rejoue une session et rend le verdict du framework (`in_sync` / `diverged`) : RAM complète et cycle final identiques à l'enregistrement |
| `tools/replay_diff.py` + `src/a2_diag.c` | rejoue la même session deux fois (A = code d'origine, B = code réécrit) et compare **image de jeu par image de jeu** : RAM (par page de 4 Ko), journal GPU, VRAM, cycles |
| `A2_VERIFY=1` | contrôle au niveau fonction : l'originale tourne aussi, ses résultats sont comparés à chaque appel, et son coût en cycles est mesuré ; `cost_mismatches` compte les appels où la version réécrite facture un autre nombre de cycles que l'originale |
| `tools/validate_session.py` | rejoue une session dans trois configurations à la fois (code d'origine, tout le code traduit, version livrée) et compare chacune à l'origine |
| `A2_LIFT_VERIFY=f1,f2` | contrôle fantôme d'une fonction traduite (registres, RAM, cycles) |
| `A2_LIFT_COVERAGE=1` | liste les fonctions de jeu appelées qui ne sont pas en C (ni traduites, ni réécrites) |
| `tools/lift_bisect.py` | retrouve par dichotomie la fonction traduite qui change le résultat |
| `A2_DIAG_TTY=1`, `A2_DIAG_VWAIT=n` | sortie console du jeu (`puts`, `printf`) ; attentes de `VSync` qui finissent à moins de `n` tours de la limite |

Sessions de référence (non versionnées, elles contiennent les cartes mémoire), rejouées avec le
code d'origine : `in_sync`.

- `saves/replays/Alundra_2-boot-20261126T091826Z.psxrpl` : 34 000 images (14 644 images de jeu) :
  démarrage, écran titre, nouvelle partie, cinématique d'ouverture, premiers pas de Flint ;
- `saves/replays/Alundra_2-boot-20261127T082542Z.psxrpl` : 40 000 images (19 062 images de jeu) :
  la même ouverture, puis déplacements, attaques, sauts, et le menu principal et ses sous-menus,
  ouverts et parcourus à plusieurs reprises (`tools/explore_session.py`).

### Le traceur `src/a2_diag.c`

Inactif sauf si `A2_DIAG_TRACE` est défini. À chaque appel bloquant de `VSync` (libetc,
`0x8003B1C8`), donc une fois par image de jeu, il écrit un enregistrement :

- numéro d'image de jeu et cycle du processeur invité ;
- **journal GPU** : empreinte et nombre de tout ce que le jeu a envoyé au GPU pendant l'image :
  paquets des tables d'ordre (`DrawOTag`, `DrawOTagEnv`), environnements de dessin (`PutDrawEnv`),
  transferts VRAM (`LoadImage`, `StoreImage`, `MoveImage`, `ClearImage`). Les adresses RAM des
  liens de la table d'ordre ne sont pas prises en compte : seul le contenu des primitives compte,
  ce qui permettra aussi de comparer le futur moteur natif ;
- empreinte de toute la VRAM (l'image) ;
- empreinte de chaque page de 4 Ko de la RAM (l'état du jeu).

Les points d'accroche déclinent toujours l'appel : le code d'origine s'exécute sans changement et
aucun cycle n'est ajouté.

Options : `A2_DIAG_DUMP_AT=i,j` (RAM complète à ces images), `A2_DIAG_GPU_DUMP_AT=i` (journal GPU
lisible de l'image `i`, une ligne par primitive), `A2_DIAG_STOP=n` (arrêt après l'image `n`).

### Utilisation

    # verdict du framework, code réécrit en mode contrôle
    python3 tools/replay_check.py <session>.psxrpl --env A2_VERIFY=1 --env A2_ANALOG=0 --log run.log
    # comparaison originale / réécrite, avec détail de la première différence
    python3 tools/replay_diff.py <session>.psxrpl --a A2_OVERRIDES=0 --b A2_ANALOG=0 --bytes --gpu

`--isolate PORT` donne à chaque instance son port de débogage et son dossier de cartes mémoire,
pour lancer plusieurs vérifications en même temps.

## Règles pour une fonction réécrite

1. **Mêmes résultats** : valeur de retour, écritures en mémoire.
2. **Mêmes registres en sortie** que l'originale (`v0`, `v1`, `a0`-`a3` tels qu'elle les laisse) :
   un appelant compilé peut, en théorie, sauvegarder ces valeurs en mémoire.
3. **Même coût en temps** : la fonction réécrite facture au processeur invité les cycles du chemin
   que l'originale aurait pris : `A2_COST_SELF` et un `a2_charge()` par bloc d'origine traversé
   (coûts donnés par `tools/block_costs.py`), avant chaque appel et à la fin. Sinon les
   interruptions tombent ailleurs et des valeurs dérivées du temps changent. Pour le programme
   principal (code compilé par le framework), le compte est exact et vérifié par le contrôle
   fantôme (`cost_mismatches=0`). Pour les modules chargés (`GAME`, `EVENT`...), que l'original
   faisait tourner dans l'interpréteur (cache d'instructions), c'est une estimation, acceptable
   hors des interruptions (voir « Le bord de `VSync` »).
4. **Critères d'acceptation**
   - `A2_VERIFY=1` : 0 différence sur toute la session, et verdict `in_sync` (l'originale garde la main
     sur le temps, la session doit rester identique au bit près) ;
   - `replay_diff` original / réécrit : journal GPU et VRAM identiques à chaque image ; RAM identique à
     chaque image **sauf** les adresses dérivées du temps listées ci-dessous.

## Ce qui dépend du temps (et pourquoi on le tolère)

Le framework simule le processeur au cycle près (cache d'instructions, attentes de chargement). Une
fonction réécrite ne peut pas reproduire exactement le coût de l'originale dans tous les cas (le
cache d'instructions dépend de ce qui a tourné avant). Une petite dérive de cycles est donc normale
en mode mixte, et disparaîtra avec la console simulée (phase 4).

Zones dont la valeur dépend du temps et pas de la logique du jeu. Le traceur exclut d'office les
trois premières de l'empreinte de la RAM ; les autres sont des pages entières, à juger au cas par
cas (`A2_DIAG_IGNORE=adresse+longueur,...` les retire aussi) :

| Zone | Contenu | Pourquoi elle varie |
|---|---|---|
| `0x80000000-0x8000FFFF` | noyau (BIOS) | pile et état des interruptions |
| pile, sous `$sp` | zone libre de la pile : 16 Ko sous `$sp`, à partir de la limite de page en dessous | une fonction réécrite ne range pas ses registres aux mêmes places ; la limite de page évite qu'un écart de quelques octets de `$sp` entre deux rejeux déplace la fenêtre (une première fenêtre de 64 Ko cachait les tampons du flux CD à `0x801EF000`) |
| `0x8005B0D0-0x8005C07F` | pile des interruptions de libetc | idem, dans le gestionnaire d'interruptions |
| page `0x80082000` | tampons SIO de la manette (libpad) | octets reçus au moment où l'interruption tombe |
| page `0x8005A000` | variables de `VSync` (libetc) | compteurs de lignes et de retours verticaux |
| `0x800FA7F4`, `0x800FA7F8` | `VSync(1)` : lignes affichées depuis le dernier retour vertical, relevées à chaque image | affichées seulement par un compteur de performance de débogage (`func_80087E34`, si l'octet `0x8005D170` est non nul) |
| `0x8007F598`, `0x8007F59C` (page `0x8007F000`) | compteurs d'attente de libcd (`CD_sync`, `CD_ready`...) | nombre de tours de boucle en attendant le lecteur |
| `0x8007DBB8` (page `0x8007D000`) | ligne en cours de la sortie console (`_putchar`) | trace le dernier message, par exemple `VSync: timeout` |
| pages des tampons de chargement (`0x8014B000`, `0x80153000`...) | données en cours de copie ou de décompression | une image isolée : la copie n'en est pas au même point au moment du relevé |
| `0x80055538`, `0x8005B000` | compteurs avancés par les interruptions | une image isolée : relevés juste avant ou juste après l'interruption (écart de 1) |

### Le bord de `VSync`

`VSync(n)` (libetc) attend le retour vertical dans une boucle qui abandonne après
`n << 15` tours et affiche alors `VSync: timeout`. Dans le modèle de temps du framework, un tour
coûte une vingtaine de cycles et une image PAL environ 677 000 : une attente d'une image entière
finit **à 0 à 4 tours de la limite** (mesuré avec `A2_DIAG_VWAIT` pendant le démarrage). Le temps
passé dans les interruptions n'est pas compté dans ces tours : si le gestionnaire du retour
vertical (lecture des manettes, son, temps de jeu) devient un peu moins cher, l'attente déborde,
`VSync` rend la main un peu avant le retour vertical, et l'état du noyau change
(`ChangeClearPad(0)`, `ChangeClearRCnt(3, 0)`).

Conséquence : les fonctions réécrites qui tournent dans l'interruption du retour vertical
facturent **exactement** les cycles de l'originale, bloc par bloc (`tools/block_costs.py`), et le
contrôle fantôme le vérifie (`cost_mismatches=0`). Le code du jeu hors interruptions peut avoir
un coût approché : l'attente de `VSync` de chaque image l'absorbe. Ce bord disparaît en phase 4,
quand `VSync` devient natif.

## Résultats

### `func_800A09C0` (cap du joueur)

- Contrôle au niveau fonction : 266 appels, 0 différence (session jouée par le harnais).
- Premier rejeu de la session de référence, avec un coût fixe de 40 cycles par appel : verdict
  `diverged`, une seule page de RAM différente. `replay_diff` montre la cause : à la première
  différence, l'horloge a 62 cycles d'écart et un seul mot diffère, `0x800FA7F4` (`0xE2` contre
  `0xE1`), soit le compteur de performance ci-dessus. La logique du jeu était identique.
- `A2_VERIFY=1` sur toute la session de référence : 7 857 appels, 0 différence, verdict `in_sync`.
  Coût mesuré de l'originale : 102 cycles à chaque appel (chemin « caméra », le seul de la session).
- Avec un coût de 23 cycles (nombre d'instructions seulement) : journal GPU et VRAM **identiques
  sur les 14 644 images de jeu** ; la RAM diffère seulement dans des zones liées au temps : tampons
  SIO de la manette (`0x80082000`), zone du noyau (`0x80009000`), pile (`0x801FF000`), compteur de
  performance (`0x800FA000`), variables de `VSync` (`0x8005A000`). Écart d'horloge entre -106 et
  +19 cycles dans une image, nul en fin de session : il ne s'accumule pas, chaque image repart du
  retour vertical.
- **Version finale** (coût de 102 cycles sur le chemin « caméra », mêmes registres en sortie que
  l'originale) : rejeu de la session de référence avec la fonction réécrite seule, verdict
  **`in_sync`** : RAM, empreintes de l'état toutes les 60 images et cycle final identiques au bit près
  à l'original sur 34 000 images. `replay_diff` confirme : RAM, journal GPU, VRAM et cycles
  identiques sur les 14 644 images de jeu.

## Phase 3 : tout le code du jeu en C

Configurations comparées au code d'origine par `tools/validate_session.py` :

- **traduit** : toutes les fonctions du jeu dans leur version traduite par `tools/a2lift.py`
  (les versions réécrites à la main sont coupées) ;
- **livré** : la version réécrite à la main là où elle existe, traduite ailleurs.

Résultats sur les deux sessions de référence (14 644 et 19 062 images de jeu), pour les deux
configurations :

- **journal GPU et VRAM identiques à chaque image** ;
- **aucune fonction du jeu exécutée hors du C** (`A2_LIFT_COVERAGE` : 0 cible) ;
- RAM identique à chaque image hors des zones de temps ci-dessus ;
- écart d'horloge entre -485 et +378 cycles dans une image pour le code traduit, quelques cycles
  en fin de session : il ne s'accumule pas. Il vient des appels imbriqués du code traduit (le
  framework vérifie les interruptions à d'autres endroits), pas de la logique du jeu. Les
  fonctions réécrites du programme principal facturent exactement les cycles de l'originale ;
  celles de `GAME00` (cadence, caméra) une estimation : la configuration livrée reste entre -385
  et +835 cycles.

Fonctions réécrites à la main, contrôle fantôme sur toute la session de référence : 0 différence
de résultat ; `cost_mismatches=0` pour celles du programme principal (lecture des manettes, temps
de jeu...).
