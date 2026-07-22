# CLAUDE.md — Instructions projet eBPF/POCOOD

## Contexte

Contexte pour Claude Code. Lis ce fichier avant toute modification.

Si une question de clarification n'appelle qu'une réponse évidente "Oui" ou "Non", suppose la réponse la plus raisonnable et continue sans demander de confirmation.

Ne pose une question que si :
- l'action est destructive,
- plusieurs interprétations sont possibles,
- ou une information est réellement indispensable.

---

Observabilité eBPF pour jobs Slurm sur un cluster HPC multi-centaines de nœuds. Quatre programmes BPF CO-RE (cpu_per_process, mem_per_node, io_per_process, net_per_process) alimentent ebpf_exporter → Prometheus → Grafana. L'exploitant parle français dans toutes les sessions.
Mais les messages de commit git se feront en uniquement en anglais et sans claude comme co-auteur.

## Environnement technique

- **OS** : RHEL 8.10 (cgroup v1, noyau 4.18) en production, RHEL 9.4+ (cgroup v2, noyau 5.14) en préparation. RHEL 10 (cgroup v1 retiré) à anticiper.
- **cgroup v2** : le point bloquant. L'extraction du jobid passe par `dfl_cgrp->kn` au lieu de `subsys[memory_cgrp_id]->cgroup->kn`. Un en-tête `common_cgroup.bpf.h` gère les deux variantes par `#ifdef CGROUP_V2`.
- **Labels eBPF** : {jobid, comm, exe} (+ dir pour io, + pid pour les maps bornées). Pas de label user dans le BPF ; la correspondance passe par une jointure PromQL avec slurm-job-exporter (`slurmjobid` renommé en `jobid` par `label_replace`).
- **Noms de métriques Prometheus** : préfixe `ebpf_exporter_<nom_programme>_<nom_métrique>`. Les noms exacts dépendent du champ `name:` du programme dans le YAML ebpf_exporter — toujours vérifier avec `curl /metrics | grep` avant de supposer.
- **node_exporter** : les jobs contiennent `_node_` dans leur nom (ex: `equipment_batch_node_exporter`), pas au début. Regex : `.*_node_.*`. Le label `host` n'existe PAS sur la métrique `up` — utiliser `label_replace` sur `instance` avec regex `([^:.]+)` pour extraire le short hostname.
- **Grafana** : Podman/UBI9, HTTPS interne, subpath `/grafana`.
- **Prometheus** : potentiellement derrière un subpath aussi. Vérifier avec `ps aux | grep external-url`.

## Workflow de livraison

1. L'exploitant uploade ses fichiers comme référence.
2. Claude génère un patch contre CE fichier exact (`git format-patch`).
3. Le patch est vérifié par `git am` dans un repo séparé.
4. Livraison : patch + fichier complet patchable.
5. **Claude ne compile pas** : chaque hypothèse doit être validée par une compilation réelle côté exploitant. Le silence après un patch signifie historiquement succès.

## Règles de rédaction

- **Langue** : français dans toutes les sessions.
- **Documentation** : MediaWiki strict. Sections niveau 2 plates (`== Titre ==`). Synoptiques dashboards avec colonne "Équivalent Linux". **Pas de markdown** (pas de `#`, `*`, `-` hors `<pre>`). Pas de tiret cadratin (—, –).
- **Humanisation obligatoire** : varier les longueurs de phrases, éviter les mots IA (crucial, essentiel, il est important de noter, fondamental, indispensable, en résumé, en conclusion, pivotal, clé, déterminant), utiliser des tournures directes. Ne pas commencer les phrases par "Il est" ou "Cela". Pas de listes à puces dans la prose — reformuler en phrase avec "x, y et z".
- **Honnêteté** : dire quand une approche ne converge pas plutôt que de spéculer. Signaler les noms de métriques inférés (non confirmés) en majuscules dans les descriptions des panels.

## Patterns PromQL validés

### Faire

- **Somme scalaire de deux métriques** : `(sum(rate(A[w])) or vector(0)) + (sum(rate(B[w])) or vector(0))`
- **Table avec deux métriques** (appariement label-safe) : `(sum by (L) (rate(A[w])) or sum by (L) (rate(B[w])) * 0) + (sum by (L) (rate(B[w])) or sum by (L) (rate(A[w])) * 0)`
- **Jointure utilisateur** : `(X * on (jobid) group_left (user) max by (jobid, user) (label_replace($jobinfo_metric, "jobid", "$1", "slurmjobid", "(.+)"))) or (X unless on (jobid) …)` — le `unless` évite les doublons.
- **Compter les séries eBPF** : `sum(scrape_samples_scraped{job=~"$ebpf_job"})` — PAS `count({__name__=~"..."})` ni `count({job=~"..."})` qui dépassent `--query.max-samples` sur les gros clusters.
- **Panels table Grafana** : toujours `"format": "table"` dans chaque target, sinon les labels ne deviennent pas des colonnes.
- **Max scrape observé** : range query + réducteur `max` Grafana + `"timeFrom": "2d"` au niveau du panel. Pas de recording rule.

### Ne pas faire

- `rate(A) + rate(B)` nu : séries qui disparaissent quand un côté manque.
- `count({__name__=~"ebpf_exporter_.+"})` : charge toutes les séries en mémoire, dépasse max-samples.
- `max_over_time(metric[$variable_longue])` dans un panel : scan de millions d'échantillons à chaque refresh.
- `histogram_quantile(0.9, rate(..._bucket...))` sur `prometheus_engine_query_duration_seconds` : c'est un summary, pas un histogram. Lire directement `{quantile="0.9"}`.
- Supposer que le label `host` existe sur `up` pour node_exporter : il n'y est pas. Toujours `label_replace` sur `instance`.

## Contraintes BPF

- `__always_inline` cause stack overflow dans les chaînes d'appels profondes. Le retirer de TOUTE la chaîne transitive, pas d'un seul niveau.
- `#pragma unroll` exige un corps de boucle statique — pas de lookup de map dans la boucle.
- Scratch percpu à slot unique = corruption par réentrance d'interruption. Multi-slots indexés par profondeur = OK.
- Race LRU entre itération et lookup au scrape : bénigne (métrique manquée un scrape).
- Modification YAML seule (ajout/retrait de métriques) : pas de recompilation du .bpf.o, juste `systemctl restart ebpf_exporter`.

## Cardinalité — les postes de coût

Par ordre d'impact variable selon le profil de charge :
1. **Histogrammes de latence I/O** : 37 séries par combinaison {jobid, comm, exe, dir} × 2 sens. Dominant quand beaucoup de répertoires (label dir). Supprimables dans le YAML sans recompilation.
2. **Maps per-PID bornées** : mem_per_node (12 dimensions × PIDs), cpu/io (quelques métriques × PIDs). Dominant sur les jobs MPI massifs (beaucoup de rangs). Calibrable via `-DPID_SNAPSHOT_MAX_ENTRIES`.
3. **Métriques node_exporter parasites** : `node_systemd_unit_state` (257K séries sur ce cluster), droppable via `--collector.systemd.unit-include` ou `metric_relabel_configs`.

## Dashboards et scripts livrés

- `ebpf_cluster_overview.json` — Vue cluster (version courante : 20). Variable `$compute_filter` pour filtrer les nœuds de calcul (`[a-z]n.*`).
- `ebpf_benchmark.json` — Monitoring du serveur Prometheus.
- `{mem_per_node,cpu_per_process,io_per_process,net_per_process}.json` — Dashboards par composant.
- `ebpf_diagnose.sh` — Diagnostic de passage à l'échelle (654 lignes, 42 commandes opératoires).
- `ebpf_readiness_check.sh` — Prérequis nœud + mesure overhead `--overhead`.
- `ebpf_benchmark.sh` — `--mode measure` (benchmarks dashboards) et `--mode compare` (avant/après).
- `pocood_top.sh` — Top-N consommateurs en terminal (colonne utilisateur, `CMDLINE=1` pour l'argv complet via ssh).
- `spank_ebpf.c` — Plugin SPANK optionnel (activation à la demande, compteur de référence multi-jobs).
- `common_cgroup.bpf.h` — En-tête partagé cgroup v1/v2.

## Nommage

- Scripts : préfixe `ebpf_` (pas `pocood_` sauf `pocood_top.sh` historique).
- Documentation wiki : `slurm_ebpf.wiki`, `README-deployment.wiki`, `adaptation_cgroup_v2.wiki`, `etude_deploiement_ebpf.wiki`, `ebpf_benchmark.wiki`, `vue_cluster_dashboard.wiki`.
- Dashboards : UID `pocood-cluster-overview` et `ebpf-benchmark`.

## Variables Grafana du dashboard Vue cluster

| Variable | Défaut | Usage |
|---|---|---|
| `$DS_PROMETHEUS` | (à l'import) | Source Prometheus |
| `$topn` | 10 | Profondeur des classements |
| `$rate_window` | 5m | Fenêtre des rate() |
| `$ebpf_job` | ebpf_exporter | Job de scrape ebpf_exporter |
| `$jobinfo_metric` | slurm_job_memory_usage | Métrique slurm-job-exporter pour la jointure user |
| `$node_job` | `.*_node_.*\|prometheus_master` | Jobs node_exporter |
| `$compute_filter` | `[a-z]n.*` | Filtre hostname nœuds de calcul |
