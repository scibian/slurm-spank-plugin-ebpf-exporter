# SPANK plugin: ebpf_exporter

This plugin starts `ebpf_exporter` for the lifetime of a Slurm job and stops
it once no job is using it on the node anymore. It avoids leaving the
exporter running permanently across the whole cluster: eBPF probes are only
loaded while a real job is present.

`spank_ebpf.c` is the plugin. `ebpf.plugstack.conf` is a sample configuration
to drop into `/etc/slurm/plugstack.conf.d/`.

## How it works

In the job prolog, run as root on the compute node before launch, the plugin
increments a reference counter and starts `ebpf_exporter` if this is the
first job on the node. In the epilog, after the job ends (including on
failure or cancellation), it decrements the counter and stops the exporter
once it reaches zero.

The counter lives in `/run/ebpf_exporter.refcount`, guarded by `flock()`. Two
jobs starting or ending at the same time on the same node don't step on each
other: the exporter starts exactly once, on the first job, and stops exactly
once, on the last. A job that ends in between does not stop the exporter
while another job is still using it.

## Prerequisites

The `ebpf_exporter` service must be installed on compute nodes, but not
enabled at boot (`systemctl disable ebpf_exporter`): the plugin drives its
lifecycle. The plugin calls `systemctl start` and `systemctl stop`, so
systemd and `flock` (util-linux package) are required.

## Build

    gcc -Wall -shared -fPIC -o spank_ebpf.so spank_ebpf.c

On RHEL 8 with `slurm-devel` in place, the build completes without warnings.
The resulting `.so` exports the symbols `slurm_spank_init`,
`slurm_spank_job_prolog`, `slurm_spank_job_epilog` and `spank_options`, which
can be checked with `nm -D spank_ebpf.so | grep slurm_spank`.

## Installation

    install -m0755 spank_ebpf.so /usr/lib64/slurm/spank_ebpf.so

Then declare the plugin. Slurm reads `/etc/slurm/plugstack.conf`. The
convention used here is to put a single include line there, and drop the
plugin's own configuration in a separate file under `plugstack.conf.d/`:

    # /etc/slurm/plugstack.conf
    include /etc/slurm/plugstack.conf.d/*.conf

    # /etc/slurm/plugstack.conf.d/ebpf.conf
    optional /usr/lib64/slurm/spank_ebpf.so mode=opt-in

**Critical point for opt-in mode, easy to miss**: unlike a plugin that would
only matter on compute nodes, this one must be installed and declared **on
both compute nodes and submission hosts** (login node, controller — wherever
`sbatch`/`srun`/`salloc` run). Without that, `sbatch --ebpf` fails immediately
with `unrecognized option '--ebpf'`, before the job is even submitted:
`sbatch` needs to load the plugin locally to recognize the option it
registers.

Restart `slurmd` on compute nodes after copying the `.so` (no need to
restart `slurmctld`/`slurmdbd`: `sbatch` and `srun` re-read the
configuration on every invocation, no daemon to reload there). The load can
be seen in the slurmd log:

    spank: /etc/slurm/plugstack.conf.d/ebpf.conf:1: Loaded plugin spank_ebpf.so

And on the submission host, the option shows up in the help output:

    sbatch --help | grep ebpf
          --ebpf                  Enable eBPF observability (ebpf_exporter) for this job

## The two modes

The mode is chosen with the `mode=` argument on the plugin line.

### mode=opt-in (recommended in production)

Only jobs that ask for it enable the exporter, via the `--ebpf` option:

    optional /usr/lib64/slurm/spank_ebpf.so mode=opt-in

    sbatch --ebpf my_script.sh
    srun --ebpf ./my_app

Validated end to end with both commands, including two overlapping
`sbatch --ebpf` jobs on the same node (reference counter correctly goes
0 → 1 → 2 → 1 → 0). A job submitted without `--ebpf` stays fully neutral, no
trace of the plugin in the slurmd log.

For `sbatch --ebpf` to work (not just `srun --ebpf`), the code explicitly
registers the option in `slurm_spank_init()` via `spank_option_register()`.
Without that call, the plugin's static option table simply isn't loaded when
`sbatch`/`salloc` run (the "allocator" context): this is documented in
`slurm/spank.h` and confirmed by real plugins with the same need (the Auks
plugin, or the University of Delaware's `gridengine_compat.c`). That's why
installing on submission hosts, as described above, is mandatory.

Technical point documented in the code (`job_requested_ebpf()`): Slurm's
documentation states that `spank_option_getopt()` should also work from the
prolog and epilog themselves, but that is not the case on some Slurm
versions such as 25.11 (it consistently returns `ESPANK_ERROR` there,
verified against the exact numeric value). The plugin therefore reads the
option from the environment variable Slurm exports to the prolog, the only
reliable path found in this specific context — which does not prevent
`sbatch --ebpf` from working, since that variable is properly set as long as
the option is correctly registered at submission time.

### mode=always

Every job that starts on the node enables the exporter, with nothing
required from the user:

    required /usr/lib64/slurm/spank_ebpf.so mode=always

Simpler (no need to install the plugin on submission hosts), but forces the
exporter on every job indiscriminately.

## Checking that it works

Submit a job, then look at the counter and the service state on the node
while it runs:

    cat /run/ebpf_exporter.refcount        # 1 while a single job is running
    systemctl is-active ebpf_exporter      # active

The plugin's traces show up in the slurmd log:

    spank_ebpf: prolog job 42
    spank_ebpf: refcount now 1
    spank_ebpf: starting ebpf_exporter
    ...
    spank_ebpf: epilog job 42
    spank_ebpf: refcount now 0
    spank_ebpf: stopping ebpf_exporter

To exercise the counter, run two jobs that overlap on the same node
(`sbatch --ebpf` twice a few seconds apart is enough, in opt-in mode). The
partition must then allow sharing (`OverSubscribe`) if it only has one node,
otherwise the two jobs run one after the other. The expected sequence is a
start on the first prolog, the counter climbing to 2, then dropping back to
1 without stopping the exporter, and the stop happening only on the last
epilog.

## Configuration-only changes

Changing the mode or moving the `plugstack.conf.d/ebpf.conf` file does not
require rebuilding the `.so`: restarting `slurmd` is enough. A rebuild is
only needed when `spank_ebpf.c` itself changes.
