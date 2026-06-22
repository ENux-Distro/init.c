/* init.c
 * init.h — shared types, constants, and prototypes
 *
 * init.c is the ENux pre-init: it runs as PID 1 on the ENux base system,
 * mounts the essential filesystems, brings up the installed layers (via
 * /enux/libexec/layer-enable, so they are ready for `layer enter`), and
 * then execs the base system's real init. It does not pivot into a layer.
 */

#ifndef ENUX_INIT_H
#define ENUX_INIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_LAYERS        16
#define MAX_PATH_LEN      512
#define MAX_NAME_LEN      64

#define ENUX_ROOT         "/enux"
#define LAYER_DIR         "/enux/layer"
#ifndef ENUX_CONF /* overridable for host-side testing */
#define ENUX_CONF         "/enux/etc/enux.conf"
#endif
#define ENUX_RELEASE      "/enux/etc/enux-release"

/* Brings up one layer's mounts so it is usable for `layer enter`. This is
 * the same script the user-facing tooling calls; init forks it once per
 * non-base layer at boot. */
#define LAYER_ENABLE_BIN  "/enux/libexec/layer-enable"

/* Primary backup path for whatever init.c replaces, created by
 * `make install`. The non-PID-1 path tries this first, then the others in
 * util.c, so a missing backup never hangs PID 1. */
#define NOT_PID1_BACKUP    "/enux/layer/enux/sbin/init.sh.bak"

/* Default init to exec on the base once layers are up, when enux.conf does
 * not specify one. */
#define DEFAULT_BASE_INIT  "/sbin/init"

#define COL_RESET   "\033[0m"
#define COL_RED     "\033[1;31m"
#define COL_GREEN   "\033[1;32m"
#define COL_YELLOW  "\033[1;33m"
#define COL_CYAN    "\033[1;36m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"

#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

typedef struct {
    char name[MAX_NAME_LEN];       /* layer name, e.g. "arch"  */
    char root[MAX_PATH_LEN];       /* /enux/layer/<name>        */
} Layer;

typedef struct {
    Layer layers[MAX_LAYERS];
    int   n_layers;
    char  base[MAX_NAME_LEN];          /* base layer name; never chrooted */
    char  base_init[MAX_PATH_LEN];     /* init to exec on the base        */
} InitState;

/* env.c */
void ensure_essential_environment(void);
void setup_term(void);

/* layer.c */
int  scan_layers(InitState *st);            /* fill st->layers; count, -1 err */
void enable_layers(InitState *st);          /* fork layer-enable per non-base */

/* util.c */
void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));
void fallback_to_backup_init(char **argv) __attribute__((noreturn));
void notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void step(int n, int total, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int  run_cmd(char *const argv[], int silent);
int  is_self(const char *path);
int  joincat(char *out, size_t outsz, ...) __attribute__((sentinel));

/* Boot timing instrumentation. Disabled unless `enux_timing` is on the
 * kernel command line. */
extern int    timing_enabled;
void          timing_init(void);
unsigned long timing_now_ms(void);
void          timing_mark(const char *label, unsigned long start_ms);

/* asm_util.asm (linked in) */
extern void asm_memzero(void *dst, size_t n);
extern int  asm_strcmp(const char *a, const char *b);
extern void asm_write_str(int fd, const char *s);   /* async-signal-safe */

#endif /* ENUX_INIT_H */
