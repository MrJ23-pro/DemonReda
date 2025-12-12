# Architecture

## Aperçu général

Le projet implémente un planificateur mono-utilisateur inspiré de `cron` en deux composants principaux :

1. **`erraid`** — un démon Unix tournant en tâche de fond. Il charge et persiste les informations sur disque, planifie l'exécution des commandes et échange avec le client via deux tubes nommés.
2. **`tadmor`** — un client CLI permettant de gérer les tâches (création, suppression, consultation) et de piloter le démon.

La communication suit un protocole binaire léger, sérialisé explicitement (voir `protocole.md`). Tous les accès disque utilisent des appels systèmes bas niveau (`open`, `read`, `write`, `close`, `fsync`, etc.).

## Organisation du projet

```
PourReda/
├── AUTHORS.md
├── ARCHITECTURE.md
├── README.md              # mode d'emploi utilisateur
├── arborescence.md        # conventions d'organisation sur disque
├── serialisation.md       # formats de stockage des tâches et des historiques
├── protocole.md           # format des requêtes/réponses FIFO
├── include/
│   ├── common.h           # types partagés et petites abstractions
│   ├── scheduler.h        # calcul des prochaines occurrences
│   ├── storage.h          # persistance des tâches et des journaux
│   ├── erraid.h           # interface interne du démon
│   └── tadmor.h           # helpers côté client
├── src/
│   ├── erraid/
│   │   ├── main.c         # point d'entrée du démon
│   │   ├── daemon.c       # boucle principale, traitement des requêtes
│   │   ├── executor.c     # gestion des fork/exec et capture stdout/stderr
│   │   └── notifier.c     # gestion des signaux et de la sortie propre
│   ├── tadmor/
│   │   ├── main.c         # parsing CLI et interaction utilisateur
│   │   └── request.c      # construction/affichage des réponses
│   └── shared/
│       ├── scheduler.c
│       ├── storage.c
│       ├── proto.c        # sérialisation/désérialisation des messages FIFO
│       └── utils.c        # fonctions utilitaires (string, horodatage)
└── Makefile
```

## Flux de données

1. `erraid` charge toutes les tâches depuis `RUN_DIRECTORY/tasks` au démarrage.
2. Pour chaque tâche planifiée, le démon calcule la prochaine échéance et dort via `ppoll` sur un timeout combiné avec la surveillance des tubes nommés.
3. Lorsqu'une échéance est atteinte, `erraid` exécute les commandes via `fork/execvp`. Les flux `stdout` et `stderr` sont capturés séparément à l'aide de pipes anonymes redirigés avec `dup2`. Les résultats sont stockés dans `RUN_DIRECTORY/logs` et publiés en mémoire.
4. Le client `tadmor` construit une requête (création, suppression, consultation, arrêt) sérialisée via `proto.c`, l'envoie sur `erraid-request-pipe` puis attend la réponse sur `erraid-reply-pipe`.
5. Le démon traite chaque requête dans sa boucle, manipule la persistance si nécessaire et répond de manière synchrone.

## Gestion des tâches et planification

- Les tâches sont identifiées par un entier unique. La persistance stocke tous les paramètres (type, commandes, planification) selon `serialisation.md`.
- Les structures en mémoire utilisent des tableaux booléens pour les minutes/heures/jours de semaine, permettant un calcul efficace des prochaines occurrences.
- Le module `scheduler` fournit une fonction `scheduler_next_occurrence` qui parcourt les minutes suivantes de manière incrémentale.

## Exécution des commandes

- Chaque tâche simple déclenche un `fork` unique puis `execvp`.
- Les tâches séquentielles réutilisent la même stratégie mais enchaînent plusieurs `fork` successifs en réutilisant les variables de status.
- Les sorties sont collectées via des pipes et rassemblées dans des buffers dynamiques. À la fin de l'exécution, elles sont écrites sur disque.

## Persistance et reprise

- Lorsqu'une tâche est créée ou modifiée, `storage_write_task` écrit un fichier atomique via un fichier temporaire puis `rename` pour garantir la cohérence.
- L'historique est stocké par tâche avec un journal append-only. En cas de redémarrage, `storage_load_state` relit toutes les tâches et leurs dernières exécutions.
- Les tubes nommés sont recréés si absents au démarrage.

## Gestion des signaux

- `erraid` ignore `SIGPIPE`, gère `SIGTERM` et `SIGINT` pour réaliser un arrêt propre.
- `tadmor` gère `SIGINT` afin de relâcher les ressources et fermer les tubes proprement.

## Sécurité et robustesse

- Toutes les opérations critiques vérifient les codes de retour et renvoient des erreurs détaillées.
- Aucune fonction interdite (`system`, famille `FILE*`, etc.) n'est utilisée.
- Les tailles des messages sont bornées et validées pour éviter les dépassements de tampon.

## Tests et extensibilité

- La structure modulaire permet d'ajouter de nouvelles options client ou de nouveaux types de tâches sans refactorisation majeure.
- Les modules partagés (`scheduler`, `storage`, `proto`) peuvent être testés séparément via des exécutables de test ajoutés au Makefile.
