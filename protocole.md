# Protocole de communication `erraid` ⇔ `tadmor`

La communication s'effectue via deux tubes nommés présents dans `RUN_DIRECTORY/pipes/` :

- `erraid-request-pipe` : écriture par `tadmor`, lecture par `erraid`.
- `erraid-reply-pipe` : écriture par `erraid`, lecture par `tadmor`.

Chaque requête est atomique : le client écrit un message complet puis attend la réponse correspondante. Les messages sont encodés dans un format binaire minimaliste composé d'un en-tête fixe et d'un corps sérialisé en JSON compact. Tous les champs utilisent l'endianness native (`little-endian` sur Linux lulu).

## En-tête commun

| Offset | Taille (octets) | Champ             | Description |
|--------|------------------|-------------------|-------------|
| 0      | 4                | `magic`           | Signature ASCII `ERID` (0x45 0x52 0x49 0x44) |
| 4      | 1                | `version`         | Version du protocole (`0x01`). |
| 5      | 1                | `type`            | Type de message (voir ci-dessous). |
| 6      | 2                | `reserved`        | Doit être zéro, réservé pour futur usage. |
| 8      | 4                | `payload_length`  | Longueur en octets du corps JSON (uint32). |

Taille totale de l'en-tête : 12 octets.

Le corps suit immédiatement et contient un objet JSON UTF-8 sans espaces superflus. Exemple : `{"task_id":5}`. La taille maximale d'un message est limitée à 4096 octets (en-tête inclus).

## Types de messages

| Type (`type`) | Sens | Objet JSON |
|---------------|------|------------|
| `0x01` | Requête `PING` (diagnostic) | `{}` |
| `0x02` | Réponse `PONG` | `{}` |
| `0x10` | Requête `LIST_TASKS` (`-l`) | `{}` |
| `0x11` | Réponse liste | `{ "tasks": [ { ... } ] }` |
| `0x20` | Requête `CREATE_SIMPLE` (`-c`) | `{ "commands": [["/bin/echo","hi"]], "schedule": { "minutes": "...", "hours": "...", "weekdays": "..." } }` |
| `0x21` | Requête `CREATE_SEQUENCE` (`-s`) | idem mais plusieurs commandes |
| `0x22` | Requête `CREATE_ABSTRACT` (`-n`) | `{ "commands": [], "schedule": null }` |
| `0x23` | Réponse création | `{ "task_id": 42 }` |
| `0x30` | Requête `REMOVE_TASK` (`-r`) | `{ "task_id": 42 }` |
| `0x31` | Réponse suppression | `{}` |
| `0x40` | Requête `LIST_HISTORY` (`-x`) | `{ "task_id": 42 }` |
| `0x41` | Réponse historique | `{ "history": [ { "epoch": 1690000000, "status": 0 } ] }` |
| `0x50` | Requête `GET_STDOUT` (`-o`) | `{ "task_id": 42 }` |
| `0x51` | Réponse stdout | `{ "stdout": "base64..." }` |
| `0x52` | Requête `GET_STDERR` (`-e`) | `{ "task_id": 42 }` |
| `0x53` | Réponse stderr | `{ "stderr": "base64..." }` |
| `0x60` | Requête `SHUTDOWN` (`-q`) | `{}` |
| `0x61` | Réponse arrêt | `{}` |
| `0x7F` | Réponse erreur | `{ "code": "TASK_NOT_FOUND", "message": "..." }` |

Les réponses incluent systématiquement un champ `status` optionnel (`"OK"` par défaut). Pour minimiser la taille, les chaînes longues (comme stdout/stderr) sont encodées en Base64.

## Règles générales

1. `tadmor` ouvre les deux FIFO en mode blocant (`O_WRONLY` / `O_RDONLY`) avant l'envoi.
2. Chaque message est écrit en une seule opération `write` (ou multiples mais atomiques < PIPE_BUF).
3. `erraid` traite les messages dans l'ordre de lecture et répond immédiatement.
4. En cas d'erreur de parsing ou de protocole, `erraid` renvoie un message de type `0x7F`.
5. Les champs JSON non reconnus sont ignorés pour permettre l'extensibilité.

## Validation côté client

- Vérifier la signature (`magic`) et la version.
- Vérifier que `payload_length` correspond à la taille réelle lue.
- Parser le JSON via une bibliothèque interne minimaliste (sans dépendances externes) ou un parseur maison robuste.

## Extensibilité

- De nouveaux types peuvent être ajoutés en réservant des plages (`0x70-0x7E`).
- Le champ `reserved` pourra transporter des drapeaux de chiffrement ou de compression si nécessaire.
