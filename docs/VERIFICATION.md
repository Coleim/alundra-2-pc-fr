# Vérification 1:1

Comment on prouve qu'une fonction réécrite se comporte exactement comme l'originale. Ces règles
s'appliquent à chaque fonction de la phase 3 et à chaque module natif de la phase 4.

## Les outils

| Outil | Rôle |
|---|---|
| `tools/record_session.py` | enregistre une session de référence depuis la mise sous tension (format PSXRTI3 du framework : boutons par image, cartes mémoire, empreintes de l'état toutes les 60 images) |
| `tools/replay_check.py` | rejoue une session et rend le verdict du framework (`in_sync` / `diverged`) : RAM complète et cycle final identiques à l'enregistrement |
| `tools/replay_diff.py` + `src/a2_diag.c` | rejoue la même session deux fois (A = code d'origine, B = code réécrit) et compare **image de jeu par image de jeu** : RAM (par page de 4 Ko), journal GPU, VRAM, cycles |
| `A2_VERIFY=1` | contrôle au niveau fonction : l'originale tourne aussi, ses résultats sont comparés à chaque appel, et son coût en cycles est mesuré |

Session de référence (non versionnée, elle contient les cartes mémoire) :
`saves/replays/Alundra_2-boot-20261126T091826Z.psxrpl`, 34 000 images (démarrage, écran titre,
nouvelle partie, cinématique d'ouverture, premiers pas de Flint). Rejouée avec le code d'origine :
`in_sync`.

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
3. **Même coût en temps** : la fonction réécrite facture au processeur invité le nombre de cycles du
   chemin que l'originale aurait pris (`FO_CREDIT_SELF` + `psx_advance_cycles`). Sinon les
   interruptions tombent ailleurs et des valeurs dérivées du temps changent.
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

Adresses connues dont la valeur dépend du temps et pas de la logique du jeu :

| Adresse | Contenu | Utilisation |
|---|---|---|
| `0x800FA7F4` | `VSync(1)` : nombre de lignes affichées depuis le dernier retour vertical, relevé à chaque image | affiché seulement par un compteur de performance de débogage (`func_80087E34`, si l'octet `0x8005D170` est non nul) |

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
