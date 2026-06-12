/*
 * init.c - native brl-enable
 *
 * Pure-C replacement for `brl-enable --skip-crossfs`, the per-stratum work
 * the shell init forks once per show_boot stratum.  See brl.h for scope and
 * the fallback contract.  Every operation the shell did via
 * `stinit busybox <cmd>` (fork -> strat -> fork -> busybox, just to reach
 * PID 1's mount namespace) is a direct syscall here: init.c is PID 1, so it
 * is already in that namespace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include "init.h"
#include "brl.h"
#include "stratum.h"
#include "bedrock_conf.h"

#define BR_STRATUM      "/bedrock/strata/bedrock"
#define ENABLED_DIR     BR_STRATUM "/bedrock/run/enabled_strata"
#define MAX_MNTS        32
#define MAX_CFG_LINES   256
#define LINE_LEN        320

int native_brl_enabled = 0;

void native_brl_init(void)
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
    if (strstr(buf, "enux_native"))
        native_brl_enabled = 1;
}

/* ------------------------------------------------------------------ */
/* small filesystem helpers                                            */
/* ------------------------------------------------------------------ */

/* mount --bind (or --rbind when rec). */
static int mnt_bind(const char *src, const char *tgt, int rec)
{
    return mount(src, tgt, NULL, MS_BIND | (rec ? MS_REC : 0), NULL);
}

/* mount --make-rshared / --make-private (propagation change only). */
static int mnt_prop(const char *tgt, unsigned long flag)
{
    return mount(NULL, tgt, NULL, flag, NULL);
}

/* Parent directory of `path` into `out` ("/a/b/c" -> "/a/b"). */
static void parent_dir(const char *path, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out)
        *slash = '\0';
    else
        snprintf(out, outsz, "/");
}

/* Last path component ("/usr/bin/sh" -> "sh"). */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Read a whole (small) file into buf.  Returns length, or -1. */
static ssize_t slurp(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += (size_t)n;
        if (total >= bufsz - 1)
            break;
    }
    close(fd);
    buf[total] = '\0';
    return (ssize_t)total;
}

/* Write `data` to `path` atomically via a "<path>-new" temp + rename. */
static int write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[MAX_PATH_LEN];
    if (joincat(tmp, sizeof(tmp), path, "-new", NULL) < 0)
        return -1;
    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return rename(tmp, path);
}

/* ------------------------------------------------------------------ */
/* string set (sorted, deduplicated) for enforce_shells                */
/* ------------------------------------------------------------------ */

struct strset {
    char items[MAX_CFG_LINES][LINE_LEN];
    int  n;
};

static void set_add(struct strset *s, const char *v)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->items[i], v) == 0)
            return;
    if (s->n < MAX_CFG_LINES)
        snprintf(s->items[s->n++], LINE_LEN, "%.*s", LINE_LEN - 1, v);
}

static int set_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void set_sort(struct strset *s)
{
    qsort(s->items, (size_t)s->n, LINE_LEN, set_cmp);
}

/* ------------------------------------------------------------------ */
/* arch / binfmt: only native strata are handled in C                  */
/* ------------------------------------------------------------------ */

/* True if `system` can execute `arch` binaries without QEMU (matches the
 * shell's check_arch_supported_natively compatibility table). */
static int arch_native_compat(const char *system, const char *arch)
{
    if (strcmp(system, arch) == 0)
        return 1;
    static const char *pairs[][2] = {
        { "aarch64", "armv7hl" }, { "aarch64", "armv7l" },
        { "armv7hl", "armv7l" },  { "armv7l", "armv7hl" },
        { "ppc64", "ppc" },       { "ppc64le", "ppc" },
        { "x86_64", "i386" },     { "x86_64", "i486" },
        { "x86_64", "i586" },     { "x86_64", "i686" },
        { NULL, NULL }
    };
    for (int i = 0; pairs[i][0]; i++)
        if (strcmp(system, pairs[i][0]) == 0 && strcmp(arch, pairs[i][1]) == 0)
            return 1;
    return 0;
}

/* Returns 1 if the stratum runs natively (no binfmt/QEMU needed), 0 if it
 * needs the shell path.  A stratum with no `arch` attr is native. */
static int stratum_is_native(const char *root)
{
    char arch[MAX_NAME_LEN];
    if (br_get_attr(root, "arch", arch, sizeof(arch)) != 0 || arch[0] == '\0')
        return 1;

    char sys[128];
    if (br_get_attr(BR_STRATUM, "arch", sys, sizeof(sys)) != 0 ||
        sys[0] == '\0') {
        struct utsname u;
        if (uname(&u) != 0)
            return 0;
        snprintf(sys, sizeof(sys), "%s", u.machine);
    }
    return arch_native_compat(sys, arch);
}

/* ------------------------------------------------------------------ */
/* etcfs                                                               */
/* ------------------------------------------------------------------ */

/* fork + chroot + exec a FUSE helper (etcfs) on `mountpoint` inside `root`.
 * libfuse backgrounds the daemon, so the foreground child returns once the
 * mount is live.  Returns the child's exit status, or -1. */
static int launch_fuse(const char *root, const char *helper,
                       const char *mountpoint)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (root[0] != '\0' && chroot(root) != 0)
            _exit(127);
        if (chdir("/") != 0)
            _exit(127);
        execl(helper, helper, "-o", "allow_other", mountpoint, (char *)NULL);
        _exit(127);
    }
    int ws;
    while (waitpid(pid, &ws, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
}

/* Append the parent-directory "override directory" targets for a key, e.g.
 * "foo/bar/baz" yields "/foo/bar" and "/foo" (mirrors cfg_etcfs's loop). */
static void add_parent_dirs(struct strset *t, const char *key)
{
    char k[LINE_LEN];
    snprintf(k, sizeof(k), "%s", key);
    char *slash;
    while ((slash = strrchr(k, '/')) != NULL) {
        *slash = '\0';
        if (k[0] == '\0')
            break;
        char line[1200];
        snprintf(line, sizeof(line), "override directory /%s x", k);
        set_add(t, line);
    }
}

/* Configure a freshly-mounted etcfs by writing the add/remove diff to its
 * control file, mirroring cfg_etcfs.  On a clean boot the control file is
 * empty so this is add-only; brl-apply re-runs the full cfg_etcfs for every
 * stratum afterwards, so this is also a correctness backstop, not the last
 * word. */
static void cfg_etcfs_native(const char *etc_mount)
{
    struct strset targets;
    targets.n = 0;

    char vals[MAX_MNTS][MAX_PATH_LEN];
    int nv = cfg_values("global", "etc", vals, MAX_MNTS);
    for (int i = 0; i < nv; i++) {
        char line[1200];
        snprintf(line, sizeof(line), "global /%.480s", vals[i]);
        set_add(&targets, line);
    }

    char keys[MAX_CFG_LINES][MAX_PATH_LEN];
    int nk = cfg_keys("etc-inject", keys, MAX_CFG_LINES);
    for (int i = 0; i < nk; i++) {
        char v[MAX_MNTS][MAX_PATH_LEN];
        int nvv = cfg_values("etc-inject", keys[i], v, 1);
        char line[1200];
        snprintf(line, sizeof(line), "override inject /%.480s %.480s",
                 keys[i], nvv > 0 ? v[0] : "");
        set_add(&targets, line);
        add_parent_dirs(&targets, keys[i]);
    }

    nk = cfg_keys("etc-symlinks", keys, MAX_CFG_LINES);
    for (int i = 0; i < nk; i++) {
        char v[MAX_MNTS][MAX_PATH_LEN];
        int nvv = cfg_values("etc-symlinks", keys[i], v, 1);
        char line[1200];
        snprintf(line, sizeof(line), "override symlink /%.480s %.480s",
                 keys[i], nvv > 0 ? v[0] : "");
        set_add(&targets, line);
        add_parent_dirs(&targets, keys[i]);
    }

    char control[MAX_PATH_LEN];
    if (joincat(control, sizeof(control),
                etc_mount, "/.bedrock-config-filesystem", NULL) < 0)
        return;

    /* Current config lines (empty on a fresh etcfs mount). */
    struct strset current;
    current.n = 0;
    char buf[16384];
    if (slurp(control, buf, sizeof(buf)) >= 0) {
        char *save;
        for (char *ln = strtok_r(buf, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save))
            set_add(&current, ln);
    }

    int fd = open(control, O_WRONLY | O_APPEND);
    if (fd < 0) {
        warn("cfg_etcfs: cannot open %s: %s", control, strerror(errno));
        return;
    }

    /* Remove stale entries: "rm_<field1> <field3>". */
    for (int i = 0; i < current.n; i++) {
        int present = 0;
        for (int j = 0; j < targets.n && !present; j++)
            present = strcmp(current.items[i], targets.items[j]) == 0;
        if (present)
            continue;
        char tmp[LINE_LEN];
        snprintf(tmp, sizeof(tmp), "%s", current.items[i]);
        char *save;
        char *f1 = strtok_r(tmp, " ", &save);
        strtok_r(NULL, " ", &save);              /* skip field 2 */
        char *f3 = strtok_r(NULL, " ", &save);
        dprintf(fd, "rm_%s %s\n", f1 ? f1 : "", f3 ? f3 : "");
    }

    /* Add new entries: "add_<target>". */
    for (int i = 0; i < targets.n; i++) {
        int present = 0;
        for (int j = 0; j < current.n && !present; j++)
            present = strcmp(targets.items[i], current.items[j]) == 0;
        if (!present)
            dprintf(fd, "add_%s\n", targets.items[i]);
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* enforce_symlinks                                                    */
/* ------------------------------------------------------------------ */

static int is_symlink(const char *path)
{
    struct stat sb;
    return lstat(path, &sb) == 0 && S_ISLNK(sb.st_mode);
}

static int path_exists(const char *path)
{
    struct stat sb;
    return lstat(path, &sb) == 0;
}

static void make_symlink(const char *tgt, const char *link)
{
    char parent[MAX_PATH_LEN];
    parent_dir(link, parent, sizeof(parent));
    mkdirp(parent, 0755);
    unlink(link);
    if (symlink(tgt, link) != 0)
        warn("symlink %s -> %s: %s", link, tgt, strerror(errno));
}

/* Apply [symlinks] requirements to one stratum (mirrors enforce_symlinks,
 * minus the unused --force branch).  `root` is the stratum root prefix. */
static void enforce_symlinks_native(const char *root)
{
    char keys[MAX_CFG_LINES][MAX_PATH_LEN];
    int nk = cfg_keys("symlinks", keys, MAX_CFG_LINES);

    for (int i = 0; i < nk; i++) {
        const char *link = keys[i];
        char tv[1][MAX_PATH_LEN];
        if (cfg_values("symlinks", link, tv, 1) < 1)
            continue;
        const char *tgt = tv[0];

        char proc_link[MAX_PATH_LEN];
        char proc_tgt[MAX_PATH_LEN];
        if (joincat(proc_link, sizeof(proc_link), root, link, NULL) < 0 ||
            joincat(proc_tgt, sizeof(proc_tgt), root, tgt, NULL) < 0)
            continue;

        char cur[MAX_PATH_LEN];
        ssize_t n = readlink(proc_link, cur, sizeof(cur) - 1);
        if (n >= 0) {
            cur[n] = '\0';
            if (strcmp(cur, tgt) == 0)
                continue;               /* already correct */
        }

        if (is_symlink(proc_link)) {            /* wrong target -> fix */
            make_symlink(tgt, proc_link);
        } else if (!path_exists(proc_link)) {   /* nothing there -> create */
            make_symlink(tgt, proc_link);
        } else if (is_symlink(proc_tgt)) {      /* swap file and target */
            unlink(proc_tgt);
            rename(proc_link, proc_tgt);
            make_symlink(tgt, proc_link);
        } else if (!path_exists(proc_tgt)) {    /* move file to target */
            char parent[MAX_PATH_LEN];
            parent_dir(proc_tgt, parent, sizeof(parent));
            mkdirp(parent, 0755);
            rename(proc_link, proc_tgt);
            make_symlink(tgt, proc_link);
        } else if (strcmp(link, "/var/lib/dbus/machine-id") == 0) {
            make_symlink(tgt, proc_link);
        } else {
            warn("file exists at both %s and %s; run `brl repair %s`",
                 proc_link, proc_tgt, base_name(root));
        }
    }
}

/* ------------------------------------------------------------------ */
/* enforce_shells / enforce_id_ranges  (run once across all strata)    */
/* ------------------------------------------------------------------ */

/* "/bedrock/strata/<name>/etc/shells", or "/etc/shells" for the init
 * stratum (whose root is the pivoted "/"). */
static void stratum_etc(InitState *st, int i, const char *file, char *out,
                        size_t outsz)
{
    if (i == st->init_index)
        snprintf(out, outsz, "/etc/%s", file);
    else
        snprintf(out, outsz, "/bedrock/strata/%s/etc/%s",
                 st->strata[i].name, file);
}

void enforce_shells_all(InitState *st)
{
    /* Desired set: every stratum's /etc/shells line, remapped to
     * /bedrock/cross/bin/<shell>. */
    struct strset cross;
    cross.n = 0;
    char buf[16384];

    for (int i = 0; i < st->n_strata; i++) {
        char shells[MAX_PATH_LEN];
        stratum_etc(st, i, "shells", shells, sizeof(shells));
        if (slurp(shells, buf, sizeof(buf)) < 0)
            continue;
        char *save;
        for (char *ln = strtok_r(buf, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            if (ln[0] != '/')
                continue;
            char mapped[LINE_LEN];
            snprintf(mapped, sizeof(mapped),
                     "/bedrock/cross/bin/%s", base_name(ln));
            set_add(&cross, mapped);
        }
    }
    set_sort(&cross);

    /* Rewrite each stratum's /etc/shells to include the cross set. */
    for (int i = 0; i < st->n_strata; i++) {
        char shells[MAX_PATH_LEN];
        stratum_etc(st, i, "shells", shells, sizeof(shells));

        struct strset merged;
        merged.n = 0;
        for (int c = 0; c < cross.n; c++)
            set_add(&merged, cross.items[c]);
        if (slurp(shells, buf, sizeof(buf)) >= 0) {
            char *save;
            for (char *ln = strtok_r(buf, "\n", &save); ln;
                 ln = strtok_r(NULL, "\n", &save))
                if (ln[0] != '\0')
                    set_add(&merged, ln);
        }
        set_sort(&merged);

        char out[16384];
        size_t off = 0;
        for (int m = 0; m < merged.n && off < sizeof(out) - LINE_LEN; m++)
            off += (size_t)snprintf(out + off, sizeof(out) - off,
                                    "%s\n", merged.items[m]);
        write_atomic(shells, out, off);
    }
}

/* ensure one "key<sep>value" line is present and correct in a config file.
 * `desired` is the full canonical line; a line whose first token equals
 * `key` (token delimited by space, tab or '=') is replaced, otherwise the
 * line is appended.  Mirrors ensure_line without regex. */
static void ensure_kv(const char *path, const char *key, const char *desired)
{
    char buf[16384];
    if (slurp(path, buf, sizeof(buf)) < 0)
        return;

    char out[16384];
    size_t off = 0;
    int    replaced = 0;
    size_t keylen = strlen(key);

    char *save;
    for (char *ln = strtok_r(buf, "\n", &save); ln;
         ln = strtok_r(NULL, "\n", &save)) {
        const char *p = ln;
        while (*p == ' ' || *p == '\t')
            p++;
        int match = strncmp(p, key, keylen) == 0 &&
                    (p[keylen] == ' ' || p[keylen] == '\t' ||
                     p[keylen] == '=' || p[keylen] == '\0');
        if (match) {
            if (strcmp(ln, desired) == 0)
                return;                 /* already correct, leave file as-is */
            off += (size_t)snprintf(out + off, sizeof(out) - off,
                                    "%s\n", desired);
            replaced = 1;
        } else {
            off += (size_t)snprintf(out + off, sizeof(out) - off,
                                    "%s\n", ln);
        }
        if (off >= sizeof(out) - LINE_LEN)
            return;                     /* file unexpectedly large; bail */
    }
    if (!replaced)
        off += (size_t)snprintf(out + off, sizeof(out) - off,
                                "%s\n", desired);
    write_atomic(path, out, off);
}

void enforce_id_ranges_all(InitState *st)
{
    static const char *login_defs[][2] = {
        { "UID_MIN", "UID_MIN 1000" },     { "UID_MAX", "UID_MAX 65534" },
        { "SYS_UID_MIN", "SYS_UID_MIN 1" }, { "SYS_UID_MAX", "SYS_UID_MAX 999" },
        { "GID_MIN", "GID_MIN 1000" },     { "GID_MAX", "GID_MAX 65534" },
        { "SYS_GID_MIN", "SYS_GID_MIN 1" }, { "SYS_GID_MAX", "SYS_GID_MAX 999" },
        { NULL, NULL }
    };
    static const char *adduser[][2] = {
        { "FIRST_UID", "FIRST_UID=1000" },  { "LAST_UID", "LAST_UID=65534" },
        { "FIRST_SYSTEM_UID", "FIRST_SYSTEM_UID=1" },
        { "LAST_SYSTEM_UID", "LAST_SYSTEM_UID=999" },
        { "FIRST_GID", "FIRST_GID=1000" },  { "LAST_GID", "LAST_GID=65534" },
        { "FIRST_SYSTEM_GID", "FIRST_SYSTEM_GID=1" },
        { "LAST_SYSTEM_GID", "LAST_SYSTEM_GID=999" },
        { NULL, NULL }
    };

    for (int i = 0; i < st->n_strata; i++) {
        char cfg[MAX_PATH_LEN];

        snprintf(cfg, sizeof(cfg),
                 "/bedrock/strata/%s/etc/login.defs", st->strata[i].name);
        if (access(cfg, F_OK) == 0)
            for (int k = 0; login_defs[k][0]; k++)
                ensure_kv(cfg, login_defs[k][0], login_defs[k][1]);

        snprintf(cfg, sizeof(cfg),
                 "/bedrock/strata/%s/etc/adduser.conf", st->strata[i].name);
        if (access(cfg, F_OK) == 0)
            for (int k = 0; adduser[k][0]; k++)
                ensure_kv(cfg, adduser[k][0], adduser[k][1]);
    }
}

/* ------------------------------------------------------------------ */
/* orchestration                                                       */
/* ------------------------------------------------------------------ */

void native_prepare_shared_mounts(void)
{
    char mnts[MAX_MNTS][MAX_PATH_LEN];
    char p[MAX_PATH_LEN];

    int n = cfg_values("global", "share", mnts, MAX_MNTS);
    for (int i = 0; i < n; i++)
        if (joincat(p, sizeof(p), BR_STRATUM, mnts[i], NULL) == 0) {
            mkdirp(p, 0755);
            mnt_prop(p, MS_SHARED | MS_REC);    /* idempotent; errors ok */
        }

    n = cfg_values("global", "bind", mnts, MAX_MNTS);
    for (int i = 0; i < n; i++)
        if (joincat(p, sizeof(p), BR_STRATUM, mnts[i], NULL) == 0) {
            mkdirp(p, 0755);
            mnt_prop(p, MS_PRIVATE);
        }
}

int native_enable_stratum(const char *name, const char *root)
{
    /* Non-native arch needs binfmt/QEMU registration; defer to the shell. */
    if (!stratum_is_native(root))
        return 1;

    char cur[MAX_NAME_LEN];
    if (br_get_attr(root, "stratum", cur, sizeof(cur)) != 0 ||
        strcmp(cur, name) != 0)
        br_set_attr(root, "stratum", name);

    mnt_bind(root, root, 0);

    char br[MAX_PATH_LEN], st_path[MAX_PATH_LEN];
    char mnts[MAX_MNTS][MAX_PATH_LEN];

    int n = cfg_values("global", "share", mnts, MAX_MNTS);
    for (int i = 0; i < n; i++) {
        if (joincat(br, sizeof(br), BR_STRATUM, mnts[i], NULL) < 0 ||
            joincat(st_path, sizeof(st_path), root, mnts[i], NULL) < 0)
            continue;
        mkdirp(st_path, 0755);
        mnt_bind(br, st_path, 1);                /* --rbind */
        mnt_prop(st_path, MS_SHARED | MS_REC);
    }

    n = cfg_values("global", "bind", mnts, MAX_MNTS);
    for (int i = 0; i < n; i++) {
        if (joincat(br, sizeof(br), BR_STRATUM, mnts[i], NULL) < 0 ||
            joincat(st_path, sizeof(st_path), root, mnts[i], NULL) < 0)
            continue;
        mkdirp(st_path, 0755);
        mnt_bind(br, st_path, 0);                /* --bind */
        mnt_prop(st_path, MS_PRIVATE);
    }

    char etc[MAX_PATH_LEN];
    if (joincat(etc, sizeof(etc), root, "/etc", NULL) == 0) {
        mkdirp(etc, 0755);
        if (launch_fuse(root, "/bedrock/libexec/etcfs", "/etc") != 0)
            warn("etcfs failed for %s", name);
        else
            cfg_etcfs_native(etc);
    }

    enforce_symlinks_native(root);

    char flag[MAX_PATH_LEN];
    if (joincat(flag, sizeof(flag), ENABLED_DIR "/", name, NULL) == 0) {
        int fd = open(flag, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* brl-apply: native replacements for its two slow sections            */
/* ------------------------------------------------------------------ */

#define RESTRICTED_DIR  "/bedrock/run/restricted_cmds"
#define APPLY_STOCK     "/bedrock/libexec/brl-apply"
#define APPLY_TRIMMED   "/bedrock/run/enux-init/brl-apply.trim"

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* brl-apply's "restricted commands" section: O(restrict^2) in the shell
 * because it re-runs cfg_values (a full bedrock.conf reparse) inside a
 * nested loop.  In C cfg_values is one in-memory call. */
static void apply_restricted_cmds(void)
{
    mkdirp(RESTRICTED_DIR, 0755);

    char cmds[MAX_CFG_LINES][MAX_PATH_LEN];
    int  n = cfg_values("restriction", "restrict", cmds, MAX_CFG_LINES);

    for (int i = 0; i < n; i++) {
        char path[MAX_PATH_LEN];
        if (joincat(path, sizeof(path), RESTRICTED_DIR "/", cmds[i], NULL) < 0)
            continue;
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
    }

    DIR *d = opendir(RESTRICTED_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' ||
             (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        int wanted = 0;
        for (int i = 0; i < n && !wanted; i++)
            wanted = strcmp(e->d_name, cmds[i]) == 0;
        if (!wanted) {
            char path[MAX_PATH_LEN];
            if (joincat(path, sizeof(path),
                        RESTRICTED_DIR "/", e->d_name, NULL) == 0)
                unlink(path);
        }
    }
    closedir(d);
}

/* brl-apply's "force login shells to cross path" section: the shell forks
 * echo|awk several times per /etc/passwd line, twice over.  Here it is a
 * single in-memory pass.  A shell field is rewritten to /bedrock/cross/bin/
 * <name> only when that cross shell exists and the field is not already a
 * cross path — exactly the shell's condition.  Requires crossfs to be
 * configured first, so this runs after the trimmed brl-apply. */
static void apply_passwd_shells(InitState *st)
{
    char buf[65536];
    if (slurp("/etc/passwd", buf, sizeof(buf)) < 0)
        return;

    char   out[65536];
    size_t off = 0;
    int    changed = 0;

    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);

        char line[2048];
        if (linelen >= sizeof(line))
            linelen = sizeof(line) - 1;
        memcpy(line, p, linelen);
        line[linelen] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        /* Locate the shell field: everything past the 6th colon. */
        int colons = 0;
        char *shell = NULL;
        for (char *c = line; *c; c++)
            if (*c == ':' && ++colons == 6) {
                shell = c + 1;
                break;
            }

        if (shell && !starts_with(shell, "/bedrock/cross/")) {
            char cross[MAX_PATH_LEN];
            snprintf(cross, sizeof(cross),
                     "/bedrock/cross/bin/%s", base_name(shell));
            if (access(cross, X_OK) == 0) {
                *shell = '\0';          /* truncate at start of shell field */
                off += (size_t)snprintf(out + off, sizeof(out) - off,
                                        "%s%s\n", line, cross);
                changed = 1;
                if (off >= sizeof(out) - 256)
                    return;             /* implausibly large; leave file as-is */
                continue;
            }
        }
        off += (size_t)snprintf(out + off, sizeof(out) - off, "%s\n", line);
        if (off >= sizeof(out) - 256)
            return;
    }

    if (changed && write_atomic("/etc/passwd", out, off) == 0)
        enforce_shells_all(st);
}

/* Write a copy of the stock brl-apply with its two slow sections removed.
 * Returns 0 on success, -1 if either section's anchor is missing (caller
 * then falls back to the stock script so configuration is never skipped). */
static int write_trimmed_apply(void)
{
    static char buf[131072];
    static char out[131072];
    ssize_t len = slurp(APPLY_STOCK, buf, sizeof(buf));
    if (len < 0)
        return -1;

    size_t off    = 0;
    int    skip   = 0;
    int    starts = 0;

    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);

        char line[1024];
        size_t copy = linelen < sizeof(line) ? linelen : sizeof(line) - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        /* End anchors first (re-enable output and keep the anchor line),
         * then start anchors (suppress the removed sections). */
        if (starts_with(line, "# Setup xorg.conf configuration") ||
            starts_with(line, "# Setup pmm front-end."))
            skip = 0;
        else if (starts_with(line,
                     "# Specify commands which should automatically be restricted") ||
                 starts_with(line, "# Force login shells to use cross path")) {
            skip = 1;
            starts++;
        }

        if (!skip)
            off += (size_t)snprintf(out + off, sizeof(out) - off,
                                    "%s\n", line);
    }

    if (starts < 2)
        return -1;                      /* anchors changed; use stock instead */
    return write_atomic(APPLY_TRIMMED, out, off);
}

int native_apply_run(InitState *st)
{
    mkdirp("/bedrock/run/enux-init", 0755);

    if (write_trimmed_apply() != 0) {
        warn("brl-apply: could not trim (anchors changed?); running stock");
        return run_cmd((char *[]){ APPLY_STOCK, "--skip-repair", NULL }, 0);
    }

    int rc = run_cmd((char *[]){ "/bedrock/libexec/busybox", "sh",
                                 (char *)APPLY_TRIMMED, "--skip-repair",
                                 NULL }, 0);

    /* The two sections removed from the trimmed script, done natively.
     * passwd_shells runs last: it needs the cross paths crossfs publishes,
     * and the trimmed script has just configured crossfs. */
    apply_restricted_cmds();
    apply_passwd_shells(st);
    return rc;
}
