/* init.c
 * init.h — shared types, constants, and prototypes
 */

#ifndef ENUX_INIT_H
#define ENUX_INIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_STRATA        16
#define MAX_PATH_LEN      512
#define MAX_NAME_LEN      64
#define MAX_INIT_PATHS    16

#define BEDROCK_ROOT      "/bedrock"
#define STRATA_DIR        "/bedrock/strata"
#ifndef BEDROCK_CONF /* overridable for host-side testing */
#define BEDROCK_CONF      "/bedrock/etc/bedrock.conf"
#endif
#define BEDROCK_RELEASE   "/bedrock/etc/bedrock-release"

/* The original Bedrock Linux shell init.  Must remain intact at this path;
 * the dual-boot selector execs it directly and the install procedure never
 * overwrites it (see INSTALL.md). */
#define BEDROCK_SHELL_INIT "/bedrock/strata/bedrock/sbin/init"

/* Primary backup path for the original init, created by `make install`.
 * The C code tries multiple paths (this one, .bin.bak, /sbin/init-bedrock-backup)
 * before giving up, so a missing .bak file won't hang PID 1. */
#define NOT_PID1_BACKUP    "/bedrock/strata/bedrock/sbin/init.sh.bak"

/* First-install flag file; presence means complete_hijack() must run. */
#define HIJACK_FLAG        "/bedrock/complete-hijack-install"

/* Dual-boot selector: seconds before defaulting to the ENux init. */
#define ENUX_SELECTOR_TIMEOUT 5

#define COL_RESET   "\033[0m"
#define COL_RED     "\033[1;31m"
#define COL_GREEN   "\033[1;32m"
#define COL_YELLOW  "\033[1;33m"
#define COL_CYAN    "\033[1;36m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"

#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

typedef struct {
    char name[MAX_NAME_LEN];       /* stratum name, e.g. "arch"      */
    char root[MAX_PATH_LEN];       /* /bedrock/strata/<name>         */
    int  show_init;                /* user.bedrock.show_init present */
    int  show_boot;                /* user.bedrock.show_boot present */
} Stratum;

typedef struct {
    Stratum strata[MAX_STRATA];
    int     n_strata;
    int     init_index;                  /* chosen stratum index            */
    char    init_cmd[MAX_PATH_LEN];      /* cmd to exec post-pivot, as
                                          * configured (e.g. /sbin/init)    */
    int     timeout;                     /* menu timeout; -1 = wait forever */
    char    def_stratum[MAX_NAME_LEN];   /* dereferenced default stratum    */
    char    def_cmd[MAX_PATH_LEN];       /* default cmd from bedrock.conf   */
    char    def_path[MAX_PATH_LEN];      /* resolved default init path
                                          * (pre-pivot), "" if invalid      */
} InitState;

/* env.c */
void ensure_essential_environment(void);
void setup_term(void);

/* mount.c */
void mount_fstab(void);
void pivot_root_to(const char *stratum_root);
void preenable_mounts(void);
int  mkdirp(const char *path, mode_t mode);

/* menu.c */
void print_logo(void);
void run_enux_selector(char **argv);
int  run_menu(InitState *st);   /* returns chosen stratum index, -1 on fail */

/* hijack.c */
void maybe_complete_hijack(void);
void complete_upgrade(void);

/* util.c */
void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));
void fallback_to_bedrock_init(char **argv) __attribute__((noreturn));
void notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void step(int n, int total, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Boot timing instrumentation.  Disabled unless `enux_timing` is present
 * on the kernel command line, so production boots stay quiet.  Throwaway
 * scaffolding for the brl-in-C port decision; remove once measured. */
extern int    timing_enabled;
void          timing_init(void);
unsigned long timing_now_ms(void);
void          timing_mark(const char *label, unsigned long start_ms);
int  run_cmd(char *const argv[], int silent);
int  is_self(const char *path);
/* Concatenate NUL-terminated strings into out; the list ends with NULL.
 * Returns 0, or -1 (out set to "") if the result would not fit. */
int  joincat(char *out, size_t outsz, ...) __attribute__((sentinel));

/* asm_util.asm (linked in) */
extern void asm_memzero(void *dst, size_t n);
extern int  asm_strcmp(const char *a, const char *b);
extern void asm_write_str(int fd, const char *s);   /* async-signal-safe */
/* Poll fd for input up to timeout_ms (-1 = forever), then read one byte.
 * Returns the byte (0-255), -1 on timeout, -2 on error/EOF.
 * Pure syscalls (poll + read), async-signal-safe, EINTR-retrying. */
extern int  asm_poll_read(int fd, int timeout_ms);
/* nanosleep wrapper, EINTR-retrying.  Pure syscall. */
extern void asm_usleep(unsigned long usec);

#endif /* ENUX_INIT_H */
