/*
 * init.c - mount operations
 *
 * Replaces the shell's pivot(), preenable(), and `mount -a` logic
 * with direct mount(2) / pivot_root(2) syscalls.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#include "init.h"

/* pivot_root syscall wrapper */
/* glibc doesn't always expose pivot_root; call it directly. */
static int do_pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

/* mkdirp */
int mkdirp(const char *path, mode_t mode)
{
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, MAX_PATH_LEN - 1);
    tmp[MAX_PATH_LEN - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

/* xmount */
int xmount(const char *src, const char *tgt,
           const char *fs, unsigned long flags, const void *data)
{
    mkdirp(tgt, 0755);
    int r = mount(src, tgt, fs, flags, data);
    if (r < 0)
        notice(COL_YELLOW "warn" COL_RESET ": mount %s→%s: %s",
               src ? src : "none", tgt, strerror(errno));
    return r;
}

/* fstab mounting */

/*
 * mount_fstab()
 *
 * Replaces:
 *   /bedrock/libexec/dmsetup mknodes
 *   /bedrock/libexec/lvm vgscan --ignorelockingfailure
 *   /bedrock/libexec/lvm vgchange -ay --ignorelockingfailure
 *   mount -a
 *
 * dmsetup and lvm still need forks (external binaries with no syscall
 * equivalents in userspace). mount -a we do ourselves by parsing /etc/fstab
 * and calling mount(2) directly, skipping noauto entries.
 */
void mount_fstab(void)
{
    /* dmsetup mknodes — needed for LVM/DM block devices */
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/dmsetup",
                  (char *[]){ "dmsetup", "mknodes", NULL });
            _exit(0);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* lvm vgscan */
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/lvm",
                  (char *[]){ "lvm", "vgscan",
                               "--ignorelockingfailure", NULL });
            _exit(0);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* lvm vgchange -ay */
    {
        pid_t pid = fork();
        if (pid == 0) {
            execv("/bedrock/libexec/lvm",
                  (char *[]){ "lvm", "vgchange", "-ay",
                               "--ignorelockingfailure", NULL });
            _exit(0);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    /* Parse /etc/fstab and mount everything (replaces `mount -a`) */
    FILE *f = setmntent("/etc/fstab", "r");
    if (!f) {
        notice(COL_YELLOW "warn" COL_RESET ": cannot open /etc/fstab: %s",
               strerror(errno));
        return;
    }

    struct mntent *ent;
    while ((ent = getmntent(f)) != NULL) {
        /* Skip pseudo/virtual filesystems that are already up */
        if (strcmp(ent->mnt_type, "swap") == 0) continue;
        if (hasmntopt(ent, "noauto")) continue;

        /* Skip if already mounted */
        FILE *mf = setmntent("/proc/mounts", "r");
        int already = 0;
        if (mf) {
            struct mntent *m;
            while ((m = getmntent(mf)) != NULL) {
                if (strcmp(m->mnt_dir, ent->mnt_dir) == 0) {
                    already = 1;
                    break;
                }
            }
            endmntent(mf);
        }
        if (already) continue;

        /* Build mount flags from options string */
        unsigned long flags = 0;
        if (hasmntopt(ent, "ro"))          flags |= MS_RDONLY;
        if (hasmntopt(ent, "noexec"))      flags |= MS_NOEXEC;
        if (hasmntopt(ent, "nosuid"))      flags |= MS_NOSUID;
        if (hasmntopt(ent, "nodev"))       flags |= MS_NODEV;
        if (hasmntopt(ent, "relatime"))    flags |= MS_RELATIME;
        if (hasmntopt(ent, "strictatime")) flags |= MS_STRICTATIME;
        if (hasmntopt(ent, "bind"))        flags |= MS_BIND;

        mkdirp(ent->mnt_dir, 0755);

        if (mount(ent->mnt_fsname, ent->mnt_dir,
                  ent->mnt_type, flags, ent->mnt_opts) < 0) {
            notice(COL_YELLOW "warn" COL_RESET
                   ": fstab mount %s: %s",
                   ent->mnt_dir, strerror(errno));
        }
    }
    endmntent(f);
}

/* pivot_root */

/*
 * pivot_root_to()
 *
 * Replaces the shell's pivot() function:
 *
 *   mount --bind "/bedrock/strata/${init_stratum}" "/bedrock/strata/${init_stratum}"
 *   mkdir -p "/bedrock/strata/${init_stratum}/bedrock"
 *   mount --bind "/bedrock" "/bedrock/strata/${init_stratum}/bedrock"
 *   cd "/bedrock/strata/${init_stratum}"
 *   pivot_root "." "bedrock/strata/${init_stratum}"
 *   cd /
 *   mount --move "/bedrock/strata/${init_stratum}" "/bedrock/strata/bedrock"
 */
void pivot_root_to(const char *stratum_root)
{
    /* pivot_root requires the new root to be a mount point */
    if (mount(stratum_root, stratum_root, NULL,
              MS_BIND | MS_REC, NULL) < 0)
        panic("bind mount stratum root: %s", strerror(errno));

    /* Make /bedrock accessible from inside the new root */
    char bedrock_in_stratum[MAX_PATH_LEN];
    snprintf(bedrock_in_stratum, sizeof(bedrock_in_stratum),
             "%s" BEDROCK_ROOT, stratum_root);
    mkdirp(bedrock_in_stratum, 0755);

    if (mount(BEDROCK_ROOT, bedrock_in_stratum, NULL,
              MS_BIND | MS_REC, NULL) < 0)
        panic("bind mount /bedrock into stratum: %s", strerror(errno));

    /* chdir to new root before pivot */
    if (chdir(stratum_root) < 0)
        panic("chdir to stratum root: %s", strerror(errno));

    /* The shell's pivot() passes the stratum path as put_old:
     *   pivot_root "." "bedrock/strata/${init_stratum}"
     *
     * This resolves to the same path as stratum_root (/bedrock/strata/enux).
     * After the pivot, the old root is at that path inside the new root.
     * We then move it to /bedrock/strata/bedrock.
     *
     * The kernel requires put_old to exist (as a directory). Since
     * stratum_root already exists (it's the directory we chdir'd into),
     * no mkdirp is needed.
     */
    const char *rel_old = stratum_root + 1; /* strip leading '/' */

    if (do_pivot_root(".", rel_old) < 0)
        panic("pivot_root: %s", strerror(errno));

    /* After pivot we're in the new root. chdir to / to refresh cwd. */
    if (chdir("/") < 0)
        panic("chdir / after pivot: %s", strerror(errno));

    /* Move the old root from its pivot location to /bedrock/strata/bedrock
     * so Bedrock's tools can find it.
     *
     * After pivot, the old root is at stratum_root in the new namespace
     * (because pivot_root placed it at put_old = "bedrock/strata/<name>"
     * which resolves to /bedrock/strata/<name> = stratum_root). */
    if (mount(stratum_root, BEDROCK_ROOT "/strata/bedrock",
              NULL, MS_MOVE, NULL) < 0) {
        notice(COL_YELLOW "warn" COL_RESET
               ": mount --move old root: %s", strerror(errno));
    }
}

/* preenable mounts */

/*
 * preenable_mounts()
 *
 * Replaces preenable() in the shell script:
 *   mount --make-shared /bedrock/strata/bedrock/proc
 *   mount --rbind /bedrock/strata/bedrock/proc /proc
 *   (same for /dev, /sys)
 *   mount -t tmpfs bedrock_run /bedrock/strata/bedrock/bedrock/run
 *   mount --rbind ...bedrock/run /bedrock/run
 *
 * All direct mount(2) calls — zero forks.
 */
void preenable_mounts(const char *init_stratum_root)
{
    (void)init_stratum_root; /* root has been pivoted; paths are absolute */

    const char *shared_dirs[] = { "/proc", "/dev", "/sys", NULL };

    for (int i = 0; shared_dirs[i]; i++) {
        const char *d = shared_dirs[i];

        char bedrock_path[MAX_PATH_LEN];
        snprintf(bedrock_path, sizeof(bedrock_path),
                 BEDROCK_ROOT "/strata/bedrock%s", d);
        mkdirp(bedrock_path, 0755);

        /* Make the bedrock copy shared so bind mounts propagate */
        mount(NULL, bedrock_path, NULL, MS_SHARED, NULL);

        /* rbind into the global namespace path */
        mkdirp(d, 0755);
        if (mount(bedrock_path, d, NULL, MS_BIND | MS_REC, NULL) < 0)
            notice(COL_YELLOW "warn" COL_RESET
                   ": rbind %s: %s", d, strerror(errno));
    }

    /* bedrock run tmpfs */
    char br_run[MAX_PATH_LEN];
    snprintf(br_run, sizeof(br_run),
             BEDROCK_ROOT "/strata/bedrock" BEDROCK_ROOT "/run");
    mkdirp(br_run, 0755);

    if (mount("bedrock_run", br_run, "tmpfs",
              MS_NOSUID | MS_NODEV, "mode=0755") < 0)
        notice(COL_YELLOW "warn" COL_RESET
               ": mount bedrock run tmpfs: %s", strerror(errno));

    /* Harden bedrock directory permissions */
    chmod(BEDROCK_ROOT "/strata/bedrock" BEDROCK_ROOT, 0755);
    chmod(br_run, 0755);

    /* rbind bedrock run into /bedrock/run */
    mkdirp(BEDROCK_ROOT "/run", 0755);
    mount(br_run, BEDROCK_ROOT "/run", NULL, MS_BIND | MS_REC, NULL);

    /* Force-remount /proc, /dev, /sys after pivot — the old mounts
     * are in the moved-old-root, not accessible at these paths anymore.
     * Unlike ensure_essential_environment(), this does not skip if
     * is_mounted() returns true (which reads stale /proc/mounts). */
    remount_essential_after_pivot();
}
