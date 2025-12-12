# Sérialisation des tâches et journaux

Toutes les sérialisations utilisent des fichiers texte UTF-8 (ASCII pur) pour simplifier la lecture humaine tout en restant compatibles avec les appels système bas niveau (`read`/`write`). Chaque champ est encodé en binaire sous forme décimale ou hexa, séparé par des sauts de ligne `\n`. Aucune ligne ne doit excéder 1024 caractères.

## Fichier `tasks/next_id`

- Contient un entier non signé sur une seule ligne (`uint64_t`).
- Valeur initiale : `1`.

## Fichier `tasks/<TASKID>.task`

Format séquentiel ligne par ligne :

| Ligne | Champ | Description |
|-------|-------|-------------|
| 1 | `task_id` | Identifiant unique (décimal).
| 2 | `task_type` | `SIMPLE`, `SEQUENCE` ou `ABSTRACT`.
| 3 | `command_count` | Nombre total de commandes (>=1 pour SIMPLE/SEQUENCE, 0 pour ABSTRACT).
| 4..(3+N) | commandes | Une commande par ligne. Chaque commande est encodée en JSON minimal (`["/bin/echo","hello"]`). Les guillemets doubles et antislash sont échappés selon JSON.
| (4+N) | `minutes` | Liste de 60 bits encodés en hexadécimal : 15 caractères hex (60 bits utiles, 4 bits padding). Bit 0 = minute 0.
| (5+N) | `hours` | 24 bits encodés en hexadécimal sur 6 caractères. Bit 0 = heure 0.
| (6+N) | `weekdays` | 7 bits encodés en hexadécimal sur 2 caractères. Bit 0 = dimanche.
| (7+N) | `flags` | Champ optionnel (réservé). Valeur actuelle : `0`.
| (8+N) | `last_run_epoch` | Timestamp UNIX de la dernière exécution connue (`int64`, `-1` si aucune).

### Exemple

```
42
SEQUENCE
2
["/usr/bin/printf","Hello"]
["/usr/bin/date"]
0FFFFFFFFFFFFFF
00000F
7F
0
-1
```

## Journaux `logs/<TASKID>/history.log`

Chaque ligne encode une exécution :

```
<epoch> <status> <stdout_len> <stderr_len>
```

- `<epoch>` : timestamp UNIX (`int64` en décimal).
- `<status>` : code de retour (`int32`).
- `<stdout_len>` / `<stderr_len>` : tailles (octets) des fichiers `last.stdout` / `last.stderr` après écriture.

## Fichiers `last.stdout` et `last.stderr`

- Contiennent les flux bruts tels qu'écrits par la commande (octets binaires). Ils sont écrasés après chaque exécution.
- La taille est reflétée dans `history.log`.

## Fichier optionnel `state/scheduler.state`

- Liste triée des événements planifiés à venir.
- Une ligne par tâche :

```
<TASKID> <next_epoch>
```

- `next_epoch = -1` si aucune occurrence programmée (tâches abstraites ou en attente de recalcul).
