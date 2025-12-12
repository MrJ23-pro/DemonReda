# Arborescence de stockage

Le démon `erraid` persiste son état dans un répertoire racine unique désigné ci-après par `RUN_DIRECTORY`.
Par défaut : `/tmp/$USER/erraid`. L'option `-r RUN_DIRECTORY` du démon permet de spécifier un autre emplacement.

```
RUN_DIRECTORY/
├── tasks/                      # Définitions des tâches (un fichier par TASKID)
│   ├── next_id                  # Fichier texte contenant le prochain identifiant disponible
│   └── <TASKID>.task            # Fichier sérialisé décrivant la tâche
├── logs/                       # Historique des exécutions
│   └── <TASKID>/
│       ├── history.log         # Entrées append-only (date, code de retour)
│       ├── last.stdout         # Dernière sortie standard
│       └── last.stderr         # Dernière sortie d'erreur
├── pipes/                      # Tubes nommés pour la communication client/démon
│   ├── erraid-request-pipe
│   └── erraid-reply-pipe
└── state/                      # Fichiers d'état supplémentaires pour reprise à chaud
    └── scheduler.state         # Timestamp des prochaines exécutions (optionnel)
```

## Permissions et création

- `erraid` assure l'existence des répertoires parents au démarrage (`mkdir -p` logique via `mkdir()` récursif).
- Les permissions par défaut sont `0700` pour le répertoire racine et `0600` pour les fichiers, afin de garantir l'isolement mono-utilisateur.

## Nettoyage

- `make distclean` supprime l'ensemble du contenu généré par l'exécution (`tasks/`, `logs/`, `pipes/`, `state/`) sans toucher au code source.
- La commande cliente `tadmor -q` demande un arrêt propre : le démon ferme les FIFO mais ne supprime pas les fichiers d'état.
