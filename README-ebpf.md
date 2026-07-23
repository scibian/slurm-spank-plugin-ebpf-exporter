# Plugin SPANK ebpf_exporter

Ce plugin démarre `ebpf_exporter` pendant la durée de vie d'un job Slurm et
l'arrête quand plus aucun job ne l'utilise sur le nœud. Il évite de laisser
l'exporter tourner en permanence sur tout le cluster : les sondes eBPF ne
sont chargées que lorsqu'un job réel est présent.

Le fichier `spank_ebpf.c` est le plugin. `ebpf.plugstack.conf` est un exemple
de configuration à déposer dans `/etc/slurm/plugstack.conf.d/`.

## Comment ça marche

Au prolog du job, exécuté en root sur le nœud de calcul avant le lancement,
le plugin incrémente un compteur de référence et démarre `ebpf_exporter` si
c'est le premier job du nœud. À l'épilogue, après la fin du job y compris en
cas d'échec ou d'annulation, il décrémente le compteur et arrête l'exporter
quand celui-ci retombe à zéro.

Le compteur vit dans `/run/ebpf_exporter.refcount`, protégé par `flock()`.
Deux jobs qui démarrent ou se terminent en même temps sur le même nœud ne se
marchent donc pas dessus : l'exporter démarre une seule fois, au premier job,
et s'arrête une seule fois, au dernier. Un job intermédiaire qui se termine
n'arrête pas l'exporter tant qu'un autre job l'utilise encore.

## Prérequis

Le service `ebpf_exporter` doit être installé sur les nœuds de calcul, mais
pas activé au démarrage (`systemctl disable ebpf_exporter`) : c'est le plugin
qui pilote son cycle de vie. Le plugin appelle `systemctl start` et
`systemctl stop`, donc systemd et `flock` (paquet util-linux) sont requis.

Slurm doit fournir les en-têtes de développement pour la compilation, via le
paquet `slurm-devel` (il apporte `/usr/include/slurm/spank.h`).

## Compilation

    gcc -Wall -shared -fPIC -o spank_ebpf.so spank_ebpf.c

Sur RHEL 8 avec `slurm-devel` en place, la compilation passe sans
avertissement. Le `.so` obtenu exporte les symboles `slurm_spank_init`,
`slurm_spank_job_prolog`, `slurm_spank_job_epilog` et `spank_options`, ce que
l'on peut vérifier avec `nm -D spank_ebpf.so | grep slurm_spank`.

Point de compilation à connaître : le plugin a besoin de `<stdint.h>` pour
`uint32_t` et de `<fcntl.h>` pour `O_RDWR`/`O_CREAT`. Les deux en-têtes sont
présents dans les sources ; sans le premier, gcc s'arrête sur
« unknown type name 'uint32_t' ».

## Installation

    install -m0755 spank_ebpf.so /usr/lib64/slurm/spank_ebpf.so

Puis déclarer le plugin. Slurm lit `/etc/slurm/plugstack.conf`. La convention
retenue ici est d'y placer une seule ligne d'inclusion, puis de déposer la
configuration du plugin dans un fichier séparé sous `plugstack.conf.d/` :

    # /etc/slurm/plugstack.conf
    include /etc/slurm/plugstack.conf.d/*.conf

    # /etc/slurm/plugstack.conf.d/ebpf.conf
    optional /usr/lib64/slurm/spank_ebpf.so mode=opt-in

**Point capital pour le mode opt-in, à ne pas manquer** : contrairement à un
plugin qui ne servirait qu'au calcul, celui-ci doit être installé et déclaré
**à la fois sur les nœuds de calcul et sur les nœuds de soumission** (login,
contrôleur — partout où `sbatch`/`srun`/`salloc` s'exécutent). Sans ça,
`sbatch --ebpf` échoue immédiatement avec `unrecognized option '--ebpf'`,
avant même que le job soit soumis : `sbatch` doit charger le plugin
localement pour reconnaître l'option qu'il enregistre.

Redémarrer `slurmd` sur les nœuds de calcul après la copie du `.so` (pas
besoin de redémarrer `slurmctld`/`slurmdbd` : `sbatch` et `srun` relisent la
configuration à chaque invocation, sans démon à relancer). Le chargement se
lit dans le log de slurmd :

    spank: /etc/slurm/plugstack.conf.d/ebpf.conf:1: Loaded plugin spank_ebpf.so

Et côté nœud de soumission, l'option apparaît dans l'aide :

    sbatch --help | grep ebpf
          --ebpf                  Enable eBPF observability (ebpf_exporter) for this job

## Les deux modes

Le mode se choisit par l'argument `mode=` sur la ligne du plugin.

### mode=opt-in (recommandé en production)

Seuls les jobs qui le demandent activent l'exporter, via l'option `--ebpf` :

    optional /usr/lib64/slurm/spank_ebpf.so mode=opt-in

    sbatch --ebpf mon_script.sh
    srun --ebpf ./mon_app

Validé de bout en bout avec les deux commandes, y compris deux jobs
`sbatch --ebpf` qui se recouvrent sur le même nœud (compteur de référence
correct : 0 → 1 → 2 → 1 → 0). Un job soumis sans `--ebpf` reste totalement
neutre, aucune trace du plugin dans le log de slurmd.

Pour que `sbatch --ebpf` fonctionne (et pas seulement `srun --ebpf`), le code
enregistre explicitement l'option dans `slurm_spank_init()` via
`spank_option_register()`. Sans cet appel, la table statique du plugin n'est
tout simplement pas chargée quand `sbatch`/`salloc` tournent (contexte
« allocateur ») : c'est documenté dans `slurm/spank.h` et confirmé par des
plugins réels comportant le même besoin (le plugin Auks, ou
`gridengine_compat.c` de l'université du Delaware). C'est pour ça que
l'installation sur les nœuds de soumission, ci-dessus, est indispensable.

Point technique documenté dans le code (`job_requested_ebpf()`) : la
documentation de Slurm indique que `spank_option_getopt()` doit aussi
fonctionner depuis le prolog et l'épilogue eux-mêmes, mais ce n'est pas le
cas sur les deux builds 25.11 testés ici (elle y renvoie systématiquement
`ESPANK_ERROR`, vérifié avec la valeur numérique exacte). Le plugin lit donc
l'option depuis la variable d'environnement que Slurm expose au prolog, seule
voie fiable constatée dans ce contexte précis — ce qui n'empêche pas
`sbatch --ebpf` de fonctionner, la variable étant bien renseignée dès lors
que l'option est correctement enregistrée à la soumission.

### mode=always

Tout job qui démarre sur le nœud active l'exporter, sans que l'utilisateur
ait rien à faire :

    required /usr/lib64/slurm/spank_ebpf.so mode=always

Plus simple (pas besoin d'installer le plugin sur les nœuds de soumission),
mais impose l'exporter à tous les jobs sans distinction.

## Vérifier que ça marche

Soumettre un job, puis regarder le compteur et l'état du service sur le nœud
pendant qu'il tourne :

    cat /run/ebpf_exporter.refcount        # 1 pendant un job unique
    systemctl is-active ebpf_exporter      # active

Les traces du plugin apparaissent dans le log de slurmd :

    spank_ebpf: prolog job 42
    spank_ebpf: refcount now 1
    spank_ebpf: starting ebpf_exporter
    ...
    spank_ebpf: epilog job 42
    spank_ebpf: refcount now 0
    spank_ebpf: stopping ebpf_exporter

Pour éprouver le compteur, lancer deux jobs qui se recouvrent sur le même
nœud (`sbatch --ebpf` deux fois de suite à quelques secondes d'écart suffit
en mode opt-in). La partition doit alors autoriser le partage
(`OverSubscribe`) si elle n'a qu'un seul nœud, sinon les deux jobs s'exécutent
l'un après l'autre. La séquence attendue est un démarrage au premier prolog,
le compteur qui monte à 2, puis redescend à 1 sans arrêter l'exporter, et
l'arrêt seulement au dernier épilogue.

## Modification de la configuration seule

Changer de mode ou déplacer le fichier `plugstack.conf.d/ebpf.conf` ne
demande pas de recompiler le `.so` : il suffit de redémarrer `slurmd`. La
recompilation n'est nécessaire que si `spank_ebpf.c` change.
