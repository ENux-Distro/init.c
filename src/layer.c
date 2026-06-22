/*
 * init.c - layer discovery and boot-time enablement
 *
 * A layer is a distribution rootfs under /enux/layer/<name>. Bringing one
 * up means bind-mounting it onto itself (so it has its own mountinfo
 * entry) and mounting /proc, /sys, /dev into it - exactly what the tested
 * /enux/libexec/layer-enable script does. init forks that script once per
 * non-base layer, in parallel, so the wall clock is the slowest layer
 * rather than the sum.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "init.h"
#include "layer.h"

/*
 * scan_layers()
 *
 * Mirrors `find /enux/layer -maxdepth 1 -mindepth 1 -type d`: real
 * directories only, via lstat, so symlink aliases are excluded.
 */
int scan_layers(InitState *st)
{
    DIR *d = opendir(LAYER_DIR);
    if (!d) {
        warn("cannot open %s: %s", LAYER_DIR, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    st->n_layers = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        if (st->n_layers >= MAX_LAYERS) {
            warn("more than %d layers; ignoring %s", MAX_LAYERS, ent->d_name);
            continue;
        }
        if (strlen(ent->d_name) >= MAX_NAME_LEN) {
            warn("layer name too long; ignoring %s", ent->d_name);
            continue;
        }

        char spath[MAX_PATH_LEN];
        snprintf(spath, sizeof(spath), LAYER_DIR "/%s", ent->d_name);

        struct stat sb;
        if (lstat(spath, &sb) < 0)
            continue;
        if (!S_ISDIR(sb.st_mode))   /* symlinks are aliases - skip */
            continue;

        Layer *l = &st->layers[st->n_layers];
        asm_memzero(l, sizeof(*l));
        snprintf(l->name, sizeof(l->name), "%.*s",
                 (int)sizeof(l->name) - 1, ent->d_name);
        snprintf(l->root, sizeof(l->root), "%s", spath);
        st->n_layers++;
    }
    closedir(d);
    return st->n_layers;
}

/*
 * enable_layers()
 *
 * Fork /enux/libexec/layer-enable for every layer except the base, all at
 * once, then reap. The base layer is the running root and is never
 * chrooted, so it is skipped. Failures are warned but never fatal - a
 * layer that fails to enable can still be brought up later with
 * `layer enable`.
 */
void enable_layers(InitState *st)
{
    if (access(LAYER_ENABLE_BIN, X_OK) != 0) {
        warn("%s missing; layers not enabled at boot", LAYER_ENABLE_BIN);
        return;
    }

    pid_t child_pid[MAX_LAYERS];
    int   child_idx[MAX_LAYERS];
    int   n_children = 0;

    for (int i = 0; i < st->n_layers; i++) {
        Layer *l = &st->layers[i];

        if (st->base[0] && asm_strcmp(l->name, st->base) == 0)
            continue;

        notice("Enabling layer " COL_CYAN "%s" COL_RESET, l->name);

        pid_t pid = fork();
        if (pid < 0) {
            warn("fork for layer %s: %s", l->name, strerror(errno));
            continue;
        }
        if (pid == 0) {
            execv(LAYER_ENABLE_BIN,
                  (char *[]){ (char *)LAYER_ENABLE_BIN, l->name, NULL });
            _exit(127);
        }

        child_pid[n_children] = pid;
        child_idx[n_children] = i;
        n_children++;
    }

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
            const char *name = st->layers[child_idx[c]].name;
            if (!(WIFEXITED(ws) && WEXITSTATUS(ws) == 0))
                warn("layer-enable %s failed", name);
            break;
        }
    }
}
