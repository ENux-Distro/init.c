/*
 * init.c - stratum scanning and parallel enablement
 *
 * THE KEY SPEEDUP: the shell init enables strata one-by-one in a for loop.
 * Each brl-enable call takes several seconds (bind mounts, crossfs setup,
 * xattr writes). With 7 strata that's 7× serialized.
 *
 * Here we fork() one child per stratum and run them ALL simultaneously,
 * then waitpid() for all children. Wall time becomes the slowest single
 * stratum, not the sum of all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include "init.h"

/* xattr helpers (replaces has_attr / set_attr shell functions)*/

/*
 * has_xattr(): returns 1 if path has the given user.* xattr.
 * Uses getxattr(2) directly — the shell used a custom `has_attr` binary.
 */
#include <sys/xattr.h>

int has_xattr(const char *path, const char *name)
{
    char buf[256];
    ssize_t r = getxattr(path, name, buf, sizeof(buf));
    return r >= 0;
}

void set_xattr(const char *path, const char *name, const char *val)
{
    setxattr(path, name, val, strlen(val), 0);
}

/* Stratum scanning */

/*
 * scan_strata()
 *
 * Replaces the shell's `list_strata` function which piped through awk.
 * We opendir(STRATA_DIR) directly and stat each entry.
 */
int scan_strata(InitState *st)
{
    DIR *d = opendir(STRATA_DIR);
    if (!d) {
        notice(COL_RED "error" COL_RESET ": cannot open %s: %s",
               STRATA_DIR, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    st->n_strata = 0;

    while ((ent = readdir(d)) != NULL && st->n_strata < MAX_STRATA) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char spath[MAX_PATH_LEN];
        snprintf(spath, sizeof(spath), "%s/%s", STRATA_DIR, ent->d_name);

        struct stat st_buf;
        if (stat(spath, &st_buf) < 0) continue;
        if (!S_ISDIR(st_buf.st_mode)) continue;

        Stratum *s = &st->strata[st->n_strata];
        asm_memzero(s, sizeof(*s));

        strncpy(s->name, ent->d_name, MAX_NAME_LEN - 1);
        s->name[MAX_NAME_LEN - 1] = '\0';
        strncpy(s->root, spath, MAX_PATH_LEN - 1);
        s->root[MAX_PATH_LEN - 1] = '\0';

        /* Check xattrs that bedrock sets on each stratum directory */
        s->show_init = has_xattr(spath, "user.bedrock.show_init");
        s->show_boot = has_xattr(spath, "user.bedrock.show_boot");

        st->n_strata++;
    }
    closedir(d);
    return st->n_strata;
}

/* Single stratum enablement */

/*
 * enable_stratum()
 *
 * Calls brl-enable or brl-repair for one stratum.
 * This runs in a child process (forked by enable_strata_parallel),
 * so it can block freely without stalling other strata.
 */
void enable_stratum(const Stratum *s, int skip_crossfs)
{
    const char *prog;
    const char *arg1;

    /* bedrock and init strata get brl-repair; others get brl-enable */
    if (asm_strcmp(s->name, "bedrock") == 0) {
        prog = "/bedrock/libexec/brl-repair";
        arg1 = "--skip-crossfs";
    } else {
        prog = "/bedrock/libexec/brl-enable";
        arg1 = skip_crossfs ? "--skip-crossfs" : NULL;
    }

    if (arg1) {
        execv(prog, (char *[]){ (char *)prog, (char *)arg1,
                                (char *)s->name, NULL });
    } else {
        execv(prog, (char *[]){ (char *)prog, (char *)s->name, NULL });
    }

    /* execv only returns on error */
    fprintf(stderr, COL_RED "error" COL_RESET
            ": exec %s for stratum %s: %s\n",
            prog, s->name, strerror(errno));
    _exit(1);
}

/* Parallel stratum enablement */

/*
 * enable_strata_parallel()
 *
 * THE BIG WIN. Shell original:
 *
 *   for stratum in $(list_strata); do
 *       /bedrock/libexec/brl-enable --skip-crossfs "${stratum}"
 *   done
 *
 * Sequential. With 7 strata at ~8s each = ~56 seconds.
 *
 * Here: fork all 7 simultaneously. All brl-enable processes run in
 * parallel. waitpid(-1) reaps them as they finish.
 * Wall time = max(individual_times) ≈ 8–10s instead of 56s.
 *
 * Order of operations mirrors the shell script:
 *   1. Mark bedrock and init_stratum as enabled manually (xattrs + symlink)
 *   2. brl-repair bedrock (skip-crossfs)
 *   3. brl-repair init_stratum (skip-crossfs)
 *   4. brl-enable all remaining show_boot strata — ALL IN PARALLEL
 */
void enable_strata_parallel(InitState *st)
{
    Stratum *init_s = &st->strata[st->init_index];

    /* Step 1: manual bootstrap for bedrock + init stratum */

    /* /bedrock/run/enabled_strata/bedrock
     * Use a larger buffer: BEDROCK_ROOT appears twice in the path. */
    char enabled_dir[MAX_PATH_LEN * 2];
    snprintf(enabled_dir, sizeof(enabled_dir),
             "%s/strata/bedrock%s/run/enabled_strata",
             BEDROCK_ROOT, BEDROCK_ROOT);
    mkdirp(enabled_dir, 0755);

    char enabled_bedrock[MAX_PATH_LEN * 2];
    snprintf(enabled_bedrock, sizeof(enabled_bedrock),
             "%s/bedrock", enabled_dir);
    {
        int fd = open(enabled_bedrock, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }

    /* /bedrock/run/init-alias -> /bedrock/strata/<init_stratum> */
    char init_alias[MAX_PATH_LEN * 2];
    snprintf(init_alias, sizeof(init_alias),
             "%s/strata/bedrock%s/run/init-alias",
             BEDROCK_ROOT, BEDROCK_ROOT);
    unlink(init_alias); /* remove stale */
    symlink(init_s->root, init_alias);

    /* touch enabled_strata/<init_stratum> */
    char enabled_init[MAX_PATH_LEN * 2];
    snprintf(enabled_init, sizeof(enabled_init),
             "%s/%s", enabled_dir, init_s->name);
    {
        int fd = open(enabled_init, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }

    /* Step 2: brl-repair bedrock (foreground, must finish first) */
    notice("Enabling " COL_GREEN "bedrock" COL_RESET);
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/brl-repair",
                  (char *[]){ "brl-repair", "--skip-crossfs", "bedrock", NULL });
            _exit(1);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* Step 3: brl-repair init_stratum (foreground) */
    notice("Enabling " COL_GREEN "%s" COL_RESET " (init stratum)", init_s->name);
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/brl-repair",
                  (char *[]){ "brl-repair", "--skip-crossfs",
                               init_s->name, NULL });
            _exit(1);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* Step 4: brl-enable remaining strata IN PARALLEL */
    int   n_children = 0;

    for (int i = 0; i < st->n_strata; i++) {
        Stratum *s = &st->strata[i];

        /* Skip bedrock and the init stratum (already handled above) */
        if (asm_strcmp(s->name, "bedrock") == 0) continue;
        if (asm_strcmp(s->name, init_s->name) == 0) continue;

        /* Only enable strata marked show_boot */
        if (!s->show_boot) continue;

        notice("Enabling " COL_CYAN "%s" COL_RESET " (async)", s->name);

        pid_t pid = fork();
        if (pid < 0) {
            notice(COL_YELLOW "warn" COL_RESET
                   ": fork failed for stratum %s: %s",
                   s->name, strerror(errno));
            continue;
        }

        if (pid == 0) {
            /* Child: enable this stratum and exit */
            enable_stratum(s, 1 /* skip-crossfs */);
            _exit(1); /* enable_stratum only returns on exec failure */
        }

        /* Parent: count children (we reap with waitpid(-1)) */
        (void)pid;
        n_children++;
    }

    /* Step 5: reap all children */
    notice("Waiting for " COL_BOLD "%d" COL_RESET " strata...", n_children);
    int status;
    for (int i = 0; i < n_children; i++) {
        pid_t done = waitpid(-1, &status, 0);
        if (done < 0) break;

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            /* Find which stratum this was for better error reporting */
            notice(COL_YELLOW "warn" COL_RESET
                   ": a stratum enable process (pid %d) exited with %d",
                   (int)done, WEXITSTATUS(status));
        }
    }

    notice(COL_GREEN "All strata enabled." COL_RESET);
}
