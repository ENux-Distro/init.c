/*
 * init.c - main entry point
 *
 * Full behavioral replacement for the Bedrock Linux shell init, plus the
 * ENux dual-boot selector.  Boot order matches the shell exactly:
 *
 *   1.  not PID 1?            exec /bedrock/strata/bedrock/sbin/init.sh.bak
 *   2.  ensure_essential_environment()
 *   3.  setup_term(), logo
 *   4.  ENux Boot Manager     (may exec the original shell init)
 *   5.  complete_hijack()     (first boot after hijack install only)
 *   6.  complete_upgrade()
 *   7.  bedrock.conf [init]   timeout / default, default resolution
 *   8.  bedrock_init= cmdline tuple, or interactive menu
 *   9.  mount fstab           (dmsetup, lvm, mount -a)
 *   10. pivot_root into the chosen stratum
 *   11. preenable shared mounts
 *   12. brl-repair bedrock + init stratum, brl-enable the rest (parallel)
 *   13. brl-apply --skip-repair
 *   14. exec the chosen init  — never returns
 *
 * Failure policy: before the pivot, fall back to the original Bedrock
 * shell init; from the pivot onward, panic() drops to an emergency shell
 * (re-running the shell init against a half-pivoted root would not work).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "init.h"
#include "bedrock_conf.h"
#include "stratum.h"

/* Parse bedrock_init=stratum:cmd from /proc/cmdline.
 * Returns 1 and fills buf if found, 0 otherwise. */
static int cmdline_init_tuple(char *buf, size_t bufsz)
{
    char line[4096];

    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    line[n] = '\0';

    char *p = strstr(line, "bedrock_init=");
    if (!p)
        return 0;
    p += strlen("bedrock_init=");

    char *end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n')
        end++;

    if ((size_t)(end - p) >= bufsz)
        return 0;
    memcpy(buf, p, (size_t)(end - p));
    buf[end - p] = '\0';
    return buf[0] != '\0';
}

/*
 * Read [init] timeout/default from bedrock.conf and resolve the default
 * tuple, mirroring the shell's startup block: dereference the stratum
 * alias, resolve the command inside the stratum, and on any failure warn
 * and fall back to wait-forever (timeout -1, no default).
 */
static void load_init_config(InitState *st)
{
    char val[MAX_PATH_LEN];

    st->timeout = 30;
    if (cfg_value("init", "timeout", val, sizeof(val)) == 0) {
        st->timeout = atoi(val);
        if (st->timeout < 0)
            st->timeout = -1;
    }

    st->def_stratum[0] = '\0';
    st->def_cmd[0]     = '\0';
    st->def_path[0]    = '\0';

    char tuple[MAX_PATH_LEN] = "";
    if (cfg_value("init", "default", tuple, sizeof(tuple)) < 0 ||
        tuple[0] == '\0') {
        st->timeout = -1;
        return;
    }

    char *colon = strchr(tuple, ':');
    if (colon) {
        *colon = '\0';
        snprintf(st->def_cmd, sizeof(st->def_cmd), "%s", colon + 1);
    }

    if (deref(tuple, st->def_stratum, sizeof(st->def_stratum)) < 0)
        st->def_stratum[0] = '\0';

    if (st->def_stratum[0] && st->def_cmd[0]) {
        char root[MAX_PATH_LEN];
        char link[MAX_PATH_LEN];
        snprintf(root, sizeof(root), STRATA_DIR "/%s", st->def_stratum);

        struct stat sb;
        if (stat(root, &sb) == 0 && S_ISDIR(sb.st_mode) &&
            stratum_realpath(root, st->def_cmd, link, sizeof(link)) == 0) {
            if (joincat(st->def_path, sizeof(st->def_path),
                        root, link, NULL) < 0 ||
                access(st->def_path, X_OK) != 0)
                st->def_path[0] = '\0';
        }
    }

    if (st->def_path[0] == '\0') {
        warn("%s [init]/default does not describe a valid "
             "stratum:init pair", BEDROCK_CONF);
        st->timeout        = -1;
        st->def_stratum[0] = '\0';
        st->def_cmd[0]     = '\0';
    }
}

int main(int argc, char **argv)
{
    (void)argc;

    static InitState st;   /* zero-initialized BSS; large struct */

    /* Not PID 1: hand off immediately, like the shell.
     * Try each candidate path in order; the Makefile's install target
     * creates .sh.bak or .bin.bak from the original init. */
    if (getpid() != 1) {
        static const char *candidates[] = {
            NOT_PID1_BACKUP,
            "/bedrock/strata/bedrock/sbin/init.bin.bak",
            "/sbin/init-bedrock-backup",
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

    ensure_essential_environment();
    setup_term();
    print_logo();

    /* ENux Boot Manager — before any state mutation, so choosing the
     * Bedrock init hands over a pristine system. */
    run_enux_selector(argv);

    /* First boot after a hijack install, then upgrade leftovers. */
    maybe_complete_hijack();
    complete_upgrade();

    /* bedrock.conf [init] */
    load_init_config(&st);

    if (scan_strata(&st) <= 0) {
        warn("no strata found in " STRATA_DIR);
        fallback_to_bedrock_init(argv);
    }

    /* Choose the init: kernel cmdline tuple wins, else the menu. */
    st.init_index = -1;

    char tuple[MAX_PATH_LEN];
    if (cmdline_init_tuple(tuple, sizeof(tuple))) {
        char *colon = strchr(tuple, ':');
        if (!colon) {
            warn("bedrock_init= has no colon: %s", tuple);
            fallback_to_bedrock_init(argv);
        }
        *colon = '\0';

        char real[MAX_NAME_LEN];
        if (deref(tuple, real, sizeof(real)) < 0)
            snprintf(real, sizeof(real), "%.*s",
                     (int)sizeof(real) - 1, tuple);

        for (int i = 0; i < st.n_strata; i++) {
            if (asm_strcmp(st.strata[i].name, real) == 0) {
                st.init_index = i;
                snprintf(st.init_cmd, sizeof(st.init_cmd),
                         "%s", colon + 1);
                break;
            }
        }
        if (st.init_index < 0) {
            warn("bedrock_init= stratum '%s' not found", tuple);
            fallback_to_bedrock_init(argv);
        }
    } else {
        st.init_index = run_menu(&st);
        if (st.init_index < 0)
            fallback_to_bedrock_init(argv);
    }

    Stratum *init_s = &st.strata[st.init_index];

    step(1, 6, "Mounting " COL_CYAN "fstab" COL_RESET);
    mount_fstab();

    step(2, 6, "Pivoting to " COL_GREEN "%s" COL_RESET, init_s->name);
    pivot_root_to(init_s->root);

    step(3, 6, "Preparing to enable");
    preenable_mounts();

    step(4, 6, "Enabling " COL_YELLOW "strata" COL_RESET);
    enable_strata_parallel(&st);

    step(5, 6, "Applying configuration");
    {
        int rc = run_cmd((char *[]){
            BEDROCK_ROOT "/libexec/brl-apply", "--skip-repair", NULL }, 0);
        if (rc != 0)
            warn("brl-apply exited %d", rc);
    }

    step(6, 6, "Handing control to " COL_GREEN "%s" COL_RESET
         ":" COL_CYAN "%s" COL_RESET, init_s->name, st.init_cmd);

    if (access(st.init_cmd, X_OK) != 0)
        panic("chosen init '%s' is not executable: %s",
              st.init_cmd, strerror(errno));

    argv[0] = st.init_cmd;
    execv(st.init_cmd, argv);

    panic("exec of '%s' failed: %s", st.init_cmd, strerror(errno));
}
