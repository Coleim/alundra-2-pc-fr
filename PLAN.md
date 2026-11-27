# PLAN — Alundra 2 (version française, SLES-02601) : vrai portage PC

Dernière mise à jour : 26 novembre 2026 (fin de la phase 2).
Suivi chiffré : `python tools/progress.py` · détails techniques : `docs/DECOMP_NOTES.md` · journal : `docs/PROGRESS.md`.

Légende : `[x]` fait · `[~]` en cours ou partiel · `[ ]` à faire.
Tailles : **S** quelques heures · **M** quelques jours · **L** quelques semaines · **XL** plusieurs mois.

---

## 0. Objectif et règles

**Objectif final :** un exécutable Windows (et macOS) compilé à partir de **vrai code source C**, sans console simulée, qui joue Alundra 2 **à l'identique de l'original (1:1)**. Les graphismes, sons, textes et vidéos viennent du disque du joueur, importé au premier lancement. Aucun contenu du jeu n'est distribué.

**Ordre de priorité**
1. **Release 1.0 = 1:1 sur PC, rien d'amélioré.** Même rythme (50 Hz PAL), mêmes contrôles, même image (4:3), mêmes sauvegardes.
2. Améliorations **après** la 1.0 : contrôles, écran large, fps, définition, etc. (phase 7).

**Règles de travail**
- Aucune amélioration ne s'ajoute avant que le 1:1 soit validé. Une amélioration reste un réglage désactivé par défaut.
- Chaque fonction réécrite est **vérifiée contre l'originale** avant d'être gardée (voir « Vérification »).
- Rien de commercial dans Git : ni disque, ni BIOS Sony, ni fichiers extraits (`extracted/`, `decomp/` restent ignorés).
- Commits locaux fréquents ; `push` seulement sur demande.
- Machine de test Windows : Dell G15 5511 (portable de jeu, deux puces graphiques : forcer la RTX).

---

## 1. Architecture visée

Trois couches, qui deviennent réelles à mesure que le jeu est réécrit :

```
┌──────────────────────────────────────────────┐
│ Couche 3 : jeu (C réécrit)                   │  logique, entités, scripts, menus, caméra
├──────────────────────────────────────────────┤
│ Couche 2 : bibliothèque PsyQ native          │  GPU (listes d'ordre), GTE, CD, manette,
│  (équivalent de PsyCross)                    │  cartes mémoire, son (VAB/SEQ), vidéo (STR)
├──────────────────────────────────────────────┤
│ Couche 1 : plateforme                        │  SDL3, OpenGL, audio, fichiers, saisie
└──────────────────────────────────────────────┘
```

**Stratégie de transition (mode mixte).** Aujourd'hui le jeu tourne dans la recompilation (PSXRecomp, la « v1 »). On y remplace les fonctions **une par une** par du C écrit à la main (`func_override`), tout en gardant la mémoire du jeu au même format que l'original. Le jeu reste jouable à chaque étape. Quand tout est réécrit, on retire la recompilation et la console simulée : il reste les couches 1 à 3.

**Décision d'architecture à trancher au début de la phase 4 :** pointeurs 32 bits du jeu d'origine (mémoire de 2 Mo à adresses fixes) ou pointeurs natifs 64 bits. Recommandation : garder les adresses d'origine tant que le mode mixte existe, migrer vers des structures natives une fois la réécriture terminée.

---

## 2. Vérification 1:1 (le critère de « terminé »)

| Niveau | Quoi | Outil |
|---|---|---|
| Fonction | l'original et la version réécrite donnent le même résultat sur les mêmes appels, et le même coût en cycles | mode `A2_VERIFY=1` (exécution en parallèle + compteurs + mesure des cycles) |
| Scène | mêmes boutons → même état du jeu (hash de la mémoire à des points de contrôle) | rejeu d'une session de référence : `tools/replay_check.py` (verdict `in_sync`) |
| Image | mêmes primitives graphiques envoyées au GPU par image, même VRAM | journal GPU de `src/a2_diag.c`, comparé image par image par `tools/replay_diff.py` |
| Son | mêmes écritures de la puce son et mêmes événements musicaux | journal SPU/SEQ |
| Jeu complet | partie du début à la fin, sauvegardes compatibles | parcours complet + sauvegardes de l'original chargées dans le port |

---

## 3. Phases jusqu'à la release 1.0 (1:1)

### Phase 0 — Base : recompilation « v1 » (usage perso)  `[x]` terminé
- [x] Recompilation du jeu français (SLES-02601) avec PSXRecomp, OpenBIOS (aucun BIOS Sony requis).
- [x] Démarre jusqu'au gameplay : logos, intro, écran titre en français, nouvelle partie, première zone.
- [x] `Alundra 2.app` (Mac, autonome) et zip Windows (autonome, disque inclus) — **usage personnel seulement, jamais publiés**.
- [x] Dépôt Git `Coleim/alundra-2-pc-fr`, identité Gmail, `main` poussé jusqu'aux scripts d'emballage.
- [~] Test sur le Dell G15 : saccades constatées (image et surtout son). Causes probables : interpréteur pour le code chargé depuis le CD, fichiers de capture, rendu 4x sur puce intégrée. **Abandonné au profit du vrai portage** ; sera résolu par la phase 4.

### Phase 1 — Cartographie et outillage  `[x]` terminée  (M)
- [x] Extraction complète du disque (`tools/extract_disc.py`), archives `PAK`/`GP` découpées.
- [x] Carte des chargements : programme principal `0x80017000` ; `GAME00`/`EVENT00`/`GAME01-03` `0x80084ED0` (modules exclusifs) ; 17 modules de code de `PACK1.PAK` à `0x80149AE8` ; carte des lectures CD d'une partie (`symbols/cd_loads.json`).
- [x] Découpage `splat` : programme principal, `GAME00-03`, `EVENT00`, modules de `PACK1` — 3 070 fonctions.
- [x] Noms automatiques : 38 appels BIOS, 17 fonctions PsyQ (via messages de debug), libpad, points d'entrée libgpu (`DrawOTag`, `LoadImage`…), `VSync`.
- [x] Chaîne de suivi de progression (`tools/progress.py`).
- [x] Harnais de test pilotable (`run_scenario.py`, `runtime_ctl.py`, `play_to_gameplay.py`, détection de blocage).
- [x] Chaîne de la manette comprise (tampons, structures, angle de direction, cap).
- [x] Mécanisme de remplacement de fonction compris (`func_override`, limite de 128 par défaut).
- [x] `GAME01.BIN`, `GAME02.BIN`, `GAME03.BIN` : adresse de chargement et configuration `splat` (S).
- [x] Code caché dans les `PAK` (lecteur vidéo vers `0x8014A000`, autres ?) : l'inventorier et le découper (M).
- [x] Inventaire des formats de données : tables des `PAK`/`GP`, textures (TIM/maison), modèles, cartes, animations, scripts de cinématique, sons VAB/SEQ, textes (M à L).
- [x] Rejeu d'entrées : enregistrer des parcours de référence (boutons par image) avec points de contrôle (M).
- [x] Journal des commandes GPU par image, pour comparer le rendu (M) : `src/a2_diag.c` (tables d'ordre, environnements de dessin, transferts VRAM, empreinte de la VRAM, RAM par page, cycles) et `tools/replay_diff.py` ; méthode dans `docs/VERIFICATION.md`.
- [x] Blocage intermittent du harnais en mode accéléré (S–M) : détection complète (plus d'image qui avance **ou** serveur de débogage muet), piles des threads relevées, relance automatique ; instances parallèles isolées ; `tools/stall_hunt.py` ; 13 sessions de suite sans blocage. Les vérifications 1:1 passent par le rejeu, sans le harnais accéléré. Cause probable : **mise en veille du Mac** (le dernier relevé montre 32 s de temps réel pour 10 ms de temps de jeu : processus gelé, pas bloqué) ; les tâches longues tournent désormais sous `caffeinate`.

### Phase 2 — Bibliothèque PsyQ : identifier et nommer  `[x]` terminée  (L)
Les fonctions de la bibliothèque Sony sont **déjà écrites publiquement** (projet de décompilation PsyQ) : on les reconnaît, on les nomme, on ne les réécrit pas à la main. Détails : `docs/LIBRARIES.md`.
- [x] Reconnaissance par signatures (`tools/psyq_sigs.py`, signatures publiques lab313ru, clonées hors du dépôt) : **149 fichiers objets, 547 fonctions, 414 noms** en une vingtaine de secondes ; kit **PsyQ 4.5** environ.
- [x] Bibliothèques nommées : libc2, libpress, libgpu, libgte, libcd, libds, libetc, libapi, libmath, libsn, libmcrd, libcard, libpad (+ `SsGetNck` de libsnd). Pas de libgs ni de libspu standard. Contrôle : 24 noms existants confirmés, 2 noms devinés corrigés.
- [x] Séparation « bibliothèque » / « code du jeu » (`tools/progress.py`) : **2 378 fonctions de jeu à réécrire** (phase 3, 2 707 une fois les fonctions cachées et les bibliothèques `AL22` trouvées), **694 fonctions de bibliothèque à remplacer** (phase 4). Aucune bibliothèque dans les modules `GAME`/`EVENT`/`PACK1` : tout y est du code du jeu.
- [x] Découverte : la fin du programme (105 fonctions, `0x80048A78`-`0x800521E0`) est un **pilote son propre au jeu** (accès direct à la puce son), ni libspu ni libsnd : à étudier avec le moteur son de la phase 4.

### Phase 3 — Le jeu en C (mode mixte)  `[x]` terminée  (XL)
Définition retenue (« A ») : **tout le code du jeu en C, exact et vérifié** — traduit
automatiquement du MIPS, plus des réécritures lisibles des systèmes utiles aux améliorations de la
phase 7 ; le reste devient lisible au fil de l'eau, en parallèle de la phase 4.
- [x] 1re fonction : `func_800A09C0` (cap du joueur) — 7 857 appels comparés, 0 différence ; **rejeu de la session de référence `in_sync` au bit près** avec la fonction réécrite seule (mêmes registres, même coût en cycles) ; version analogique vérifiée à 11 angles *(l'extension analogique appartient à la phase 7, voir plus bas : elle reste désactivée dans la 1.0)*.
- [x] Outils : brouillons C par décompilateur (`tools/decompile.py`, m2c), couche commune `src/a2_rewrite.h` (accès mémoire, appels, contrôle fantôme original/réécrit à chaque appel, coût en cycles par bloc avec `tools/block_costs.py`), vérification statique des appels (`tools/check_calls.py`), graphe d'appels (`tools/callgraph.py`), méthode dans `docs/REWRITING.md`.
- [x] Limite de 128 remplacements levée : 4 096, recherche par table de hachage (patch du framework appliqué à la compilation, `patches/psxrecomp/`) ; rejeu de non-régression identique au bit près.
- [x] **Tout le code du jeu en C : 2 707 fonctions** — 2 678 traduites par `tools/a2lift.py` (`src/game/lifted/`), 29 réécrites à la main. Le compte a grandi pendant la traduction : 77 fonctions que le découpage n'avait pas vues (atteintes seulement par pointeur, dont les points d'entrée de `GAME00` et `EVENT00`), et 4 bibliothèques en assembleur chargées à `0x80010000` (`AL22`, `PACK0.PAK` entrées 106-109, 256 fonctions). Détails dans `docs/REWRITING.md`.
- [x] **Vérification** sur deux sessions (14 644 et 19 062 images de jeu ; la seconde parcourt combats et menus) : journal GPU et VRAM **identiques à chaque image**, aucune fonction du jeu exécutée hors du C (`A2_LIFT_COVERAGE`), RAM identique hors des zones de temps documentées (`docs/VERIFICATION.md`).
- [x] **Lot 1 — couche manette** (15 fonctions, `src/game/input.c`) : contrôlées sur toute la session, **0 différence** ; coûts en cycles exacts, chemin par chemin.
- [x] **Lot 2 — système** (`src/game/system.c`) : routine du retour vertical, indicateur de chargement, temps de jeu ; coûts exacts (ils tournent dans l'interruption : voir « le bord de `VSync` » dans `docs/VERIFICATION.md`).
- [x] **Lot 3 — cadence et listes d'affichage** (`GAME00`, `src/game/frame.c`) : fin d'image, `VSync(2)` (25 images/s), double tampon, tables d'ordre et tampons de paquets.
- [x] **Lot 4 — caméra** (`GAME00`, `src/game/camera.c`) : modes, suivi, orbite, rail, zoom.
- [x] Ce qui est compris du moteur (modules en mémoire, image de jeu, cadence, tables d'ordre, caméra, projection) : `docs/ENGINE.md`.
- [ ] À rendre lisible au fil de l'eau (phase 4 en parallèle) : chargeur de fichiers (`func_8001BD5C`, `func_8001BDB0`, `func_8001BCA4`), boucle principale (`main` = `func_8001A99C`, `GAME00` = `func_80089AE8`), projection (`func_8002569C`, `func_80025A9C`, `func_80025B90`), entités et collisions (table `0x8011DCE0`, `func_800A71FC`), rendu des modèles et décors, scripts de cinématique (`EVENT00`), menus, son, mini-jeux.
- **Jalon atteint :** 100 % des fonctions du jeu en C, mode mixte encore actif pour la bibliothèque PsyQ et la console simulée.

### Phase 4 — Couche PsyQ native et plateforme  `[x]` terminée  (XL)
La console simulée est remplacée par un moteur PC autonome, `src/native/` (cible `alundra2`, voir `docs/NATIVE.md`). Le code du jeu (traduit et réécrit en phase 3) et la bibliothèque PsyQ traduite tournent dessus ; il n'y a plus ni PSXRecomp ni OpenBIOS dans le produit.
- [x] **Fichiers :** lecteur de CD natif (registres d'origine, libcd/libds/streaming inchangés) qui lit les fichiers copiés du disque du joueur. Les **269 215 secteurs** relus sont identiques octet pour octet à l'image (`alundra2 --verify-disc`). Chargements bien plus rapides que la console (secteurs de données au rythme du jeu, flux XA/vidéo au rythme d'origine).
- [x] **Entrées :** manette et clavier via SDL3, vus par le jeu comme une DualShock analogique (sticks, vibrations), au format libpad d'origine.
- [x] **Cartes mémoire :** images de 128 Ko au format standard des cartes PlayStation (`.mcd`, compatibles émulateurs) ; libmcrd native ; chargement vérifié dans le jeu, création/écriture/lecture/suppression vérifiées (`--self-test`). Sauvegarde en cours de partie réelle : à confirmer en phase 6.
- [x] **GTE :** en logiciel, **bit-exact** (69 445 270 commandes comparées au framework, 0 écart).
- [~] **GPU :** rasteriseur logiciel (VRAM 1024×512, polygones, sprites, textures 4/8/15 bits, transparences, masque, dither), image présentée par SDL3. Rejeu d'images capturées dans le framework (`tools/gpucheck`) : logos et vidéo **identiques** ; scènes 3D et menus ~94 % des pixels de VRAM identiques, le reste à ±1 pas de couleur (arrondis d'interpolation). À affiner pour le 1:1 strict.
- [x] **Son :** puce sonore native (24 voix ADPCM, enveloppes, modulation, bruit, réverbération, entrée CD) pilotée par le pilote son du jeu lui-même ; musiques XA décodées. Interpolation cubique au lieu de la table de la console : même rendu à l'oreille, pas au bit près.
- [x] **Vidéo :** MDEC natif au niveau registres (libpress inchangée) : intro `A2STR.STR` et films.
- [x] **Temps :** retour vertical à 50 Hz (PAL) sur l'horloge du PC, interruptions coopératives ; mode déterministe (`A2_DETERMINISTIC=1`) pour les tests. Reprise sur erreur disque (`longjmp`) vérifiée.
- [x] **Performance :** ~15 % d'un cœur sur Mac (Apple Silicon) à 50 Hz ; test sur le Dell G15 à faire par l'utilisateur.
- [x] PSXRecomp et OpenBIOS retirés du produit : `alundra2` se construit avec `-DA2_FRAMEWORK=OFF`.
- **Jalon atteint :** le jeu tourne sans la console simulée (logos, film d'intro, titre, nouvelle partie, cinématiques, jeu, menus, cartes mémoire).

### Phase 5 — Importation du disque et empaquetage  `[x]` terminée  (M)
- [x] Premier lancement : l'utilisateur choisit son `.cue`/`.bin` (fenêtre SDL et boîte de dialogue du système) ; vérification de l'exécutable français (empreinte de `SLES_026.01`) ; copie des fichiers avec barre de progression vers le dossier de données de l'utilisateur. `.chd` : non pris en charge.
- [x] Versions de disque : français SLES-02601 seulement.
- [x] Windows x64 : `alundra2.exe` autonome (compilation croisée MinGW, SDL3 statique), demande de la carte graphique performante.
- [x] macOS : `Alundra 2.app` universelle (Apple Silicon et Intel), signée localement.
- [x] Construction automatisée (CI, `.github/workflows/native.yml`) : moteur sur Linux, macOS et Windows, sans aucune donnée du jeu ; les paquets complets se font en local (`scripts/package_native.sh`).
- [x] Notice d'installation en français : `docs/INSTALL_FR.md` (jointe aux paquets).

### Phase 6 — Validation 1:1 et release 1.0  `[ ]`  (L)
- [ ] Parcours complet du jeu, du début à la fin, sans bloquant ; comparaison avec l'original sur des points de contrôle.
- [ ] Sauvegardes de l'original chargées dans le port, et sauvegardes du port relues par l'original.
- [ ] Toutes les cinématiques et vidéos, tous les mini-jeux, tous les menus.
- [ ] Tests sur Dell G15 (Windows), Mac Apple Silicon.
- [ ] Liste de défauts ouverts à zéro bloquant.
- [ ] Licence, mentions, README ; décision de visibilité du dépôt (public ou privé).
- **Release 1.0 (1:1) :** exécutables Windows et macOS, aucun contenu du jeu inclus.

---

## 4. Après la 1.0 — améliorations (phase 7)

Aucune n'entre avant la 1.0. Chacune est un **réglage**, désactivé par défaut, et ne casse pas le 1:1 quand elle est coupée.

| Amélioration | Prérequis | Taille | État |
|---|---|---|---|
| **Contrôles analogiques 360°** (cap exact du stick) | couche entrées + caméra réécrites | M | `[~]` prototype vérifié (cap), mouvement réel à confirmer |
| Vitesse proportionnelle à l'inclinaison du stick | entités du joueur réécrites | M | `[ ]` |
| **Caméra au stick droit** | caméra réécrite | M | `[ ]` |
| **Écran large (16:9, 21:9)** sans objets qui apparaissent sur les bords | rendu et caméra réécrits | L | `[ ]` |
| Interface recadrée pour l'écran large | idem | M | `[ ]` |
| **Définition** (rendu interne 2x–8x, correction des polygones qui tremblent) | GPU natif | M | `[ ]` |
| **60 images/s** (ou plus) | physique et animations réécrites, minuteries identifiées | XL | `[ ]` |
| Interpolation d'images (alternative au vrai 60 fps) | GPU natif | M | `[ ]` |
| Textures HD (IA) | formats de textures décodés, chargeur de textures | L | `[ ]` |
| Vidéo d'intro HD | décodeur vidéo | M | `[ ]` |
| Chargements instantanés, sauvegarde rapide | fichiers natifs | S | `[ ]` |
| Remappage des touches, options modernes | entrées natives | S | `[ ]` |
| Support d'autres versions (US/JP/DE…) | tout le reste | L | `[ ]` |

---

## 5. Risques connus

| Risque | Effet | Parade |
|---|---|---|
| Volume de la réécriture (2 707 fonctions) | durée de plusieurs mois | **traduction automatique exacte de tout le code** (fait), puis réécriture lisible par lots, en commençant par ce dont la phase 7 a besoin |
| Temps de la console simulée : `VSync` à la limite de son délai | une fonction un peu moins chère dans l'interruption fait déborder l'attente | coûts exacts pour le code des interruptions ; disparaît avec `VSync` natif (phase 4) |
| Code compilé de l'époque (compilateur PsyQ) difficile à relire | fonctions lentes à comprendre | décompilateur MIPS, comparaison automatique, tests d'équivalence |
| Limite de 128 remplacements simultanés | bloque le mode mixte au-delà | **résolu** : patch du framework (4 096, table de hachage) |
| Précision du son (réverbération, séquences) | écart audible avec l'original | journaux SPU/SEQ comparés à l'original |
| Blocage intermittent du harnais (cartes mémoire) | tests peu fiables | détection complète et relance ; vérifications par rejeu, sans le harnais ; cause non reproduite (13 sessions sans blocage) |
| Dépendance au Dell G15 pour le test Windows | boucle de test plus lente | compilation croisée depuis le Mac, zip à tester |
| Aspect juridique (code dérivé de celui d'Activision/Sony) | retrait possible d'un dépôt public | aucune donnée du jeu dans le dépôt ; visibilité à décider avant publication |

---

## 6. État actuel (résumé)

- **Terminé :** v1 jouable (Mac/Windows) ; **phase 1** : disque extrait, tout le code cartographié et découpé, formats inventoriés, harnais de test, outils de vérification 1:1 (rejeu, journal GPU, comparaison image par image) ; **phase 2** : bibliothèques Sony reconnues par signature, code du jeu et bibliothèques séparés ; **phase 3** : tout le code du jeu en C (2 707 fonctions), vérifié sur deux sessions de référence.
- **Phases 4 et 5 terminées :** moteur PC autonome (`src/native/`, cible `alundra2`) sans console simulée ; import du disque au premier lancement ; paquets Windows et macOS sans données du jeu (version 0.3.0).
- **Écarts connus au 1:1 :** GPU (~6 % des pixels à ±1 pas sur les scènes 3D capturées), son et vidéo pas au bit près (formules standard), sauvegarde en cours de partie à confirmer.
- **Prochaine étape recommandée :** phase 6 (parcours complet, sauvegardes croisées avec l'original, test sur le Dell G15), en affinant le rasteriseur avec `tools/gpucheck`.
