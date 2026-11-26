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
- [x] Blocage intermittent du harnais en mode accéléré (S–M) : détection complète (plus d'image qui avance **ou** serveur de débogage muet), piles des threads relevées, relance automatique ; instances parallèles isolées ; `tools/stall_hunt.py` ; 13 sessions de suite sans blocage. Les vérifications 1:1 passent par le rejeu, sans le harnais accéléré. Cause non reproduite ; pistes : correctifs SIO/IRQ du framework en amont (à reprendre lors du fork, phase 3).

### Phase 2 — Bibliothèque PsyQ : identifier et nommer  `[x]` terminée  (L)
Les fonctions de la bibliothèque Sony sont **déjà écrites publiquement** (projet de décompilation PsyQ) : on les reconnaît, on les nomme, on ne les réécrit pas à la main. Détails : `docs/LIBRARIES.md`.
- [x] Reconnaissance par signatures (`tools/psyq_sigs.py`, signatures publiques lab313ru, clonées hors du dépôt) : **149 fichiers objets, 547 fonctions, 414 noms** en une vingtaine de secondes ; kit **PsyQ 4.5** environ.
- [x] Bibliothèques nommées : libc2, libpress, libgpu, libgte, libcd, libds, libetc, libapi, libmath, libsn, libmcrd, libcard, libpad (+ `SsGetNck` de libsnd). Pas de libgs ni de libspu standard. Contrôle : 24 noms existants confirmés, 2 noms devinés corrigés.
- [x] Séparation « bibliothèque » / « code du jeu » (`tools/progress.py`) : **2 378 fonctions de jeu à réécrire** (phase 3), **694 fonctions de bibliothèque à remplacer** (phase 4). Aucune bibliothèque dans les modules `GAME`/`EVENT`/`PACK1` : tout y est du code du jeu.
- [x] Découverte : la fin du programme (105 fonctions, `0x80048A78`-`0x800521E0`) est un **pilote son propre au jeu** (accès direct à la puce son), ni libspu ni libsnd : à étudier avec le moteur son de la phase 4.

### Phase 3 — Réécriture du jeu en C (mode mixte)  `[~]` démarrée  (XL)
**2 378 fonctions** du jeu à réécrire (programme 411, `GAME00` 1 264, `EVENT00` 204, `GAME01-03` 134, `PACK1` 365). Ordre proposé, du plus utile au plus lourd :
- [x] 1re fonction : `func_800A09C0` (cap du joueur) — 7 857 appels comparés, 0 différence ; **rejeu de la session de référence `in_sync` au bit près** avec la fonction réécrite seule (mêmes registres, même coût en cycles) ; version analogique vérifiée à 11 angles *(l'extension analogique appartient à la phase 7, voir plus bas : elle reste désactivée dans la 1.0)*.
- [ ] Couche entrées complète : mise à jour des manettes, direction, boutons, répétition, vibration (`0x8002A5F0` et voisines) (M).
- [ ] Boucle principale et gestionnaire d'état du jeu / changement de mode (GAME00 ↔ EVENT00 ↔ GAME01-03) (M).
- [ ] Chargeur de fichiers et de modules (lecture CD, décompression, `PAK`/`GP`) (L).
- [ ] Gestion mémoire du jeu (allocation, tas) (M).
- [ ] Caméra (L).
- [ ] Entités et physique : Flint (déplacement, attaque, saut), ennemis, objets, collisions (XL).
- [ ] Scripts de cinématique / moteur d'événements (`EVENT00`) (XL).
- [ ] Menus, inventaire, boutique, forge, sauvegarde/chargement, options (L).
- [ ] Rendu du jeu : construction des listes d'ordre, modèles, décors, effets, interface (XL).
- [ ] Son du jeu : musiques, bruitages, voix, mixage (L).
- [ ] Mini-jeux et cas particuliers (L).
- [ ] Lever la limite de 128 remplacements (fork de PSXRecomp, ou passage à la phase 4) (S).
- [ ] À chaque lot : vérification fonction par fonction + rejeu de scène (niveaux 1 et 2).
- **Jalon :** 100 % des fonctions du jeu réécrites, mode mixte encore actif pour la bibliothèque PsyQ.

### Phase 4 — Couche PsyQ native et plateforme  `[ ]`  (XL, en parallèle de la phase 3)
Remplacer la console simulée par du code PC, module par module. Chaque module est livré et vérifié séparément :
- [ ] **Fichiers :** le « CD » devient des fichiers extraits du disque du joueur ; chargements instantanés (M).
- [ ] **Entrées :** manette et clavier via SDL3, au format libpad d'origine (S–M).
- [ ] **Cartes mémoire :** fichiers de sauvegarde **compatibles avec l'original** (format PS1) (M).
- [ ] **GTE :** calculs de géométrie en logiciel, bit-exacts (M).
- [ ] **GPU :** rendu OpenGL des primitives PS1 (listes d'ordre, polygones, sprites, textures à palette, transparences, dither). Résolution interne 1x en 1.0 (L).
- [ ] **Son :** pilote de synthèse des banques VAB et des séquences SEQ, mixage, effets (réverbération), musiques XA (L).
- [ ] **Vidéo :** décodeur MDEC/STR (intro `A2STR.STR`) (M).
- [ ] **Temps :** boucle à 50 Hz (PAL) indépendante du rythme de l'écran ; son indépendant de l'image (M).
- [ ] **Cible performance :** 50 images/s stables sur le Dell G15 (RTX forcée) et jouable sur puce intégrée (S–M).
- [ ] Retirer PSXRecomp et OpenBIOS du produit final ; l'exécutable ne contient plus de recompilation (M).
- **Jalon :** le jeu tourne 1:1 sans la console simulée.

### Phase 5 — Importation du disque et empaquetage  `[ ]`  (M)
- [ ] Assistant de premier lancement : l'utilisateur choisit son `.cue`/`.bin` (ou `.chd`) ; vérification de l'empreinte du disque français ; extraction vers un dossier de données.
- [ ] Versions de disque : français SLES-02601 d'abord (autres langues = hors périmètre 1.0).
- [ ] Windows x64 : exécutable + installation simple ; flag « carte graphique hautes performances ».
- [ ] macOS Apple Silicon : `Alundra 2.app`.
- [ ] Construction automatisée depuis le code source (CI), sans aucune donnée du jeu.
- [ ] Documentation d'installation (français).

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
| Volume de la réécriture (2 378 fonctions) | durée de plusieurs mois | travail par lots, outils de nommage automatique, vérification automatique, priorité aux parties qui bloquent le reste |
| Code compilé de l'époque (compilateur PsyQ) difficile à relire | fonctions lentes à comprendre | décompilateur MIPS, comparaison automatique, tests d'équivalence |
| Limite de 128 remplacements simultanés | bloque le mode mixte au-delà | fork du framework ou passage direct à la phase 4 |
| Précision du son (réverbération, séquences) | écart audible avec l'original | journaux SPU/SEQ comparés à l'original |
| Blocage intermittent du harnais (cartes mémoire) | tests peu fiables | détection complète et relance ; vérifications par rejeu, sans le harnais ; cause non reproduite (13 sessions sans blocage) |
| Dépendance au Dell G15 pour le test Windows | boucle de test plus lente | compilation croisée depuis le Mac, zip à tester |
| Aspect juridique (code dérivé de celui d'Activision/Sony) | retrait possible d'un dépôt public | aucune donnée du jeu dans le dépôt ; visibilité à décider avant publication |

---

## 6. État actuel (résumé)

- **Terminé :** v1 jouable (Mac/Windows) ; **phase 1** : disque extrait, tout le code cartographié et découpé (programme, 5 modules `GAME`/`EVENT`, 17 modules de `PACK1`), formats inventoriés, noms de base, harnais de test, outils de vérification 1:1 (rejeu, journal GPU, comparaison image par image) ; première fonction réécrite, **identique au bit près** sur la session de référence.
- **Phase 2 terminée :** bibliothèques Sony reconnues par signature, code du jeu et bibliothèques séparés.
- **Chiffres :** 3 072 fonctions = **2 378 de jeu** (1 réécrite, 0,04 %) + 694 de bibliothèque (547 reconnues) ; 457 nommées.
- **Prochaine étape recommandée (1:1 d'abord) :**
  1. Réécriture de la couche entrées complète, de la boucle principale et du chargeur (phase 3).
  2. Démarrer les modules natifs les plus simples (fichiers, entrées, cartes mémoire) pour réduire peu à peu la dépendance à la recompilation (phase 4).
