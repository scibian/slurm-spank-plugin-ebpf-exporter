/*
 * spank_ebpf.c — SPANK plugin that enables ebpf_exporter for the lifetime
 * of a job.
 *
 * Principle:
 *   - Job prolog (before launch, as root on the node): increment a reference
 *     counter and start ebpf_exporter if this is the first job on the node.
 *   - Job epilog (after the job ends): decrement the counter and stop the
 *     exporter once no job is using it anymore.
 *
 * The reference counter lives in /run/ebpf_exporter.refcount and is guarded
 * with flock() so concurrent jobs on the same node stay consistent.
 *
 * Two modes (selected in plugstack.conf):
 *   always   — every job enables the exporter (default)
 *   opt-in   — only jobs that ask for it enable the exporter
 *
 * Build:
 *   gcc -Wall -shared -fPIC -o spank_ebpf.so spank_ebpf.c
 *   install -m0755 spank_ebpf.so /usr/lib64/slurm/
 *
 * Configuration in /etc/slurm/plugstack.conf (or a file included from it):
 *   # always mode: every job enables the exporter
 *   required /usr/lib64/slurm/spank_ebpf.so mode=always
 *
 *   # opt-in mode: only jobs that request it enable the exporter
 *   optional /usr/lib64/slurm/spank_ebpf.so mode=opt-in
 *
 * Opt-in usage (see job_requested_ebpf() for the detection details):
 *   sbatch --ebpf my_script.sh
 *   srun --ebpf ./my_app
 *
 * For `sbatch --ebpf`/`salloc --ebpf` to be accepted, the plugin and its
 * plugstack.conf entry must also be installed on the submission host (login
 * node, or wherever sbatch/salloc run) — not only on compute nodes. Without
 * that, sbatch's own CLI parser rejects --ebpf as an unrecognized option
 * before the job is even submitted. See slurm_spank_init() for why this is
 * needed on top of the static spank_options[] table.
 *
 * Dependencies: systemd (systemctl), flock (util-linux).
 * The ebpf_exporter service must be installed but NOT enabled at boot.
 *
 * Logs: syslog via slurm_info/slurm_error (in the slurmd log, e.g.
 * /var/log/slurm/slurmd.log or journalctl -u slurmd / slurmstepd).
 */

#include <slurm/spank.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>     /* uint32_t (S_JOB_ID) */
#include <string.h>
#include <unistd.h>
#include <fcntl.h>      /* O_RDWR, O_CREAT (open in refcount_change) */
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>

SPANK_PLUGIN(spank_ebpf, 1);

#define REFCOUNT_FILE "/run/ebpf_exporter.refcount"
#define SERVICE_NAME  "ebpf_exporter"

/* Set by _opt_handler when --ebpf is parsed. Only meaningful in the process
 * that parses the option (allocator/task); used as a last-resort fallback in
 * job_requested_ebpf(). */
static int ebpf_requested = 0;

/* ── --ebpf option registered with Slurm ──────────────────────────────── */

static int _opt_handler(int val, const char *optarg, int remote);

struct spank_option spank_options[] = {
    { "ebpf", NULL,
      "Enable eBPF observability (ebpf_exporter) for this job",
      0, 0, _opt_handler },
    SPANK_OPTIONS_TABLE_END
};

static int _opt_handler(int val, const char *optarg, int remote)
{
    ebpf_requested = 1;
    return 0;
}

/* ── Context-independent detection of the mode and of --ebpf ───────────────
 *
 * A previous version relied on static globals set in slurm_spank_init() and
 * _opt_handler(). That is NOT reliable: those callbacks run in other
 * processes/contexts (allocator/task), and their state is not carried into
 * the S_CTX_JOB_SCRIPT context where the prolog and epilog run. Verified on
 * Slurm 25.11: inside the prolog av[0]=="mode=opt-in" is present, yet the
 * static mode flag was 0 because slurm_spank_init() never ran there.
 *
 * So the mode is re-read from the local av[] (guaranteed to be passed to
 * every callback), and --ebpf is detected from what actually reaches the
 * prolog/epilog context.
 */
static int plugin_mode_opt_in(int ac, char **av)
{
    int i;
    for (i = 0; i < ac; i++) {
        if (strncmp(av[i], "mode=", 5) == 0)
            return strcmp(av[i] + 5, "opt-in") == 0;
    }
    return 0; /* default: always */
}

/* Env var Slurm exports to prolog/epilog scripts when a SPANK option was set,
 * named SPANK__SLURM_SPANK_OPTION_<plugin>_<option>. Here the plugin is
 * "spank_ebpf" and the option is "ebpf". */
#define EBPF_OPT_ENV "SPANK__SLURM_SPANK_OPTION_spank_ebpf_ebpf"

/*
 * job_requested_ebpf() — did the user ask for eBPF on this job?
 *
 * spank_option_getopt() is documented (slurm/spank.h) as valid from
 * slurm_spank_job_prolog/epilog, but it does NOT resolve the option there on
 * Slurm 25.11 (both the OpenHPC 25.11.4 and hpck.it 25.11.5 el8 builds used
 * here): it returns ESPANK_ERROR ("option wasn't used") for a job where the
 * option demonstrably WAS used. Proof: with --ebpf given an argument
 * (has_arg=1, e.g. `srun --ebpf=1`), a debug build logged
 * `spank_option_getopt() == 3000` (ESPANK_ERROR, per
 * /usr/include/slurm/slurm_errno.h) in the very same prolog call where
 * getenv(EBPF_OPT_ENV) read "1" — i.e. Slurm's own env-based transport had
 * the value while the documented API call to read it did not. Switching the
 * option to take an argument did not fix spank_option_getopt() in this
 * context, so the option is kept as a plain flag (has_arg=0, simpler for
 * users) and step 1 below is kept only as a cheap first check that costs
 * nothing when it fails. SchedMD's own changelog (slurm-25.11.md, "Changes
 * in 25.11.3": "Avoid failure for spank options that do not require
 * arguments") shows this general area — has_arg=0 options carried across
 * contexts — has had recent, real bugs; what's covered here is not.
 *
 * Client support, verified live with both `srun --ebpf ...` and
 * `sbatch --ebpf ...` (the latter requires spank_option_register() in
 * slurm_spank_init(), see there): both reach the prolog through EBPF_OPT_ENV,
 * including two overlapping `sbatch --ebpf` jobs on the same node (reference
 * counter goes 0->1->2->1->0 correctly). A batch job's prolog does not see
 * the user's shell environment via spank_getenv(), so plain
 * `export SLURM_EBPF=1` in a batch script does NOT reach it; step 3 below
 * only helps in contexts where the job environment is exposed.
 */
static int job_requested_ebpf(spank_t sp)
{
    char *optarg = NULL;

    /* 1) Allocator/task context: the API resolves the option value there.
     *    Not relied upon in job_script context, see the comment above. */
    if (spank_option_getopt(sp, &spank_options[0], &optarg) == ESPANK_SUCCESS)
        return 1;

    /* 2) Prolog/epilog context: Slurm forwards the option as an env var. */
    if (getenv(EBPF_OPT_ENV) != NULL)
        return 1;

    /* 3) Best-effort user-facing switch: SLURM_EBPF=1 in the job environment
     *    (e.g. `export SLURM_EBPF=1` in a batch script). Effective only where
     *    the prolog/epilog context exposes the job environment. */
    {
        char buf[8] = {0};
        if (spank_getenv(sp, "SLURM_EBPF", buf, sizeof(buf)) == ESPANK_SUCCESS
            && (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y'))
            return 1;
    }

    /* 4) Fallback: global set by _opt_handler in the same process. */
    return ebpf_requested;
}

/* ── Reference counter, guarded by flock() ────────────────────────────── */

static int refcount_read(void)
{
    FILE *f = fopen(REFCOUNT_FILE, "r");
    int count = 0;
    if (f) {
        if (fscanf(f, "%d", &count) != 1)
            count = 0;
        fclose(f);
    }
    return count;
}

static int refcount_write(int count)
{
    FILE *f = fopen(REFCOUNT_FILE, "w");
    if (!f) {
        slurm_error("spank_ebpf: cannot write %s: %s",
                     REFCOUNT_FILE, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", count);
    fclose(f);
    return 0;
}

/*
 * refcount_change() — increment (+1) or decrement (-1) the counter under an
 * exclusive lock. Returns the new value, or -1 on error.
 */
static int refcount_change(int delta)
{
    int fd, count, new_count;

    fd = open(REFCOUNT_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        slurm_error("spank_ebpf: cannot open %s: %s",
                     REFCOUNT_FILE, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX) < 0) {
        slurm_error("spank_ebpf: flock failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    count = refcount_read();
    new_count = count + delta;
    if (new_count < 0)
        new_count = 0;

    refcount_write(new_count);

    flock(fd, LOCK_UN);
    close(fd);

    return new_count;
}

/* ── systemctl start / stop ───────────────────────────────────────────── */

static int service_start(void)
{
    int rc;

    /* Skip a redundant systemctl call if the service is already up. */
    rc = system("systemctl is-active --quiet " SERVICE_NAME);
    if (rc == 0) {
        slurm_info("spank_ebpf: " SERVICE_NAME " already running");
        return 0;
    }

    slurm_info("spank_ebpf: starting " SERVICE_NAME);
    rc = system("systemctl start " SERVICE_NAME);
    if (rc != 0) {
        slurm_error("spank_ebpf: failed to start " SERVICE_NAME
                     " (exit code %d)", rc);
        return -1;
    }

    return 0;
}

static int service_stop(void)
{
    int rc;

    rc = system("systemctl is-active --quiet " SERVICE_NAME);
    if (rc != 0) {
        slurm_info("spank_ebpf: " SERVICE_NAME " already stopped");
        return 0;
    }

    slurm_info("spank_ebpf: stopping " SERVICE_NAME);
    rc = system("systemctl stop " SERVICE_NAME);
    if (rc != 0) {
        slurm_error("spank_ebpf: failed to stop " SERVICE_NAME
                     " (exit code %d)", rc);
        return -1;
    }

    return 0;
}

/* ── SPANK hooks ──────────────────────────────────────────────────────── */

/*
 * slurm_spank_init() — called in several contexts. Only used here to validate
 * the mode=... plugin argument and warn on an unknown value. The prolog and
 * epilog re-read the mode locally (see plugin_mode_opt_in()) rather than rely
 * on any state set here.
 */
int slurm_spank_init(spank_t sp, int ac, char **av)
{
    int i;

    /* The static spank_options[] table above is NOT loaded in ALLOCATOR
     * context (sbatch/salloc) — only in local (srun) and remote context.
     * Without this explicit call, `sbatch --ebpf` fails at the CLI with
     * "unrecognized option '--ebpf'": sbatch never even parses it as a
     * plugin option. Confirmed live: `sbatch --help` did not list --ebpf
     * until this call was added, and did afterwards. Real plugins that
     * support sbatch/salloc do the same (e.g. auks' slurm-spank-auks.c,
     * Frey's gridengine_compat.c). spank_option_register() must be called
     * from slurm_spank_init() (the only context it is valid from); calling
     * it unconditionally in every context is fine and is what auks does. */
    spank_option_register(sp, &spank_options[0]);

    for (i = 0; i < ac; i++) {
        if (strncmp(av[i], "mode=", 5) == 0) {
            const char *m = av[i] + 5;
            if (strcmp(m, "opt-in") != 0 && strcmp(m, "always") != 0)
                slurm_error("spank_ebpf: unknown mode '%s' "
                            "(valid: always, opt-in)", m);
        }
    }
    return 0;
}

/*
 * slurm_spank_job_prolog() — runs as ROOT on the compute node, BEFORE the job
 * starts. Context S_CTX_JOB_SCRIPT.
 */
int slurm_spank_job_prolog(spank_t sp, int ac, char **av)
{
    uint32_t jobid = 0;
    int new_count;

    /* opt-in mode: do nothing unless the job requested --ebpf. Mode and
     * option are re-read locally, see plugin_mode_opt_in(). */
    if (plugin_mode_opt_in(ac, av) && !job_requested_ebpf(sp))
        return 0;

    spank_get_item(sp, S_JOB_ID, &jobid);
    slurm_info("spank_ebpf: prolog job %u", jobid);

    new_count = refcount_change(+1);
    if (new_count < 0)
        return -1;

    slurm_info("spank_ebpf: refcount now %d", new_count);

    if (new_count == 1) {
        /* First job on this node: start the exporter. */
        return service_start();
    }

    /* The exporter is already running for another job. */
    return 0;
}

/*
 * slurm_spank_job_epilog() — runs as ROOT on the compute node, AFTER the job
 * ends (including on failure or cancellation).
 */
int slurm_spank_job_epilog(spank_t sp, int ac, char **av)
{
    uint32_t jobid = 0;
    int new_count;

    /* Same guard as the prolog: otherwise the epilog would decrement a
     * counter the prolog never incremented (opt-in without --ebpf). */
    if (plugin_mode_opt_in(ac, av) && !job_requested_ebpf(sp))
        return 0;

    spank_get_item(sp, S_JOB_ID, &jobid);
    slurm_info("spank_ebpf: epilog job %u", jobid);

    new_count = refcount_change(-1);
    if (new_count < 0)
        return -1;

    slurm_info("spank_ebpf: refcount now %d", new_count);

    if (new_count == 0) {
        /* No job left on this node: stop the exporter. */
        return service_stop();
    }

    /* Other jobs are still using the exporter. */
    return 0;
}
