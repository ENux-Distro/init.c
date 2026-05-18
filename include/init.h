/* init.c
init.h
*/

#ifndef ENUX_INIT_H
#define ENUX_INIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_STRATA        16
#define MAX_PATH_LEN      512
#define MAX_NAME_LEN      64
#define MAX_INIT_PATHS    8
#define BEDROCK_ROOT      "/bedrock"
#define STRATA_DIR        "/bedrock/strata"
#define BEDROCK_CONF      "/bedrock/etc/bedrock.conf"
#define BEDROCK_RUN       "/bedrock/run"

#define COL_RESET   "\033[0m"
#define COL_RED     "\033[1;31m"
#define COL_GREEN   "\033[1;32m"
#define COL_YELLOW  "\033[1;33m"
#define COL_CYAN    "\033[1;36m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"

#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define BEDROCK_INIT_BACKUP    "/bedrock/strata/bedrock/sbin/init.sh.bak"
#define BEDROCK_INIT_BIN_BAK   "/bedrock/strata/bedrock/sbin/init.bin.bak"

typedef struct {
    char name[MAX_NAME_LEN];       /* stratum name, e.g. "arch" */
    char root[MAX_PATH_LEN];       /* /bedrock/strata/<name>    */
    char init_path[MAX_PATH_LEN];  /* resolved init binary path */
    char init_cmd[MAX_PATH_LEN];   /* cmd as in bedrock.conf    */
    int  show_init;                /* eligible as init provider */
    int  show_boot;                /* auto-enable on boot       */
    int  enabled;                  /* set after brl-enable      */
} Stratum;

typedef struct {
    Stratum strata[MAX_STRATA];
    int     n_strata;
    int     init_index;            /* chosen stratum index      */
    char    init_cmd[MAX_PATH_LEN];
    int     timeout;               /* menu timeout seconds      */
    char    default_tuple[MAX_PATH_LEN]; /* "stratum:cmd"       */
} InitState;


/* env.c */
void ensure_essential_environment(void);
void remount_essential_after_pivot(void);
void setup_term(void);

/* mount.c */
void mount_fstab(void);
void pivot_root_to(const char *stratum_root);
void preenable_mounts(const char *init_stratum_root);

/* stratum.c */
int  scan_strata(InitState *st);
void enable_strata_parallel(InitState *st);
void enable_stratum(const Stratum *s, int skip_crossfs);

/* menu.c */
void print_logo(void);
void run_fallback_menu(char **argv);
int  run_menu(InitState *st);   /* returns chosen index */

/* conf.c */
int  parse_bedrock_conf(InitState *st);

/* util.c */
void  panic(const char *fmt, ...);
void  notice(const char *fmt, ...);
void  step(int n, int total, const char *fmt, ...);
int   xmount(const char *src, const char *tgt,
              const char *fs, unsigned long flags, const void *data);
int   mkdirp(const char *path, mode_t mode);
int   has_xattr(const char *path, const char *name);
void  set_xattr(const char *path, const char *name, const char *val);

/* asm_util.asm (linked in) */
extern void asm_memzero(void *dst, size_t n);
extern int  asm_strcmp(const char *a, const char *b);
extern void asm_write_str(int fd, const char *s);   /* async-signal-safe */

#endif /* ENUX_INIT_H */
