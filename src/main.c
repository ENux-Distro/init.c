/*
 * init.c - main entry point
 *
 * Replaces the Bedrock shell init with a C program that:
 *   1. Sets up the essential mount environment (direct syscalls, no forks)
 *   2. Presents an interactive stratum/init selection menu
 *   3. Pivots root into the chosen stratum
 *   4. Enables ALL strata IN PARALLEL via fork()+waitpid()
 *   5. Execs the real init — this process is gone, real init owns PID 1
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "init.h"

/* If we are not PID 1, hand off to the real (backup) init immediately. */
static void check_pid1(char **argv)
{
    if (getpid() != 1) {
        execv("/sbin/init-bedrock-backup", argv);
        /* execv only returns on error */
        panic("Not PID 1 and backup init exec failed: %s", strerror(errno));
    }
}

/* Parse bedrock_init= from /proc/cmdline.
 * Returns 1 and fills buf if found, 0 otherwise. */
static int cmdline_init_tuple(char *buf, size_t bufsz)
{
    char line[4096];
    int  fd, n;
    char *p, *end;

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;

    n = (int)read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) return 0;
    line[n] = '\0';

    p = strstr(line, "bedrock_init=");
    if (!p) return 0;

    p += strlen("bedrock_init=");
    end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n') end++;

    if ((size_t)(end - p) >= bufsz) return 0;
    memcpy(buf, p, (size_t)(end - p));
    buf[end - p] = '\0';
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;

    InitState st;

    /* Safety: if not PID 1, exec backup init*/
    check_pid1(argv);

    /* Zero out state (uses ASM helper) */
    asm_memzero(&st, sizeof(st));

    /* 2. Essential mounts, terminal, logo */
    ensure_essential_environment();
    setup_term();
    print_logo();

    /* 3. Parse bedrock.conf */
    if (parse_bedrock_conf(&st) < 0)
        panic("Failed to parse " BEDROCK_CONF);

    /* 4. Scan available strata */
    if (scan_strata(&st) < 0)
        panic("Failed to scan strata in " STRATA_DIR);

    /* 5. Resolve which init to use */
    char forced_tuple[MAX_PATH_LEN] = {0};
    int  from_cmdline = cmdline_init_tuple(forced_tuple, sizeof(forced_tuple));

    if (from_cmdline) {
        /* Parse "stratum:cmd" from kernel cmdline */
        char *colon = strchr(forced_tuple, ':');
        if (!colon)
            panic("bedrock_init= on cmdline has no colon: %s", forced_tuple);

        *colon = '\0';
        const char *kstrat = forced_tuple;
        const char *kcmd   = colon + 1;

        st.init_index = -1;
        for (int i = 0; i < st.n_strata; i++) {
            if (asm_strcmp(st.strata[i].name, kstrat) == 0) {
                st.init_index = i;
                strncpy(st.init_cmd, kcmd, MAX_PATH_LEN - 1);
                break;
            }
        }
        if (st.init_index < 0)
            panic("bedrock_init= stratum '%s' not found", kstrat);
    } else {
        /* Interactive menu (with optional timeout) */
        st.init_index = run_menu(&st);
        if (st.init_index < 0)
            panic("No init selected");
        strncpy(st.init_cmd,
                st.strata[st.init_index].init_cmd,
                MAX_PATH_LEN - 1);
    }

    Stratum *init_s = &st.strata[st.init_index];

    /* 6. Mount fstab (dmsetup, lvm, mount -a equivalent) */
    step(1, 6, "Mounting " COL_CYAN "fstab" COL_RESET);
    mount_fstab();

    /* 7. pivot_root into the chosen stratum */
    step(2, 6, "Pivoting to " COL_GREEN "%s" COL_RESET, init_s->name);
    pivot_root_to(init_s->root);

    /* 8. Pre-enable shared mounts */
    step(3, 6, "Preparing to enable");
    preenable_mounts(init_s->root);

    /* 9. Enable ALL strata in paralel*/
    step(4, 6, "Enabling " COL_YELLOW "strata" COL_RESET " (parallel)");
    enable_strata_parallel(&st);

    /* 10. Apply configuration */
    step(5, 6, "Applying configuration");
    execv("/bedrock/libexec/brl-apply",
          (char *[]){ "brl-apply", "--skip-repair", NULL });
    /* brl-apply is expected to return; if it exec-replaced us that's a bug */
    /* so we call it via fork instead: */
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/brl-apply",
                  (char *[]){ "brl-apply", "--skip-repair", NULL });
            _exit(127);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* 11. Hand off to the real init*/
    step(6, 6, "Handing control to " COL_GREEN "%s" COL_RESET ":" COL_CYAN "%s" COL_RESET,
         init_s->name, st.init_cmd);

    /* Verify the binary is executable before we try to exec it */
    if (access(st.init_cmd, X_OK) != 0)
        panic("Chosen init '%s' is not executable: %s",
              st.init_cmd, strerror(errno));

    /*
     * The critical exec: after this line, we are GONE.
     * The real init takes over PID 1.
     * If this returns, the kernel will panic — which is correct behavior.
     */
    execv(st.init_cmd, argv);

    /* Should never reach here */
    panic("exec of '%s' failed: %s", st.init_cmd, strerror(errno));
    return 1; /* unreachable, satisfies compiler */
}
