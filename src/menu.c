/*
 * init.c - interactive menus
 *
 * Two menus, in boot order:
 *
 *   run_enux_selector() — ENux Boot Manager.  Choice between continuing
 *     with this C init (default, 5s timeout) and exec'ing the original
 *     Bedrock shell init, which then owns the whole boot.  Runs before
 *     any state is mutated so the handoff is always clean.
 *
 *   run_menu() — Bedrock init selection, behaviorally matching the shell's
 *     get_init_choice(): candidates are bedrock.conf [init] paths resolved
 *     inside every show_init stratum, the [init] default is preselected,
 *     and an invalid default means wait-forever instead of countdown.
 *
 * All keyboard waiting and countdown ticking goes through asm_poll_read /
 * asm_usleep — raw poll/read/nanosleep syscalls in asm_util.asm.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "init.h"
#include "bedrock_conf.h"
#include "stratum.h"

/* VT100 */
#define VT_CLEAR        "\033[2J\033[H"
#define VT_CURSOR_HIDE  "\033[?25l"
#define VT_CURSOR_SHOW  "\033[?25h"

static void w(const char *s)
{
    asm_write_str(STDOUT_FILENO, s);
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
    char release[128] = "Bedrock Linux";
    int fd = open(BEDROCK_RELEASE, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, release, sizeof(release) - 1);
        if (n > 0) {
            release[n] = '\0';
            if (release[n - 1] == '\n')
                release[n - 1] = '\0';
        }
        close(fd);
    }

    w("\n" COL_BOLD);
    w("__          __             __\n");
    w("\\ \\_________\\ \\____________\\ \\___\n");
    w(" \\  _ \\  _\\ _  \\  _\\ __ \\ __\\   /\n");
    w("  \\___/\\__/\\__/ \\_\\ \\___/\\__/\\_\\_\\\n");
    w(COL_RESET "  " COL_CYAN);
    w(release);
    w(COL_RESET COL_DIM "  |  init.c" COL_RESET "\n\n");
}

/* Raw terminal helpers */

static struct termios saved_termios;
static int            raw_active  = 0;
static int            menu_tty_fd = -1;

/* At PID 1, stdin may not be a usable TTY; fall back to the console. */
static int open_menu_tty(void)
{
    if (menu_tty_fd >= 0)
        return menu_tty_fd;

    if (isatty(STDIN_FILENO)) {
        menu_tty_fd = STDIN_FILENO;
        return menu_tty_fd;
    }

    static const char *tty_paths[] = {
        "/dev/console", "/dev/tty0", "/dev/tty1", NULL
    };
    for (int i = 0; tty_paths[i]; i++) {
        int fd = open(tty_paths[i], O_RDWR | O_NOCTTY);
        if (fd >= 0 && isatty(fd)) {
            menu_tty_fd = fd;
            return fd;
        }
        if (fd >= 0)
            close(fd);
    }

    menu_tty_fd = STDIN_FILENO;
    return menu_tty_fd;
}

static void enter_raw(void)
{
    int fd = open_menu_tty();
    if (tcgetattr(fd, &saved_termios) < 0)
        return;
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) < 0)
        return;
    w(VT_CURSOR_HIDE);
    raw_active = 1;
}

static void leave_raw(void)
{
    if (!raw_active)
        return;
    if (tcsetattr(menu_tty_fd, TCSANOW, &saved_termios) < 0)
        warn("tcsetattr restore: %s", strerror(errno));
    w(VT_CURSOR_SHOW);
    raw_active = 0;
}

/* Read one key with a millisecond timeout via the asm poll loop.
 * For ESC, consume a "[X" continuation if one arrives promptly and map
 * arrow keys to 'A'/'B' with the high bit set. */
#define KEY_UP   0x141
#define KEY_DOWN 0x142

static int read_key(int timeout_ms)
{
    int fd = open_menu_tty();
    int c  = asm_poll_read(fd, timeout_ms);
    if (c != 0x1b)
        return c;

    int c1 = asm_poll_read(fd, 50);
    if (c1 != '[')
        return 0x1b;
    int c2 = asm_poll_read(fd, 50);
    if (c2 == 'A')
        return KEY_UP;
    if (c2 == 'B')
        return KEY_DOWN;
    return 0x1b;
}

/* ENux dual-boot selector */

static void render_selector(int sel, int remaining, int bedrock_ok)
{
    w(VT_CLEAR);
    print_logo();
    w(COL_BOLD "  ENux Boot Manager\n" COL_RESET);
    if (remaining > 0)
        wf(COL_DIM "  Booting default in %d second%s\n" COL_RESET,
           remaining, remaining == 1 ? "" : "s");
    w("\n");

    wf("  %s 1. ENux init (init.c)  " COL_DIM "[default]" COL_RESET "\n",
       sel == 0 ? COL_GREEN ">" COL_RESET : " ");
    if (bedrock_ok)
        wf("  %s 2. Bedrock Linux init  " COL_DIM "(%s)" COL_RESET "\n",
           sel == 1 ? COL_GREEN ">" COL_RESET : " ", BEDROCK_SHELL_INIT);
    else
        w("    2. " COL_DIM "Bedrock Linux init (unavailable)"
          COL_RESET "\n");

    w("\n" COL_DIM "  [1/2] choose   [up/down] move   [Enter] confirm\n"
      COL_RESET);
}

void run_enux_selector(char **argv)
{
    int bedrock_ok = (access(BEDROCK_SHELL_INIT, X_OK) == 0) &&
                     !is_self(BEDROCK_SHELL_INIT);

    enter_raw();

    int sel       = 0;
    int remaining = ENUX_SELECTOR_TIMEOUT;
    int done      = 0;

    render_selector(sel, remaining, bedrock_ok);

    while (!done) {
        /* One countdown tick: wait up to 1s for a key. */
        int c = read_key(1000);

        switch (c) {
        case -2: /* input error — fall through to timeout behavior */
        case -1: /* tick */
            if (--remaining <= 0)
                done = 1;
            else
                render_selector(sel, remaining, bedrock_ok);
            if (c == -2 && !done) {
                /* no usable input; don't spin on a broken fd */
                asm_usleep(1000 * 1000);
            }
            break;
        case '1':
            sel  = 0;
            done = 1;
            break;
        case '2':
            if (bedrock_ok) {
                sel  = 1;
                done = 1;
            }
            break;
        case KEY_UP:
            sel       = 0;
            remaining = ENUX_SELECTOR_TIMEOUT;
            render_selector(sel, remaining, bedrock_ok);
            break;
        case KEY_DOWN:
            if (bedrock_ok)
                sel = 1;
            remaining = ENUX_SELECTOR_TIMEOUT;
            render_selector(sel, remaining, bedrock_ok);
            break;
        case '\n':
        case '\r':
            done = 1;
            break;
        default:
            break;
        }
    }

    leave_raw();

    if (sel == 1 && bedrock_ok) {
        w(VT_CLEAR);
        notice("Handing off to the Bedrock Linux init");
        argv[0] = (char *)BEDROCK_SHELL_INIT;
        execv(BEDROCK_SHELL_INIT, argv);
        /* Exec failed; the C init is still a fully capable PID 1. */
        warn("exec %s: %s — continuing with init.c",
             BEDROCK_SHELL_INIT, strerror(errno));
    }

    w(VT_CLEAR);
    print_logo();
}

/* wait_for_keyboard */

static int keyboard_is_present(void)
{
    if (access(BEDROCK_ROOT "/libexec/keyboard_is_present", X_OK) != 0)
        return 1; /* helper missing: assume present, like a 0.7.7 system */
    return run_cmd((char *[]){
        BEDROCK_ROOT "/libexec/keyboard_is_present", NULL }, 1) == 0;
}

/* Slow-to-initialize USB keyboards confused users who could not type in
 * the menu.  Load configured input modules, then vocally wait up to
 * `timeout` seconds for a keyboard, polling every quarter second. */
static void wait_for_keyboard(int timeout)
{
    char mods[8][MAX_PATH_LEN];
    int  nm = cfg_values("init", "modules", mods, 8);
    for (int i = 0; i < nm; i++) {
        if (access("/sbin/modprobe", X_OK) == 0)
            run_cmd((char *[]){ "/sbin/modprobe", mods[i], NULL }, 1);
    }

    if (keyboard_is_present())
        return;

    wf("Waiting up to %d seconds for keyboard initialization...", timeout);

    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) == 0) {
        for (;;) {
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
                break;
            if (now.tv_sec - start.tv_sec >= timeout)
                break;
            if (keyboard_is_present())
                break;
            asm_usleep(250 * 1000);
        }
    }

    w("\r\033[K");
    if (!keyboard_is_present())
        notice(COL_YELLOW "WARNING: unable to detect keyboard" COL_RESET);
}

/* Init option discovery */

typedef struct {
    char stratum[MAX_NAME_LEN];
    char cmd[MAX_PATH_LEN];       /* as configured, e.g. /sbin/init   */
    char link[MAX_PATH_LEN];      /* resolved inside the stratum      */
    char full[MAX_PATH_LEN];      /* pre-pivot path for validation    */
    int  is_default;
} InitOption;

static InitOption options[MAX_STRATA * MAX_INIT_PATHS];
static int        n_options = 0;

static int option_cmp(const void *a, const void *b)
{
    const InitOption *x = a, *y = b;
    int r = asm_strcmp(x->stratum, y->stratum);
    if (r != 0)
        return r;
    return asm_strcmp(x->cmd, y->cmd);
}

/*
 * Mirrors list_init_options(): every show_init stratum (bedrock excluded;
 * aliases never appear since scan_strata skips symlinks) crossed with
 * every [init] paths entry, resolved inside the stratum, executable
 * regular files only.  Duplicate resolved paths within a stratum keep the
 * first hit, matching the upstream awk's effective behavior.
 */
static void populate_options(InitState *st)
{
    char paths[MAX_INIT_PATHS][MAX_PATH_LEN];
    int  np = cfg_values("init", "paths", paths, MAX_INIT_PATHS);

    if (np == 0) {
        /* No configured paths (broken/missing bedrock.conf).  Probe the
         * conventional locations rather than presenting nothing. */
        static const char *fallback[] = {
            "/sbin/init", "/lib/systemd/systemd",
            "/usr/lib/systemd/systemd", NULL
        };
        for (int i = 0; fallback[i] && np < MAX_INIT_PATHS; i++)
            snprintf(paths[np++], MAX_PATH_LEN, "%s", fallback[i]);
        warn("no [init] paths in %s; using built-in defaults",
             BEDROCK_CONF);
    }

    n_options = 0;

    for (int i = 0; i < st->n_strata; i++) {
        Stratum *s = &st->strata[i];

        if (asm_strcmp(s->name, "bedrock") == 0)
            continue;
        if (!s->show_init)
            continue;

        for (int j = 0; j < np; j++) {
            char link[MAX_PATH_LEN];
            if (stratum_realpath(s->root, paths[j],
                                 link, sizeof(link)) < 0)
                continue;

            char full[MAX_PATH_LEN];
            if (snprintf(full, sizeof(full), "%s%s", s->root, link)
                    >= (int)sizeof(full))
                continue;

            struct stat sb;
            if (stat(full, &sb) < 0)
                continue;
            if (!S_ISREG(sb.st_mode) || !(sb.st_mode & 0111))
                continue;

            int dup = 0;
            for (int k = 0; k < n_options; k++) {
                if (asm_strcmp(options[k].full, full) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                continue;
            if (n_options >= (int)(sizeof(options) / sizeof(options[0])))
                return;

            InitOption *o = &options[n_options];
            if (joincat(o->stratum, sizeof(o->stratum), s->name,
                        NULL) < 0 ||
                joincat(o->cmd, sizeof(o->cmd), paths[j], NULL) < 0 ||
                joincat(o->link, sizeof(o->link), link, NULL) < 0 ||
                joincat(o->full, sizeof(o->full), full, NULL) < 0)
                continue;
            o->is_default = 0;
            n_options++;
        }
    }

    qsort(options, (size_t)n_options, sizeof(options[0]), option_cmp);

    /* Star the configured default by resolved path, like the shell. */
    if (st->def_path[0]) {
        for (int i = 0; i < n_options; i++) {
            if (asm_strcmp(options[i].full, st->def_path) == 0)
                options[i].is_default = 1;
        }
    }
}

/* Init selection menu */

static void render_menu(InitState *st, int highlight, int remaining)
{
    w(VT_CLEAR);
    print_logo();

    w(COL_BOLD "  Select init for this session\n" COL_RESET);
    w(COL_DIM "  Configure default and timeout: " BEDROCK_CONF
      " [init]\n" COL_RESET);
    if (remaining > 0)
        wf(COL_DIM "  Selecting default in %d second%s\n" COL_RESET,
           remaining, remaining == 1 ? "" : "s");
    w("\n");

    for (int i = 0; i < n_options; i++) {
        InitOption *o = &options[i];
        wf("%s%s" COL_DIM "%2d." COL_RESET " " COL_GREEN "%s" COL_RESET
           ":" COL_CYAN "%s" COL_RESET,
           o->is_default ? COL_YELLOW "*" COL_RESET : " ",
           (i == highlight) ? COL_GREEN " > " COL_RESET : "   ",
           i + 1, o->stratum, o->cmd);
        if (asm_strcmp(o->cmd, o->link) != 0)
            wf(COL_DIM " -> %s" COL_RESET, o->link);
        w("\n");
    }

    w("\n" COL_DIM "  [up/down] move   [1-9] jump   [Enter] select\n"
      COL_RESET);
    (void)st;
}

/*
 * run_menu()
 *
 * Returns the chosen stratum's index in st->strata and sets st->init_cmd
 * to the *configured* command (e.g. /sbin/init) — the path is exec'd
 * post-pivot where the stratum is the root, exactly like the shell.
 * Returns -1 if nothing valid was chosen.
 */
int run_menu(InitState *st)
{
    wait_for_keyboard(st->timeout > 0 ? st->timeout : 0);

    populate_options(st);

    int have_default = (st->def_path[0] != '\0');

    if (n_options == 0 && !have_default) {
        warn("no init options found in any stratum");
        return -1;
    }

    int highlight = 0;
    for (int i = 0; i < n_options; i++) {
        if (options[i].is_default) {
            highlight = i;
            break;
        }
    }

    int chosen_default = 0;

    /* timeout 0 + valid default: boot it without showing the menu. */
    if (st->timeout == 0 && have_default) {
        chosen_default = 1;
    } else {
        /* No valid default means wait forever (shell sets timeout -1). */
        int remaining = have_default ? st->timeout : -1;

        enter_raw();
        render_menu(st, highlight, remaining);

        int done = 0;
        while (!done) {
            int c = read_key(remaining > 0 ? 1000 : -1);

            switch (c) {
            case -1: /* tick */
                if (remaining > 0 && --remaining <= 0) {
                    chosen_default = 1;
                    done = 1;
                } else {
                    render_menu(st, highlight, remaining);
                }
                break;
            case -2: /* broken input: only the default can save us */
                if (have_default) {
                    chosen_default = 1;
                    done = 1;
                } else {
                    asm_usleep(1000 * 1000);
                }
                break;
            case KEY_UP:
                if (highlight > 0)
                    highlight--;
                remaining = have_default ? st->timeout : -1;
                render_menu(st, highlight, remaining);
                break;
            case KEY_DOWN:
                if (highlight < n_options - 1)
                    highlight++;
                remaining = have_default ? st->timeout : -1;
                render_menu(st, highlight, remaining);
                break;
            case '\n':
            case '\r':
                if (n_options > 0)
                    done = 1;
                break;
            case '0':
                if (have_default) {
                    chosen_default = 1;
                    done = 1;
                }
                break;
            default:
                if (c > '0' && c <= '9' && (c - '1') < n_options) {
                    highlight = c - '1';
                    remaining = have_default ? st->timeout : -1;
                    render_menu(st, highlight, remaining);
                }
                break;
            }
        }

        leave_raw();
    }

    const char *want_stratum;
    const char *want_cmd;

    if (chosen_default) {
        want_stratum = st->def_stratum;
        want_cmd     = st->def_cmd;
    } else {
        want_stratum = options[highlight].stratum;
        want_cmd     = options[highlight].cmd;
    }

    for (int i = 0; i < st->n_strata; i++) {
        if (asm_strcmp(st->strata[i].name, want_stratum) == 0) {
            snprintf(st->init_cmd, sizeof(st->init_cmd), "%s", want_cmd);
            return i;
        }
    }

    warn("chosen stratum '%s' not found", want_stratum);
    return -1;
}
