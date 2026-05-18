/*
 * init.c - main entry point
 *
 * Replaces the Bedrock shell init with a C program that:
 *   1. Sets essential environment and PATH
 *   2. Shows a first-boot boot menu with fallback option
 *   3. Sets up essential mounts (direct syscalls, no forks)
 *   4. Presents an interactive stratum/init selection menu
 *   5. Pivots root into the chosen stratum
 *   6. Enables ALL strata IN PARALLEL via fork()+waitpid()
 *   7. Validates critical paths before exec
 *   8. Execs the real init — this process is gone, real init owns PID 1
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

/* Validate that a path exists, is not a broken symlink, and is executable.
 * Returns 1 if OK, 0 if broken/missing. */
static int validate_binary(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;
    if (S_ISLNK(st.st_mode)) {
        char target[MAX_PATH_LEN];
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len < 0 || len >= (ssize_t)sizeof(target))
            return 0;
        target[len] = '\0';
        if (access(target, X_OK) != 0)
            return 0;
    }
    if (access(path, X_OK) != 0)
        return 0;
    return 1;
}

/* Validate bedrock base environment is present and usable. */
static int validate_bedrock_base(void)
{
    struct stat st;
    if (lstat(BEDROCK_ROOT "/strata/bedrock", &st) < 0)
        return 0;
    if (S_ISLNK(st.st_mode)) {
        notice(COL_YELLOW "warn" COL_RESET ": " BEDROCK_ROOT "/strata/bedrock is a symlink");
    }
    return 1;
}

/* Safe fallback: try to exec /sbin/init (the protected backup). */
static void try_fallback_init(char **argv)
{
    static const char *fallback_candidates[] = {
        "/sbin/init",
        NULL
    };

    for (int i = 0; fallback_candidates[i]; i++) {
        if (validate_binary(fallback_candidates[i])) {
            notice("Attempting fallback: %s", fallback_candidates[i]);
            execv(fallback_candidates[i], argv);
        }
    }

    panic("No fallback init found — system cannot boot");
}

int main(int argc, char **argv)
{
    (void)argc;

    InitState st;

    /* Safety: if not PID 1, exec backup init */
    check_pid1(argv);

    /* Zero out state (uses ASM helper) */
    asm_memzero(&st, sizeof(st));

    /* 1. Set PATH explicitly — critical for PID 1 where env may be empty */
    setenv("PATH", DEFAULT_PATH, 1);

    /* 2. Essential mounts, terminal, logo */
    ensure_essential_environment();
    setup_term();
    print_logo();

    /* 3. Validate bedrock base before proceeding */
    if (!validate_bedrock_base()) {
        notice(COL_RED "error" COL_RESET ": bedrock base environment broken");
        try_fallback_init(argv);
    }

    /* 5. Parse bedrock.conf */
    if (parse_bedrock_conf(&st) < 0)
        panic("Failed to parse " BEDROCK_CONF);

    /* 6. Scan available strata */
    if (scan_strata(&st) < 0)
        panic("Failed to scan strata in " STRATA_DIR);

    /* 7. Resolve which init to use */
    char forced_tuple[MAX_PATH_LEN] = {0};
    int  from_cmdline = cmdline_init_tuple(forced_tuple, sizeof(forced_tuple));

    if (from_cmdline) {
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
        if (st.init_index < 0) {
            notice(COL_RED "error" COL_RESET ": bedrock_init= stratum '%s' not found", kstrat);
            try_fallback_init(argv);
        }
    } else {
        st.init_index = run_menu(&st);
        if (st.init_index < 0) {
            notice(COL_RED "error" COL_RESET ": No init selected");
            try_fallback_init(argv);
        }
        strncpy(st.init_cmd,
                st.strata[st.init_index].init_cmd,
                MAX_PATH_LEN - 1);
    }

    Stratum *init_s = &st.strata[st.init_index];

    /* 8. Mount fstab (dmsetup, lvm, mount -a equivalent) */
    step(1, 6, "Mounting " COL_CYAN "fstab" COL_RESET);
    mount_fstab();

    /* 9. pivot_root into the chosen stratum */
    step(2, 6, "Pivoting to " COL_GREEN "%s" COL_RESET, init_s->name);
    pivot_root_to(init_s->root);

    /* 10. Pre-enable shared mounts */
    step(3, 6, "Preparing to enable");
    preenable_mounts(init_s->root);

    /* 11. Enable ALL strata in parallel */
    step(4, 6, "Enabling " COL_YELLOW "strata" COL_RESET " (parallel)");
    enable_strata_parallel(&st);

    /* 12. Apply configuration (must fork — if brl-apply ran as PID 1
     * and exited, the kernel would panic) */
    step(5, 6, "Applying configuration");
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/brl-apply",
                  (char *[]){ "brl-apply", "--skip-repair", NULL });
            _exit(127);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* 13. Hand off to the real init */
    step(6, 6, "Handing control to " COL_GREEN "%s" COL_RESET ":" COL_CYAN "%s" COL_RESET,
         init_s->name, st.init_cmd);

    if (!validate_binary(st.init_cmd)) {
        notice(COL_RED "error" COL_RESET ": Chosen init '%s' is not executable",
               st.init_cmd);
        try_fallback_init(argv);
    }

    execv(st.init_cmd, argv);

    panic("exec of '%s' failed: %s", st.init_cmd, strerror(errno));
    return 1;
}
