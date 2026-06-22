/*
 * init.c - main entry point
 *
 * The ENux pre-init. Runs as PID 1 on the ENux base system and:
 *
 *   1. not PID 1?            exec a backup init and bail
 *   2. ensure_essential_environment()  (/proc /sys /dev /run, fuse, ...)
 *   3. setup_term(), ENux logo
 *   4. read enux.conf        [layers] base, [init] base init command
 *   5. scan the layer dir    discover installed layers
 *   6. enable_layers()       fork layer-enable per non-base layer
 *   7. exec the base init    - never returns
 *
 * It does NOT pivot into a layer; layers are entered on demand at runtime
 * with `layer enter`. Failure before the exec hands the boot to a backup
 * init so PID 1 never just dies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "init.h"
#include "enux_conf.h"
#include "layer.h"

static void print_logo(void)
{
    asm_write_str(STDOUT_FILENO,
        COL_CYAN
        "\n  init.c  " COL_DIM "- Built for The ENux Layer\n" COL_RESET "\n");
}

/*
 * Read the base layer name and the init command to run on it.
 *
 *   [layers] exec_order = <base>,...   first entry is the base
 *   [init]   default     = <layer>:<cmd>   <cmd> is the base init
 *
 * Both are optional: the base defaults to "enux" and the init to
 * DEFAULT_BASE_INIT (/sbin/init).
 */
static void load_init_config(InitState *st)
{
    snprintf(st->base, sizeof(st->base), "enux");

    char order[MAX_LAYERS][MAX_PATH_LEN];
    if (cfg_values("layers", "exec_order", order, MAX_LAYERS) > 0)
        snprintf(st->base, sizeof(st->base), "%.*s",
                 (int)sizeof(st->base) - 1, order[0]);

    snprintf(st->base_init, sizeof(st->base_init), "%s", DEFAULT_BASE_INIT);

    char tuple[MAX_PATH_LEN];
    if (cfg_value("init", "default", tuple, sizeof(tuple)) == 0 &&
        tuple[0] != '\0') {
        char *colon = strchr(tuple, ':');
        if (colon && colon[1] != '\0')
            snprintf(st->base_init, sizeof(st->base_init), "%s", colon + 1);
    }
}

int main(int argc, char **argv)
{
    (void)argc;

    static InitState st;   /* zero-initialized BSS */

    /* Not PID 1: hand off immediately. */
    if (getpid() != 1) {
        static const char *candidates[] = {
            NOT_PID1_BACKUP,
            "/sbin/init-enux-backup",
            NULL
        };
        for (int i = 0; candidates[i]; i++) {
            if (access(candidates[i], X_OK) == 0) {
                argv[0] = (char *)candidates[i];
                execv(candidates[i], argv);
            }
        }
        panic("not PID 1 and no backup init available");
    }

    asm_memzero(&st, sizeof(st));

    /* PID 1 starts with an empty environment; children need a PATH. */
    if (setenv("PATH", DEFAULT_PATH, 1) < 0)
        warn("setenv PATH: %s", strerror(errno));

    timing_init();

    unsigned long t = timing_now_ms();
    ensure_essential_environment();
    timing_mark("ensure_essential_environment", t);
    setup_term();
    print_logo();

    load_init_config(&st);

    step(1, 3, "Discovering " COL_YELLOW "layers" COL_RESET);
    if (scan_layers(&st) < 0)
        warn("could not scan " LAYER_DIR "; continuing without layers");

    step(2, 3, "Enabling " COL_YELLOW "layers" COL_RESET);
    t = timing_now_ms();
    enable_layers(&st);
    timing_mark("enable_layers", t);

    step(3, 3, "Handing control to " COL_GREEN "%s" COL_RESET,
         st.base_init);

    if (access(st.base_init, X_OK) != 0) {
        warn("base init '%s' is not executable: %s",
             st.base_init, strerror(errno));
        fallback_to_backup_init(argv);
    }

    argv[0] = st.base_init;
    execv(st.base_init, argv);

    panic("exec of '%s' failed: %s", st.base_init, strerror(errno));
}
