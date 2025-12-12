# DemonReda

## Compilation

```bash
make
```
Cette commande produit les binaires `erraid` (démon) et `tadmor` (client CLI).

Pour nettoyer complètement l’arborescence de build :

```bash
make distclean
```

## Tests automatiques

Un script de bout en bout est fourni dans `scripts/e2e.sh`. Il :

1. redémarre un environnement propre,
2. lance `erraid`,
3. crée une tâche simple et une tâche séquentielle,
4. vérifie les sorties `stdout/stderr` et `history.log`,
5. supprime les tâches et arrête le démon.

Exécution :

```bash
./scripts/e2e.sh
```

Le script échoue avec un message explicite si une étape ne produit pas le résultat attendu.

## Utilisation manuelle

### 1. Démarrer le démon

```bash
./erraid -r /chemin/vers/rundir
```

Sans option, le démon utilise `/tmp/$USER/erraid`.

### 2. Créer des tâches avec `tadmor`

Dans un autre terminal :

```bash
# tâche simple exécutée chaque minute
./tadmor -c -m 0FFFFFFFFFFFFFF -H 00000F -w 7F /bin/echo "bonjour"

# tâche séquentielle : deux commandes exécutées l’une après l’autre
./tadmor -s -m 0FFFFFFFFFFFFFF -H 00000F -w 7F /bin/echo "phase 1" -- /bin/sh -c "echo phase 2"

# tâche abstraite (pas d’exécution)
./tadmor -n -m 0 -H 0 -w 0
```

Le démon répond avec un JSON contenant `{"status":"OK","task_id":X}`.

### 3. Consultation

```bash
# lister toutes les tâches
./tadmor -l

# afficher l’historique d’une tâche
./tadmor -x <task_id>

# récupérer le dernier stdout / stderr
./tadmor -o <task_id>
./tadmor -e <task_id>
```

Les fichiers correspondants sont stockés sous `rundir/logs/<task_id>/` (`history.log`, `last.stdout`, `last.stderr` + snapshots).

### 4. Suppression et arrêt

```bash
# supprimer une tâche
./tadmor -r <task_id>

# arrêter le démon
./tadmor -q
```

## Structure disque

L’arborescence complète du répertoire d’exécution est décrite dans `arborescence.md`. Les formats de sérialisation sont détaillés dans `serialisation.md` et le protocole FIFO dans `protocole.md`.
