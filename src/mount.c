/*
 * init.c - mount operations
 *
 * pivot() and preenable() as direct mount(2)/pivot_root(2) syscalls.
 * fstab mounting delegates to `busybox mount -a` — reimplementing mount's
 * fs-type probing and option parsing is exactly the kind of logic we are
 * told not to duplicate.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "init.h"

/* glibc does not expose pivot_root(2); call it directly. */
static int do_pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

int mkdirp(const char *path, mode_t mode)
{
    char tmp[MAX_PATH_LEN];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * mount_fstab()
 *
 * Shell equivalent:
 *   /bedrock/libexec/dmsetup mknodes
 *   /bedrock/libexec/lvm vgscan --ignorelockingfailure
 *   /bedrock/libexec/lvm vgchange -ay --ignorelockingfailure
 *   mount -a
 *
 * All failures are warnings: the shell runs with `set +e` here, and a
 * missing LVM setup is normal on non-LVM systems.
 */
void mount_fstab(void)
{
    if (access(BEDROCK_ROOT "/libexec/dmsetup", X_OK) == 0)
        run_cmd((char *[]){ BEDROCK_ROOT "/libexec/dmsetup",
                            "mknodes", NULL }, 1);

    if (access(BEDROCK_ROOT "/libexec/lvm", X_OK) == 0) {
        run_cmd((char *[]){ BEDROCK_ROOT "/libexec/lvm",
                            "vgscan", "--ignorelockingfailure", NULL }, 1);
        run_cmd((char *[]){ BEDROCK_ROOT "/libexec/lvm",
                            "vgchange", "-ay", "--ignorelockingfailure",
                            NULL }, 1);
    }

    int rc = -1;
    if (access(BEDROCK_ROOT "/libexec/busybox", X_OK) == 0)
        rc = run_cmd((char *[]){ BEDROCK_ROOT "/libexec/busybox",
                                 "mount", "-a", NULL }, 0);
    if (rc != 0 && access("/bin/mount", X_OK) == 0)
        rc = run_cmd((char *[]){ "/bin/mount", "-a", NULL }, 0);
    if (rc != 0)
        warn("mount -a exited %d", rc);
}

/*
 * pivot_root_to()
 *
 * Shell equivalent:
 *   mount --bind "/bedrock/strata/${init}" "/bedrock/strata/${init}"
 *   mkdir -p     "/bedrock/strata/${init}/bedrock"
 *   mount --bind /bedrock "/bedrock/strata/${init}/bedrock"
 *   cd "/bedrock/strata/${init}"
 *   pivot_root "." "bedrock/strata/${init}"
 *   cd /
 *   mount --move "/bedrock/strata/${init}" "/bedrock/strata/bedrock"
 *
 * Binds are non-recursive (--bind, not --rbind), matching the shell.
 * After the pivot, put_old resolves to the same string as stratum_root in
 * the new namespace; the old root is then moved onto
 * /bedrock/strata/bedrock so the original system — including its /proc,
 * /dev, /sys mounts — is reachable as the bedrock stratum.
 */
void pivot_root_to(const char *stratum_root)
{
    if (mount(stratum_root, stratum_root, NULL, MS_BIND, NULL) < 0)
        panic("bind mount %s: %s", stratum_root, strerror(errno));

    char bedrock_in_stratum[MAX_PATH_LEN];
    snprintf(bedrock_in_stratum, sizeof(bedrock_in_stratum),
             "%s" BEDROCK_ROOT, stratum_root);
    if (mkdirp(bedrock_in_stratum, 0755) < 0)
        panic("mkdir %s: %s", bedrock_in_stratum, strerror(errno));

    if (mount(BEDROCK_ROOT, bedrock_in_stratum, NULL, MS_BIND, NULL) < 0)
        panic("bind mount /bedrock into stratum: %s", strerror(errno));

    if (chdir(stratum_root) < 0)
        panic("chdir %s: %s", stratum_root, strerror(errno));

    /* put_old is stratum_root relative to the new root. */
    if (do_pivot_root(".", stratum_root + 1) < 0)
        panic("pivot_root: %s", strerror(errno));

    if (chdir("/") < 0)
        panic("chdir / after pivot: %s", strerror(errno));

    if (mount(stratum_root, BEDROCK_ROOT "/strata/bedrock",
              NULL, MS_MOVE, NULL) < 0)
        warn("move old root to /bedrock/strata/bedrock: %s",
             strerror(errno));

    /* `man 8 pivot_root` says to chroot here; the shell found that breaks
     * mount point access and skips it.  We do the same. */
}

/*
 * preenable_mounts()
 *
 * Shell equivalent: for each of proc, dev, sys —
 *   mount --make-shared /bedrock/strata/bedrock/<d>
 *   mount --rbind /bedrock/strata/bedrock/<d> /<d>
 * then mount the bedrock_run tmpfs and rbind it to /bedrock/run, and
 * re-run ensure_essential_environment() as a safety net.
 *
 * The source paths live under the moved old root, so these rbinds pull
 * the already-populated /proc, /dev, /sys mounts into the new root.
 */
void preenable_mounts(void)
{
    static const char *shared_dirs[] = { "/proc", "/dev", "/sys", NULL };

    for (int i = 0; shared_dirs[i]; i++) {
        const char *d = shared_dirs[i];

        char src[MAX_PATH_LEN];
        snprintf(src, sizeof(src), BEDROCK_ROOT "/strata/bedrock%s", d);
        if (mkdirp(src, 0755) < 0 && errno != EEXIST)
            warn("mkdir %s: %s", src, strerror(errno));

        if (mount(NULL, src, NULL, MS_SHARED, NULL) < 0)
            warn("make-shared %s: %s", src, strerror(errno));

        if (mkdirp(d, 0755) < 0 && errno != EEXIST)
            warn("mkdir %s: %s", d, strerror(errno));
        if (mount(src, d, NULL, MS_BIND | MS_REC, NULL) < 0)
            warn("rbind %s -> %s: %s", src, d, strerror(errno));
    }

    /* bedrock run tmpfs */
    const char *br_dir = BEDROCK_ROOT "/strata/bedrock" BEDROCK_ROOT;
    const char *br_run = BEDROCK_ROOT "/strata/bedrock" BEDROCK_ROOT "/run";

    if (mkdirp(br_run, 0755) < 0 && errno != EEXIST)
        warn("mkdir %s: %s", br_run, strerror(errno));

    if (mount("bedrock_run", br_run, "tmpfs",
              MS_NOSUID | MS_NODEV, "mode=0755") < 0)
        warn("mount bedrock_run tmpfs: %s", strerror(errno));

    /* chmod go-w on both, like the shell */
    if (chmod(br_dir, 0755) < 0)
        warn("chmod %s: %s", br_dir, strerror(errno));
    if (chmod(br_run, 0755) < 0)
        warn("chmod %s: %s", br_run, strerror(errno));

    if (mkdirp(BEDROCK_ROOT "/run", 0755) < 0 && errno != EEXIST)
        warn("mkdir " BEDROCK_ROOT "/run: %s", strerror(errno));
    if (mount(br_run, BEDROCK_ROOT "/run", NULL,
              MS_BIND | MS_REC, NULL) < 0)
        warn("rbind " BEDROCK_ROOT "/run: %s", strerror(errno));

    /* Safety net, exactly like the shell.  All checks inside are against
     * the freshly rbound /proc, so this is a no-op when all is well. */
    ensure_essential_environment();
}
