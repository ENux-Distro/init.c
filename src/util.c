/*
 * init.c - utility functions
 *
 * Output helpers, fatal-error paths, and the fork+execve wrapper used for
 * every external binary.  No system(), no popen().
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "init.h"

/*
 * Boot timing instrumentation
 *
 * Off by default; enabled by adding `enux_timing` to the kernel command
 * line.  CLOCK_MONOTONIC, millisecond resolution.  timing_mark() prints
 * elapsed-since-start so each phase is timed independently of the
 * interactive menu wait.
 */
int timing_enabled = 0;

void timing_init(void)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strstr(buf, "enux_timing"))
        timing_enabled = 1;
}

unsigned long timing_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL +
           (unsigned long)ts.tv_nsec / 1000000UL;
}

void timing_mark(const char *label, unsigned long start_ms)
{
    if (!timing_enabled)
        return;
    char line[256];
    snprintf(line, sizeof(line),
             COL_DIM "  [+%5lu ms] %s" COL_RESET "\n",
             timing_now_ms() - start_ms, label);
    asm_write_str(STDOUT_FILENO, line);
}

/*
 * panic()
 *
 * Mirrors the shell's fatal_error(): print the message, then drop to an
 * emergency shell.  Used for failures after pivot_root, where re-running
 * the Bedrock shell init would redo mounts against a half-pivoted world.
 * Output goes through asm_write_str (raw write(2)) so it works even with
 * corrupted stdio state.
 */
void panic(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    asm_write_str(STDERR_FILENO, "\n" COL_RED "FATAL: " COL_RESET);
    asm_write_str(STDERR_FILENO, buf);
    asm_write_str(STDERR_FILENO, "\n\nStarting emergency shell\nGood luck\n\n");

    /* busybox dispatches on argv[0], so exec it with argv[0] = "sh" */
    execv("/bedrock/libexec/busybox", (char *[]){ "sh", NULL });
    execv("/bin/sh", (char *[]){ "sh", NULL });
    execv("/sbin/sh", (char *[]){ "sh", NULL });

    /* Nothing left to run as PID 1 — exiting causes a kernel panic,
     * which is the honest outcome at this point. */
    asm_write_str(STDERR_FILENO, "No shell available\n");
    _exit(1);
}

/*
 * is_self()
 *
 * Returns 1 if `path` refers to the same executable as the currently
 * running process (by comparing device + inode), 0 on error or mismatch.
 * Guards against exec loops when a fallback candidate is our own binary.
 */
int is_self(const char *path)
{
    struct stat a, b;
    if (stat("/proc/self/exe", &a) < 0 || stat(path, &b) < 0)
        return 0;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/*
 * fallback_to_bedrock_init()
 *
 * Pre-pivot fatal path: hand the boot to the original Bedrock shell init
 * or any available backup init.  Tries multiple known paths so we don't
 * hang PID 1 if the primary path was overwritten or is missing.
 *
 * BEDROCK_SHELL_INIT is checked first but skipped if it points to our
 * own binary (to avoid an exec loop when init.c was placed at or linked
 * from the shell init's path).
 */
void fallback_to_bedrock_init(char **argv)
{
    static const char *candidates[] = {
        BEDROCK_SHELL_INIT,
        NOT_PID1_BACKUP,
        "/bedrock/strata/bedrock/sbin/init.bin.bak",
        "/sbin/init-bedrock-backup",
        "/sbin/init",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (is_self(candidates[i]))
            continue;
        if (access(candidates[i], X_OK) == 0) {
            asm_write_str(STDERR_FILENO,
                          "\nFalling back to ");
            asm_write_str(STDERR_FILENO, candidates[i]);
            asm_write_str(STDERR_FILENO, "\n\n");
            argv[0] = (char *)candidates[i];
            execv(candidates[i], argv);
        }
    }
    panic("no backup bedrock init available");
}

void notice(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    asm_write_str(STDOUT_FILENO, COL_CYAN "* " COL_RESET);
    asm_write_str(STDOUT_FILENO, buf);
    asm_write_str(STDOUT_FILENO, "\n");
}

void warn(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    asm_write_str(STDOUT_FILENO, COL_YELLOW "warn" COL_RESET ": ");
    asm_write_str(STDOUT_FILENO, buf);
    asm_write_str(STDOUT_FILENO, "\n");
}

void step(int n, int total, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char line[1200];
    snprintf(line, sizeof(line),
             COL_DIM "[%d/%d]" COL_RESET " %s\n", n, total, buf);
    asm_write_str(STDOUT_FILENO, line);
}

/*
 * joincat()
 *
 * Bounded string concatenation for building paths.  Unlike snprintf
 * chains, overflow is an explicit, checkable failure.
 */
int joincat(char *out, size_t outsz, ...)
{
    va_list ap;
    size_t  used = 0;

    out[0] = '\0';
    va_start(ap, outsz);
    const char *part;
    while ((part = va_arg(ap, const char *)) != NULL) {
        size_t len = strlen(part);
        if (used + len >= outsz) {
            va_end(ap);
            out[0] = '\0';
            return -1;
        }
        memcpy(out + used, part, len + 1);
        used += len;
    }
    va_end(ap);
    return 0;
}

/*
 * run_cmd()
 *
 * fork + execve + waitpid for one external binary.  argv[0] must be the
 * absolute path.  If `silent`, the child's stdout/stderr go to /dev/null.
 *
 * Returns the child's exit status (0-255), 127 if exec failed, or -1 if
 * fork/waitpid itself failed or the child died from a signal.
 */
int run_cmd(char *const argv[], int silent)
{
    pid_t pid = fork();
    if (pid < 0) {
        warn("fork for %s: %s", argv[0], strerror(errno));
        return -1;
    }

    if (pid == 0) {
        if (silent) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                if (dup2(fd, STDOUT_FILENO) < 0 ||
                    dup2(fd, STDERR_FILENO) < 0)
                    _exit(127);
                close(fd);
            }
        }
        execv(argv[0], argv);
        _exit(127);
    }

    int ws;
    for (;;) {
        pid_t r = waitpid(pid, &ws, 0);
        if (r == pid)
            break;
        if (r < 0 && errno == EINTR)
            continue;
        warn("waitpid for %s: %s", argv[0], strerror(errno));
        return -1;
    }

    if (WIFEXITED(ws))
        return WEXITSTATUS(ws);
    return -1;
}
