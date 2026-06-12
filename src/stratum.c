/*
 * init.c - stratum attributes, scanning, path resolution, enablement
 *
 * C replacements for common-code's list_strata/deref/has_attr/get_attr/
 * set_attr, plus the chroot+realpath trick list_init_options uses, plus
 * the strata-enable loop — parallelized.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#include "init.h"
#include "stratum.h"

/* xattr helpers (user.bedrock.* namespace) */

static void attr_name(char *buf, size_t bufsz, const char *attr)
{
    snprintf(buf, bufsz, "user.bedrock.%s", attr);
}

int br_has_attr(const char *path, const char *attr)
{
    char name[128];
    attr_name(name, sizeof(name), attr);
    return getxattr(path, name, NULL, 0) >= 0;
}

int br_get_attr(const char *path, const char *attr, char *out, size_t outsz)
{
    char name[128];
    attr_name(name, sizeof(name), attr);
    ssize_t r = getxattr(path, name, out, outsz - 1);
    if (r < 0) {
        out[0] = '\0';
        return -1;
    }
    out[r] = '\0';
    return 0;
}

int br_set_attr(const char *path, const char *attr, const char *val)
{
    char name[128];
    attr_name(name, sizeof(name), attr);
    if (setxattr(path, name, val, strlen(val), 0) < 0) {
        warn("setxattr %s on %s: %s", name, path, strerror(errno));
        return -1;
    }
    return 0;
}

/* deref */

int deref(const char *name, char *out, size_t outsz)
{
    char path[MAX_PATH_LEN];
    char resolved[PATH_MAX];

    snprintf(path, sizeof(path), STRATA_DIR "/%s", name);
    if (!realpath(path, resolved))
        return -1;

    const char *base = strrchr(resolved, '/');
    base = base ? base + 1 : resolved;
    if (*base == '\0')
        return -1;

    snprintf(out, outsz, "%s", base);
    return 0;
}

/* Stratum scanning */

/*
 * scan_strata()
 *
 * Mirrors `find /bedrock/strata -maxdepth 1 -mindepth 1 -type d`:
 * directories only, via lstat, so alias symlinks are excluded.
 */
int scan_strata(InitState *st)
{
    DIR *d = opendir(STRATA_DIR);
    if (!d) {
        warn("cannot open %s: %s", STRATA_DIR, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    st->n_strata = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        if (st->n_strata >= MAX_STRATA) {
            warn("more than %d strata; ignoring %s", MAX_STRATA, ent->d_name);
            continue;
        }
        if (strlen(ent->d_name) >= MAX_NAME_LEN) {
            warn("stratum name too long; ignoring %s", ent->d_name);
            continue;
        }

        char spath[MAX_PATH_LEN];
        snprintf(spath, sizeof(spath), STRATA_DIR "/%s", ent->d_name);

        struct stat sb;
        if (lstat(spath, &sb) < 0)
            continue;
        if (!S_ISDIR(sb.st_mode))   /* symlinks are aliases — skip */
            continue;

        Stratum *s = &st->strata[st->n_strata];
        asm_memzero(s, sizeof(*s));

        snprintf(s->name, sizeof(s->name), "%.*s",
                 (int)sizeof(s->name) - 1, ent->d_name);
        snprintf(s->root, sizeof(s->root), "%s", spath);
        s->show_init = br_has_attr(spath, "show_init");
        s->show_boot = br_has_attr(spath, "show_boot");

        st->n_strata++;
    }
    closedir(d);
    return st->n_strata;
}

/* In-stratum path resolution */

/*
 * stratum_realpath()
 *
 * Resolves `path` component by component as if chrooted into `root`:
 * absolute symlink targets restart from the stratum root, not the real
 * root.  Replaces the shell's per-candidate
 *     mount proc + chroot + busybox realpath + umount
 * with pure lstat/readlink — no forks, no transient proc mounts.
 *
 * Every component must exist (matching `realpath` without -m).
 */
int stratum_realpath(const char *root, const char *path,
                     char *out, size_t outsz)
{
    char rest[MAX_PATH_LEN * 2];   /* unprocessed components  */
    char res[MAX_PATH_LEN];        /* resolved, root-relative */
    int  links = 0;

    if (snprintf(rest, sizeof(rest), "%s", path) >= (int)sizeof(rest))
        return -1;
    res[0] = '\0';

    char *r = rest;
    while (*r) {
        while (*r == '/')
            r++;
        if (*r == '\0')
            break;

        /* Extract next component. */
        char comp[MAX_NAME_LEN * 4];
        size_t cl = 0;
        while (*r && *r != '/' && cl < sizeof(comp) - 1)
            comp[cl++] = *r++;
        comp[cl] = '\0';
        if (*r && *r != '/')
            return -1; /* component longer than buffer */

        if (asm_strcmp(comp, ".") == 0)
            continue;
        if (asm_strcmp(comp, "..") == 0) {
            char *slash = strrchr(res, '/');
            if (slash)
                *slash = '\0';
            continue;
        }

        char cand[MAX_PATH_LEN];
        if (snprintf(cand, sizeof(cand), "%s/%s", res, comp)
                >= (int)sizeof(cand))
            return -1;

        char full[MAX_PATH_LEN * 2];
        snprintf(full, sizeof(full), "%s%s", root, cand);

        struct stat sb;
        if (lstat(full, &sb) < 0)
            return -1;

        if (S_ISLNK(sb.st_mode)) {
            if (++links > 40)
                return -1; /* symlink loop */

            char tgt[MAX_PATH_LEN];
            ssize_t n = readlink(full, tgt, sizeof(tgt) - 1);
            if (n < 0 || n >= (ssize_t)sizeof(tgt) - 1)
                return -1;
            tgt[n] = '\0';

            /* New work queue: target, then whatever remains. */
            char next[sizeof(rest)];
            if (snprintf(next, sizeof(next), "%s/%s", tgt, r)
                    >= (int)sizeof(next))
                return -1;
            memcpy(rest, next, sizeof(rest));
            r = rest;

            if (tgt[0] == '/')
                res[0] = '\0'; /* absolute: restart from stratum root */
            continue;
        }

        memcpy(res, cand, sizeof(res));
    }

    snprintf(out, outsz, "%s", res[0] ? res : "/");
    return 0;
}

/* Stratum enablement */

/* Open a per-stratum log file for a brl-enable child; parallel children
 * cannot share the console without interleaving garbage. */
static int open_enable_log(const char *stratum, char *logpath, size_t logsz)
{
    if (mkdirp(BEDROCK_ROOT "/run/enux-init", 0755) < 0 && errno != EEXIST)
        return -1;
    snprintf(logpath, logsz, BEDROCK_ROOT "/run/enux-init/%s.log", stratum);
    return open(logpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
}

/*
 * enable_strata_parallel()
 *
 * Shell equivalent (sequential):
 *   mkdir -p /bedrock/strata/bedrock/bedrock/run/enabled_strata
 *   touch    .../enabled_strata/bedrock
 *   ln -s /bedrock/strata/<init> .../run/init-alias
 *   touch    .../enabled_strata/<init>
 *   brl-repair --skip-crossfs bedrock
 *   brl-repair --skip-crossfs <init>
 *   for each remaining show_boot stratum: brl-enable --skip-crossfs <s>
 *
 * The two brl-repair calls are ordering-sensitive and stay sequential.
 * The brl-enable calls are independent and are forked all at once; the
 * wall clock becomes the slowest stratum instead of the sum.  brl's
 * common-code lock is a blocking flock, so concurrent calls are safe —
 * worst case they serialize on the lock.
 *
 * Failures are warned but never fatal, matching the shell's `set +e`.
 */
void enable_strata_parallel(InitState *st)
{
    Stratum *init_s = &st->strata[st->init_index];

    /* Manual bootstrap: cannot brl-enable bedrock or the init stratum.
     * Paths are post-pivot: the bedrock stratum lives at
     * /bedrock/strata/bedrock and its run dir at <that>/bedrock/run. */
    const char *enabled_dir =
        BEDROCK_ROOT "/strata/bedrock" BEDROCK_ROOT "/run/enabled_strata";
    if (mkdirp(enabled_dir, 0755) < 0 && errno != EEXIST)
        warn("mkdir %s: %s", enabled_dir, strerror(errno));

    char marker[MAX_PATH_LEN];
    snprintf(marker, sizeof(marker), "%s/bedrock", enabled_dir);
    int fd = open(marker, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        warn("create %s: %s", marker, strerror(errno));
    else
        close(fd);

    const char *init_alias =
        BEDROCK_ROOT "/strata/bedrock" BEDROCK_ROOT "/run/init-alias";
    if (unlink(init_alias) < 0 && errno != ENOENT)
        warn("unlink %s: %s", init_alias, strerror(errno));
    if (symlink(init_s->root, init_alias) < 0)
        warn("symlink %s: %s", init_alias, strerror(errno));

    snprintf(marker, sizeof(marker), "%s/%s", enabled_dir, init_s->name);
    fd = open(marker, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        warn("create %s: %s", marker, strerror(errno));
    else
        close(fd);

    /* brl-repair bedrock, then the init stratum — sequential, output
     * visible, like the shell. */
    notice("Enabling " COL_GREEN "bedrock" COL_RESET);
    int rc = run_cmd((char *[]){
        BEDROCK_ROOT "/libexec/brl-repair", "--skip-crossfs",
        "bedrock", NULL }, 0);
    if (rc != 0)
        warn("brl-repair bedrock exited %d", rc);

    notice("Enabling " COL_GREEN "%s" COL_RESET " (init stratum)",
           init_s->name);
    rc = run_cmd((char *[]){
        BEDROCK_ROOT "/libexec/brl-repair", "--skip-crossfs",
        init_s->name, NULL }, 0);
    if (rc != 0)
        warn("brl-repair %s exited %d", init_s->name, rc);

    /* brl-enable every remaining show_boot stratum, all in parallel. */
    pid_t child_pid[MAX_STRATA];
    int   child_idx[MAX_STRATA];
    char  child_log[MAX_STRATA][MAX_PATH_LEN];
    int   n_children = 0;

    for (int i = 0; i < st->n_strata; i++) {
        Stratum *s = &st->strata[i];

        if (asm_strcmp(s->name, "bedrock") == 0)
            continue;
        if (asm_strcmp(s->name, init_s->name) == 0)
            continue;
        if (!s->show_boot)
            continue;

        notice("Enabling " COL_CYAN "%s" COL_RESET " (parallel)", s->name);

        char logpath[MAX_PATH_LEN];
        pid_t pid = fork();
        if (pid < 0) {
            warn("fork for stratum %s: %s", s->name, strerror(errno));
            continue;
        }

        if (pid == 0) {
            int lfd = open_enable_log(s->name, logpath, sizeof(logpath));
            if (lfd >= 0) {
                if (dup2(lfd, STDOUT_FILENO) < 0 ||
                    dup2(lfd, STDERR_FILENO) < 0)
                    _exit(127);
                close(lfd);
            }
            execv(BEDROCK_ROOT "/libexec/brl-enable",
                  (char *[]){ BEDROCK_ROOT "/libexec/brl-enable",
                              "--skip-crossfs", s->name, NULL });
            _exit(127);
        }

        child_pid[n_children] = pid;
        child_idx[n_children] = i;
        snprintf(child_log[n_children], MAX_PATH_LEN,
                 BEDROCK_ROOT "/run/enux-init/%s.log", s->name);
        n_children++;
    }

    /* Reap until all our children are accounted for.  waitpid(-1) may
     * also hand us orphaned grandchildren reparented to PID 1 — ignore
     * any pid not in our table and keep waiting. */
    int remaining = n_children;
    while (remaining > 0) {
        int ws;
        pid_t done = waitpid(-1, &ws, 0);
        if (done < 0) {
            if (errno == EINTR)
                continue;
            warn("waitpid: %s", strerror(errno));
            break;
        }
        for (int c = 0; c < n_children; c++) {
            if (child_pid[c] != done)
                continue;
            child_pid[c] = -1;
            remaining--;
            const char *name = st->strata[child_idx[c]].name;
            if (WIFEXITED(ws) && WEXITSTATUS(ws) == 0)
                break;
            if (WIFEXITED(ws))
                warn("brl-enable %s exited %d (log: %s)",
                     name, WEXITSTATUS(ws), child_log[c]);
            else
                warn("brl-enable %s killed by signal (log: %s)",
                     name, child_log[c]);
            break;
        }
    }

    notice(COL_GREEN "All strata enabled" COL_RESET);
}
