/*
 * init.c - environment setup
 *
 * C equivalents of the shell's ensure_essential_environment() and
 * setup_term().  Mount checks read /proc/mounts directly; mounts are
 * direct mount(2) calls.  External binaries (modprobe, depmod, mdev,
 * plymouth-quit, manage_tty_lock) go through run_cmd().
 *
 * Called once at boot and again at the end of preenable_mounts(), like
 * the shell — every action is guarded so the second call is a no-op for
 * anything already in place.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#include "init.h"

/* Check if a filesystem type is mounted at a target path. */
static int is_mounted(const char *target, const char *fstype)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;

    char dev[256], mnt[256], fs[64];
    int  found = 0;

    /* Only the first three fields matter; discard the rest of each line. */
    while (fscanf(f, "%255s %255s %63s %*[^\n]", dev, mnt, fs) == 3) {
        if (asm_strcmp(mnt, target) == 0 &&
            (fstype == NULL || asm_strcmp(fs, fstype) == 0)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        warn("mkdir %s: %s", path, strerror(errno));
}

/* modprobe via /sbin/modprobe (a kmod symlink on Bedrock systems),
 * falling back to the bedrock stratum's kmod with argv[0] dispatch. */
static void modprobe(const char *mod, int silent)
{
    if (access("/sbin/modprobe", X_OK) == 0) {
        run_cmd((char *[]){ "/sbin/modprobe", (char *)mod, NULL }, silent);
        return;
    }
    if (access(BEDROCK_ROOT "/libexec/kmod", X_OK) == 0) {
        pid_t pid = fork();
        if (pid < 0)
            return;
        if (pid == 0) {
            if (silent) {
                int fd = open("/dev/null", O_WRONLY);
                if (fd >= 0) {
                    dup2(fd, STDOUT_FILENO);
                    dup2(fd, STDERR_FILENO);
                    close(fd);
                }
            }
            /* kmod dispatches on argv[0] */
            execv(BEDROCK_ROOT "/libexec/kmod",
                  (char *[]){ "modprobe", (char *)mod, NULL });
            _exit(127);
        }
        int ws;
        while (waitpid(pid, &ws, 0) < 0 && errno == EINTR)
            ;
    }
}

/* Check /proc/filesystems for a filesystem type. */
static int fs_available(const char *fstype)
{
    FILE *f = fopen("/proc/filesystems", "r");
    if (!f)
        return 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == '\t' || *p == ' ')
            p++;
        if (strncmp(p, "nodev", 5) == 0) {
            p += 5;
            while (*p == '\t' || *p == ' ')
                p++;
        }
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\n')
            p[len - 1] = '\0';
        if (asm_strcmp(p, fstype) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

void ensure_essential_environment(void)
{
    /* /proc — first, everything else keys off /proc/mounts */
    if (!is_mounted("/proc", "proc")) {
        ensure_dir("/proc");
        if (mount("proc", "/proc", "proc",
                  MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0 &&
            errno != EBUSY)
            warn("mount proc: %s", strerror(errno));
    }

    /* Remount root read-write; harmless if already rw. */
    if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) < 0)
        (void)0; /* matches the shell's `| head -n0` discard */

    /* /sys */
    if (!is_mounted("/sys", "sysfs")) {
        ensure_dir("/sys");
        if (mount("sysfs", "/sys", "sysfs",
                  MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0 &&
            errno != EBUSY)
            warn("mount sysfs: %s", strerror(errno));
    }

    /* /dev — followed by `mdev -s` like the shell, to populate any nodes
     * devtmpfs missed */
    if (!is_mounted("/dev", "devtmpfs")) {
        ensure_dir("/dev");
        if (mount("devtmpfs", "/dev", "devtmpfs",
                  MS_NOSUID, "mode=0755") < 0 && errno != EBUSY)
            warn("mount devtmpfs: %s", strerror(errno));
        if (access(BEDROCK_ROOT "/libexec/busybox", X_OK) == 0)
            run_cmd((char *[]){ BEDROCK_ROOT "/libexec/busybox",
                                "mdev", "-s", NULL }, 1);
    }

    /* /dev/pts */
    if (!is_mounted("/dev/pts", "devpts")) {
        ensure_dir("/dev/pts");
        if (mount("devpts", "/dev/pts", "devpts",
                  MS_NOSUID | MS_NOEXEC,
                  "mode=0620,gid=5,ptmxmode=0666") < 0 && errno != EBUSY)
            warn("mount devpts: %s", strerror(errno));
    }

    /* /run */
    if (!is_mounted("/run", "tmpfs")) {
        ensure_dir("/run");
        if (mount("tmpfs", "/run", "tmpfs",
                  MS_NOSUID | MS_NODEV, "mode=0755") < 0 && errno != EBUSY)
            warn("mount tmpfs on /run: %s", strerror(errno));
    }

    /* A non-zstd-aware depmod may have emptied modules.dep; regenerate. */
    {
        struct utsname uts;
        if (uname(&uts) == 0) {
            char mdep[MAX_PATH_LEN];
            snprintf(mdep, sizeof(mdep),
                     "/lib/modules/%s/modules.dep", uts.release);

            struct stat sb;
            if (stat(mdep, &sb) == 0 && sb.st_size == 0) {
                notice("Regenerating modules.dep...");
                if (access("/sbin/depmod", X_OK) == 0)
                    run_cmd((char *[]){ "/sbin/depmod", NULL }, 0);
            }
        }
    }

    /* fuse — crossfs depends on it */
    if (!fs_available("fuse"))
        modprobe("fuse", 0);

    if (access("/dev/fuse", F_OK) != 0) {
        ensure_dir("/dev");
        if (mknod("/dev/fuse", S_IFCHR | 0660, makedev(10, 229)) < 0 &&
            errno != EEXIST)
            warn("mknod /dev/fuse: %s", strerror(errno));
    }

    /* binfmt_misc — failure tolerated, like the shell's `|| true` */
    if (!fs_available("binfmt_misc"))
        modprobe("binfmt_misc", 1);

    /* Kill /run/systemd so a userland systemd doesn't think an
     * initrd-systemd is still its parent. */
    {
        struct stat sb;
        if (lstat("/run/systemd", &sb) == 0 && S_ISDIR(sb.st_mode)) {
            int rc = run_cmd((char *[]){ BEDROCK_ROOT "/libexec/busybox",
                                         "rm", "-r", "/run/systemd",
                                         NULL }, 1);
            if (rc != 0)
                run_cmd((char *[]){ "/bin/rm", "-rf", "/run/systemd",
                                    NULL }, 1);
        }
    }

    /* Remove /etc/resolv.conf symlinks; networking software recreates the
     * file and gets confused by stale symlinks across init switches. */
    {
        struct stat sb;
        if (lstat("/etc/resolv.conf", &sb) == 0 && S_ISLNK(sb.st_mode)) {
            if (unlink("/etc/resolv.conf") < 0)
                warn("unlink /etc/resolv.conf: %s", strerror(errno));
        }
    }
}

/*
 * setup_term()
 *
 * Shell equivalent:
 *   /bedrock/libexec/plymouth-quit
 *   /bedrock/libexec/manage_tty_lock unlock
 *   reset; stty sane; stty cooked; reset
 *
 * Plymouth and the tty lock need their helper binaries; the stty work is
 * a terminal reset escape plus tcsetattr — no forks.
 */
void setup_term(void)
{
    if (access(BEDROCK_ROOT "/libexec/plymouth-quit", X_OK) == 0)
        run_cmd((char *[]){ BEDROCK_ROOT "/libexec/plymouth-quit",
                            NULL }, 1);

    if (access(BEDROCK_ROOT "/libexec/manage_tty_lock", X_OK) == 0)
        run_cmd((char *[]){ BEDROCK_ROOT "/libexec/manage_tty_lock",
                            "unlock", NULL }, 1);

    /* RIS — full terminal reset, the escape `reset` ultimately sends. */
    asm_write_str(STDOUT_FILENO, "\033c");

    /* `stty sane`-equivalent cooked-mode settings. */
    struct termios t;
    int fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        fd = STDIN_FILENO;

    if (tcgetattr(fd, &t) == 0) {
        t.c_iflag |= ICRNL | IXON | BRKINT;
        t.c_iflag &= ~(unsigned)(IGNBRK | INLCR | IGNCR | ISTRIP);
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
        t.c_cflag |= CS8 | CREAD;
        if (tcsetattr(fd, TCSANOW, &t) < 0)
            warn("tcsetattr: %s", strerror(errno));
    }

    if (fd != STDIN_FILENO)
        close(fd);
}
