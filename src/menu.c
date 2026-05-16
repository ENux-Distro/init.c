/*
 * init.c - interactive init selection menu
 *
 * Replaces the shell's get_init_choice() / pretty_print_options() which
 * used read -t, awk, cut, and multiple subshells per redraw.
 *
 * This implementation uses:
 *   - timerfd_create() for the countdown (no `date` calls in a loop)
 *   - select() to multiplex keyboard + timer
 *   - direct write() for VT100 rendering — no printf subprocesses
 *   - chroot(2) + execve to resolve init symlinks (replaces shell chroot)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/mount.h>
#include <stdint.h>
#include <time.h>

#include "init.h"

#define VT_CLEAR        "\033[2J\033[H"
#define VT_CURSOR_HIDE  "\033[?25l"
#define VT_CURSOR_SHOW  "\033[?25h"
#define VT_BOLD         "\033[1m"
#define VT_DIM          "\033[2m"
#define VT_RESET        "\033[0m"
#define VT_GREEN        "\033[1;32m"
#define VT_CYAN         "\033[1;36m"
#define VT_YELLOW       "\033[1;33m"
#define VT_RED          "\033[1;31m"

static void w(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void wf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    w(buf);
}

/* Logo */
void print_logo(void)
{
    /* Read the release string */
    char release[128] = "ENux";
    {
        int fd = open("/bedrock/etc/bedrock-release", O_RDONLY);
        if (fd >= 0) {
            int n = (int)read(fd, release, sizeof(release) - 1);
            if (n > 0) {
                release[n] = '\0';
                /* strip trailing newline */
                if (release[n-1] == '\n') release[n-1] = '\0';
            }
            close(fd);
        }
    }

    w("\033[90m\033[1m");

w("eeeeeeeeeeeeeeeeeeeeeeeee\n");
w("eeeeeeeeeeeeeeeeeeeeeeeee\n");
w("eeeee\n");
w("eeeee\n");
w("eeeee\n");
w("eeeeeeeeeeeeeeeeeeeeeeeee\n");
w("eeeeeeeeeeeeeeeeeeeeeeeee\n");
w("eeeee\n");
w("eeeee\n");
w("eeeee\n");
w("eeeeeeeeeeeeeeeeeeeeeeeee\n");
w("eeeeeeeeeeeeeeeeeeeeeeeee\n");

w(VT_RESET);
wf("  ENux %s\n\n", release);

}

/* Init path resolution via chroot */

/*
 * resolve_init_path()
 *
 * Replaces the shell's:
 *   mount -t proc proc "${sproc}"
 *   link="$(chroot "/bedrock/strata/${stratum}" \
 *     "/proc/1/root/bedrock/libexec/busybox" realpath "${cmd}")"
 *   umount "${sproc}"
 *
 * We do: fork → chroot → execv busybox realpath → read stdout pipe.
 * One fork per stratum per init path — but only run once at startup.
 */
static int resolve_init_path(const char *stratum_root,
                              const char *cmd,
                              char *out, size_t outsz)
{
    /* Mount proc inside stratum so chroot'd realpath works */
    char sproc[MAX_PATH_LEN];
    snprintf(sproc, sizeof(sproc), "%s/proc", stratum_root);
    mkdir(sproc, 0755);
    int mounted = (mount("proc", sproc, "proc", 0, NULL) == 0);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        if (mounted) umount(sproc);
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: chroot and run busybox realpath */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        if (chroot(stratum_root) < 0) _exit(1);
        if (chdir("/") < 0) _exit(1);

        execv("/proc/1/root/bedrock/libexec/busybox",
              (char *[]){ "busybox", "realpath", (char *)cmd, NULL });
        _exit(1);
    }

    close(pipefd[1]);

    int ret = -1;
    if (pid > 0) {
        ssize_t n = read(pipefd[0], out, outsz - 1);
        if (n > 0) {
            out[n] = '\0';
            if (out[n-1] == '\n') out[n-1] = '\0';
            ret = 0;
        }
        waitpid(pid, NULL, 0);
    }
    close(pipefd[0]);

    if (mounted) umount(sproc);
    return ret;
}

/* Populate init options */

typedef struct {
    char stratum[MAX_NAME_LEN];
    char cmd[MAX_PATH_LEN];
    char resolved[MAX_PATH_LEN];
    char full_path[MAX_PATH_LEN]; /* stratum_root + resolved */
    int  is_default;
} InitOption;

static InitOption options[MAX_STRATA * MAX_INIT_PATHS];
static int        n_options = 0;

/* Known init paths to probe (mirrors bedrock.conf [init] paths) */
static const char *known_init_paths[] = {
    "/sbin/init",
    "/bin/init",
    "/usr/sbin/init",
    "/usr/bin/init",
    "/lib/systemd/systemd",
    "/usr/lib/systemd/systemd",
    NULL
};

static void populate_options(InitState *st)
{
    n_options = 0;

    for (int i = 0; i < st->n_strata; i++) {
        Stratum *s = &st->strata[i];

        if (!s->show_init) continue;
        if (asm_strcmp(s->name, "bedrock") == 0) continue;

        for (int j = 0; known_init_paths[j]; j++) {
            const char *cmd = known_init_paths[j];

            char resolved[MAX_PATH_LEN] = {0};
            if (resolve_init_path(s->root, cmd, resolved, sizeof(resolved)) < 0)
                continue;

            char full[MAX_PATH_LEN];
            snprintf(full, sizeof(full), "%s%s", s->root, resolved);

            struct stat st_buf;
            if (stat(full, &st_buf) < 0) continue;
            if (!(st_buf.st_mode & S_IXUSR)) continue;

            /* De-duplicate by resolved path */
            int dup = 0;
            for (int k = 0; k < n_options; k++) {
                if (asm_strcmp(options[k].full_path, full) == 0) {
                    dup = 1; break;
                }
            }
            if (dup) continue;

            InitOption *opt = &options[n_options++];
            strncpy(opt->stratum,   s->name,  MAX_NAME_LEN - 1);
            strncpy(opt->cmd,       cmd,       MAX_PATH_LEN - 1);
            strncpy(opt->resolved,  resolved,  MAX_PATH_LEN - 1);
            strncpy(opt->full_path, full,      MAX_PATH_LEN - 1);
            opt->is_default = 0;

            if (n_options >= (int)(sizeof(options)/sizeof(options[0])))
                goto done_scanning;
        }
    }
done_scanning:;

    /* Mark default */
    if (st->default_tuple[0]) {
        char *colon = strchr(st->default_tuple, ':');
        if (colon) {
            *colon = '\0';
            for (int i = 0; i < n_options; i++) {
                if (asm_strcmp(options[i].stratum, st->default_tuple) == 0 &&
                    asm_strcmp(options[i].cmd, colon + 1) == 0) {
                    options[i].is_default = 1;
                }
            }
            *colon = ':';
        }
    }
}

/* Menu rendering */

static void render_menu(int highlight, int remaining_secs)
{
    w(VT_CLEAR);
    print_logo();

    w(VT_BOLD "  Select init for this session\n" VT_RESET);
    if (remaining_secs > 0) {
        wf(VT_DIM "  Auto-selecting in %d second%s  (edit /bedrock/etc/bedrock.conf [init])\n" VT_RESET,
           remaining_secs, remaining_secs == 1 ? "" : "s");
    }
    w("\n");

    for (int i = 0; i < n_options; i++) {
        InitOption *o = &options[i];
        const char *star   = o->is_default ? VT_YELLOW "*" VT_RESET : " ";
        const char *sel    = (i == highlight) ? VT_GREEN "▶ " VT_RESET : "  ";

        wf("%s%s %s%2d.%s %s%s%s:%s%s%s\n",
           star, sel,
           VT_DIM, i + 1, VT_RESET,
           VT_GREEN, o->stratum, VT_RESET,
           VT_CYAN, o->cmd, VT_RESET);
    }

    w("\n");
    w(VT_DIM "  [↑/↓] navigate   [Enter] select   [0-9] jump\n" VT_RESET);
    w(VT_DIM "  (or type number and press Enter)\n" VT_RESET);
}

/* Raw terminal mode  */

static struct termios saved_termios;

static void enter_raw(void)
{
    tcgetattr(STDIN_FILENO, &saved_termios);
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    w(VT_CURSOR_HIDE);
}

static void leave_raw(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    w(VT_CURSOR_SHOW);
}

/* ── Main menu entry point */

/*
 * run_menu()
 *
 * Replaces the shell's get_init_choice() loop which used:
 *   read -r -t "${init_timeout}"
 *   pretty_print_options
 *   list_init_options | awk ...
 *
 * We use timerfd + select() for async countdown — no `date` polling,
 * no usleep loop, no awk. Arrow key navigation is a bonus.
 *
 * Returns the index into st->strata of the chosen init.
 */
int run_menu(InitState *st)
{
    /* First, resolve all available init options */
    notice("Scanning init options...");
    populate_options(st);

    if (n_options == 0)
        panic("No init options found in any stratum");

    /* Find default highlight */
    int highlight = 0;
    for (int i = 0; i < n_options; i++) {
        if (options[i].is_default) { highlight = i; break; }
    }

    /* If timeout is 0 and there's a valid default, skip menu entirely */
    if (st->timeout == 0 && options[highlight].is_default) {
        goto selected;
    }

    /* Set up timerfd for countdown */
    int tfd = -1;
    int remaining = st->timeout;

    if (remaining > 0) {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd >= 0) {
            struct itimerspec its = {
                .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
                .it_value    = { .tv_sec = 1, .tv_nsec = 0 },
            };
            timerfd_settime(tfd, 0, &its, NULL);
        }
    }

    enter_raw();
    render_menu(highlight, remaining);

    char num_buf[8] = {0};
    int  num_len    = 0;
    int  done       = 0;

    while (!done) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = STDIN_FILENO;
        if (tfd >= 0) { FD_SET(tfd, &rfds); if (tfd > maxfd) maxfd = tfd; }

        int r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0 && errno == EINTR) continue;

        /* Timer tick */
        if (tfd >= 0 && FD_ISSET(tfd, &rfds)) {
            uint64_t exp;
            read(tfd, &exp, sizeof(exp));
            remaining -= (int)exp;

            if (remaining <= 0 && options[highlight].is_default) {
                done = 1;
                break;
            }
            render_menu(highlight, remaining > 0 ? remaining : 0);
        }

        /* Keyboard input */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            unsigned char ch;
            if (read(STDIN_FILENO, &ch, 1) != 1) break;

            /* ESC sequence (arrow keys) */
            if (ch == 0x1b) {
                unsigned char seq[2];
                /* Non-blocking peek */
                struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
                fd_set peek; FD_ZERO(&peek); FD_SET(STDIN_FILENO, &peek);
                if (select(STDIN_FILENO+1, &peek, NULL, NULL, &tv) > 0) {
                    read(STDIN_FILENO, seq, 2);
                    if (seq[0] == '[') {
                        if (seq[1] == 'A') { /* Up */
                            if (highlight > 0) highlight--;
                            remaining = st->timeout; /* reset timer on input */
                        } else if (seq[1] == 'B') { /* Down */
                            if (highlight < n_options - 1) highlight++;
                            remaining = st->timeout;
                        }
                    }
                }
                num_len = 0;
            }
            else if (ch == '\n' || ch == '\r') {
                if (num_len > 0) {
                    int n = atoi(num_buf);
                    if (n >= 1 && n <= n_options)
                        highlight = n - 1;
                    num_len = 0;
                    asm_memzero(num_buf, sizeof(num_buf));
                }
                done = 1;
            }
            else if (ch >= '0' && ch <= '9') {
                if (num_len < (int)sizeof(num_buf) - 1) {
                    num_buf[num_len++] = (char)ch;
                    num_buf[num_len]   = '\0';
                    remaining = st->timeout; /* reset timer on input */
                }
            }

            render_menu(highlight, remaining > 0 ? remaining : 0);
        }
    }

    leave_raw();
    if (tfd >= 0) close(tfd);

selected:;
    /* Write the selection back into the state */
    InitOption *chosen = &options[highlight];

    /* Find the matching stratum index */
    for (int i = 0; i < st->n_strata; i++) {
        if (asm_strcmp(st->strata[i].name, chosen->stratum) == 0) {
            strncpy(st->strata[i].init_cmd, chosen->cmd,
                    MAX_PATH_LEN - 1);
            strncpy(st->strata[i].init_path, chosen->full_path,
                    MAX_PATH_LEN - 1);
            /* Also fix the init_cmd to point to resolved path */
            strncpy(st->init_cmd, chosen->resolved, MAX_PATH_LEN - 1);
            return i;
        }
    }

    return -1;
}
