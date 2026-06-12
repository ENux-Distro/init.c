/*
 * init.c - first-boot hijack completion and upgrade fixups
 *
 * complete_hijack() runs exactly once, on the first boot after a hijack
 * install (flag: /bedrock/complete-hijack-install).  It is a 1:1 port of
 * the shell function — cold path, correctness over speed.  File moves use
 * rename(2); hijack installs place /bedrock on the hijacked system's root
 * filesystem, so cross-device moves do not occur on a sane install.
 *
 * complete_upgrade() finishes pre-0.7.8 bouncer swaps while crossfs is
 * not yet running.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "init.h"
#include "bedrock_conf.h"
#include "stratum.h"

static int path_exists(const char *path)
{
    struct stat sb;
    return lstat(path, &sb) == 0;
}

static void xrename(const char *from, const char *to)
{
    if (rename(from, to) < 0)
        warn("move %s -> %s: %s", from, to, strerror(errno));
}

static void xsymlink(const char *target, const char *linkpath)
{
    if (symlink(target, linkpath) < 0 && errno != EEXIST)
        warn("symlink %s -> %s: %s", linkpath, target, strerror(errno));
}

static void touch(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        warn("touch %s: %s", path, strerror(errno));
    else
        close(fd);
}

/* mkdir -p for the parent directory of `path`. */
static void mkdirp_parent(const char *path)
{
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = '\0';
    if (mkdirp(tmp, 0755) < 0 && errno != EEXIST)
        warn("mkdir %s: %s", tmp, strerror(errno));
}

/* Top-level / entries that stay in place during the hijack move. */
static int hijack_skip_item(const char *name)
{
    return asm_strcmp(name, "proc") == 0 ||
           asm_strcmp(name, "sys") == 0 ||
           asm_strcmp(name, "dev") == 0 ||
           asm_strcmp(name, "run") == 0 ||
           asm_strcmp(name, "boot") == 0 ||
           strncmp(name, "bedrock", 7) == 0;
}

/* Paths excluded from the global share/bind moves. */
static int global_skip_path(const char *path)
{
    return asm_strcmp(path, "/proc") == 0 ||
           asm_strcmp(path, "/sys") == 0 ||
           asm_strcmp(path, "/dev") == 0 ||
           asm_strcmp(path, "/run") == 0 ||
           asm_strcmp(path, "/boot") == 0 ||
           strncmp(path, "/bedrock", 8) == 0;
}

static void complete_hijack(void)
{
    notice("Completing " COL_CYAN "hijack install" COL_RESET);

    char hijacked[MAX_NAME_LEN];
    if (deref("hijacked", hijacked, sizeof(hijacked)) < 0) {
        warn("cannot dereference 'hijacked' alias; skipping hijack "
             "completion");
        return;
    }

    char hroot[MAX_PATH_LEN];
    snprintf(hroot, sizeof(hroot), STRATA_DIR "/%s", hijacked);

    /* [1/6] move / contents into the hijacked stratum */
    step(1, 6, "Moving " COL_GREEN "%s" COL_RESET
         " files to " COL_CYAN "%s" COL_RESET, hijacked, hroot);
    if (mkdirp(hroot, 0755) < 0 && errno != EEXIST)
        warn("mkdir %s: %s", hroot, strerror(errno));
    br_set_attr(hroot, "stratum", hijacked);

    DIR *d = opendir("/");
    if (!d) {
        warn("opendir /: %s", strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        if (hijack_skip_item(ent->d_name))
            continue;

        char from[MAX_PATH_LEN], to[MAX_PATH_LEN];
        if (joincat(from, sizeof(from), "/", ent->d_name, NULL) < 0 ||
            joincat(to, sizeof(to), hroot, "/", ent->d_name, NULL) < 0) {
            warn("path too long; skipping /%s", ent->d_name);
            continue;
        }
        xrename(from, to);
    }
    closedir(d);

    /* [2/6] move global items back to / */
    step(2, 6, "Moving " COL_GREEN "global" COL_RESET
         " items to " COL_CYAN "/" COL_RESET);
    br_set_attr("/", "stratum", "bedrock");

    char globals[64][MAX_PATH_LEN];
    int  ng = cfg_values("global", "share", globals, 32);
    ng += cfg_values("global", "bind", globals + ng, 64 - ng);

    for (int i = 0; i < ng; i++) {
        const char *g = globals[i];
        if (global_skip_path(g))
            continue;

        char in_hijacked[MAX_PATH_LEN];
        if (joincat(in_hijacked, sizeof(in_hijacked), hroot, g,
                    NULL) < 0) {
            warn("path too long; skipping %s", g);
            continue;
        }

        if (path_exists(in_hijacked)) {
            mkdirp_parent(g);
            xrename(in_hijacked, g);
            if (mkdirp(in_hijacked, 0755) < 0 && errno != EEXIST)
                warn("mkdir %s: %s", in_hijacked, strerror(errno));
        } else {
            if (mkdirp(g, 0755) < 0 && errno != EEXIST)
                warn("mkdir %s: %s", g, strerror(errno));
        }
    }

    char etcs[64][MAX_PATH_LEN];
    int  ne = cfg_values("global", "etc", etcs, 64);
    for (int i = 0; i < ne; i++) {
        char dst[MAX_PATH_LEN], src[MAX_PATH_LEN];
        if (joincat(dst, sizeof(dst), "/etc/", etcs[i], NULL) < 0 ||
            joincat(src, sizeof(src), hroot, "/etc/", etcs[i],
                    NULL) < 0) {
            warn("path too long; skipping etc item %d", i);
            continue;
        }
        mkdirp_parent(dst);
        if (path_exists(src))
            xrename(src, dst);
    }

    /* [3/6] root skeleton */
    step(3, 6, "Creating root files and directories");
    static const char *skel_dirs[] = {
        "/bin", "/dev", "/etc", "/lib/systemd", "/mnt", "/proc", "/root",
        "/run", "/sbin", "/sys", "/tmp", "/usr/bin", "/usr/sbin",
        "/usr/share/info", "/var", NULL
    };
    for (int i = 0; skel_dirs[i]; i++) {
        if (mkdirp(skel_dirs[i], 0755) < 0 && errno != EEXIST)
            warn("mkdir %s: %s", skel_dirs[i], strerror(errno));
    }

    xsymlink("../bedrock/etc/bedrock-release", "/etc/bedrock-release");
    xsymlink("../bedrock/etc/os-release", "/etc/os-release");
    xsymlink(BEDROCK_ROOT "/libexec/kmod", "/sbin/depmod");
    xsymlink(BEDROCK_ROOT "/libexec/kmod", "/sbin/insmod");
    xsymlink(BEDROCK_ROOT "/libexec/kmod", "/sbin/lsmod");
    xsymlink(BEDROCK_ROOT "/libexec/kmod", "/sbin/modinfo");
    xsymlink(BEDROCK_ROOT "/libexec/kmod", "/sbin/modprobe");
    xsymlink(BEDROCK_ROOT "/libexec/kmod", "/sbin/rmmod");
    /* literal "hijacked" alias path, as in the shell */
    xsymlink(STRATA_DIR "/hijacked/usr/share/grub", "/usr/share/grub");

    int rc = run_cmd((char *[]){ BEDROCK_ROOT "/libexec/busybox",
                                 "--install", "-s", NULL }, 1);
    if (rc != 0)
        warn("busybox --install -s exited %d", rc);

    touch("/usr/share/info/.keepinfodir");

    char hinit[MAX_PATH_LEN];
    if (joincat(hinit, sizeof(hinit), hroot, "/sbin/init", NULL) == 0)
        xrename(hinit, "/sbin/init");

    /* [4/6] restore the hijacked system's original init binaries */
    step(4, 6, "Restoring " COL_CYAN "hijacked" COL_RESET " init systems");
    static const char *init_paths[] = {
        "/bin/init", "/sbin/init", "/usr/bin/init", "/usr/sbin/init",
        "/lib/systemd/systemd", "/usr/lib/systemd/systemd", NULL
    };
    for (int i = 0; init_paths[i]; i++) {
        char bak[MAX_PATH_LEN], orig[MAX_PATH_LEN];
        if (joincat(bak, sizeof(bak), hroot, init_paths[i],
                    "-bedrock-backup", NULL) < 0 ||
            joincat(orig, sizeof(orig), hroot, init_paths[i], NULL) < 0)
            continue;
        if (path_exists(bak))
            xrename(bak, orig);
    }

    /* [5/6] strat capabilities */
    step(5, 6, "Granting " COL_CYAN "strat" COL_RESET
         " necessary capabilities");
    rc = run_cmd((char *[]){ BEDROCK_ROOT "/libexec/setcap",
                             "cap_sys_chroot=ep",
                             BEDROCK_ROOT "/bin/strat", NULL }, 0);
    if (rc != 0)
        warn("setcap on strat exited %d", rc);

    /* [6/6] clear the flag so this never runs again */
    step(6, 6, "Completing " COL_CYAN "hijack install" COL_RESET);
    if (unlink(HIJACK_FLAG) < 0)
        warn("unlink %s: %s", HIJACK_FLAG, strerror(errno));

    asm_write_str(STDOUT_FILENO, "\n");
}

void maybe_complete_hijack(void)
{
    if (access(HIJACK_FLAG, F_OK) == 0)
        complete_hijack();
}

void complete_upgrade(void)
{
    /* crossfs builds before 0.7.8 could not handle bouncer changing out
     * from under them; the upgrade staged the new bouncer alongside.
     * Swap it in now, while crossfs is not running. */
    if (path_exists(BEDROCK_ROOT "/libexec/bouncer-0.7.9"))
        xrename(BEDROCK_ROOT "/libexec/bouncer-0.7.9",
                BEDROCK_ROOT "/libexec/bouncer");
}
