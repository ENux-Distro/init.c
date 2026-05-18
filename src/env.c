/*
 * init.c - environment setup
 *
 * Replaces the shell's ensure_essential_environment() and setup_term()
 * with direct libc/syscall equivalents. No forks, no subshells.
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
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <linux/loop.h>

#include "init.h"

/* Check if a filesystem type is already mounted at a target path.
 * Reads /proc/mounts directly — no grep subprocess. */
static int is_mounted(const char *target, const char *fstype)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;

    char dev[256], mnt[256], fs[64], opts[256];
    int  freq, pass;
    int  found = 0;

    while (fscanf(f, "%255s %255s %63s %255s %d %d\n",
                  dev, mnt, fs, opts, &freq, &pass) == 6) {
        if (strcmp(mnt, target) == 0 &&
            (fstype == NULL || strcmp(fs, fstype) == 0)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Ensure a directory exists (like mkdir -p but single-level for speed) */
static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        notice(COL_YELLOW "warn" COL_RESET ": mkdir %s: %s", path, strerror(errno));
}

/* Load a kernel module via modprobe (still needs a fork, but only done once) */
static void modprobe(const char *mod)
{
    pid_t pid = fork();
    if (pid == 0) {
        execv("/sbin/modprobe",
              (char *[]){ "modprobe", (char *)mod, NULL });
        _exit(1);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* Check if a filesystem type is available in /proc/filesystems */
static int fs_available(const char *fstype)
{
    FILE *f = fopen("/proc/filesystems", "r");
    if (!f) return 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Lines are either "nodev\tfstype" or "\tfstype" */
        char *p = line;
        while (*p == '\t' || *p == ' ') p++;
        /* skip "nodev\t" prefix if present */
        if (strncmp(p, "nodev", 5) == 0) {
            p += 5;
            while (*p == '\t' || *p == ' ') p++;
        }
        /* strip newline */
        size_t len = strlen(p);
        if (len > 0 && p[len-1] == '\n') p[len-1] = '\0';

        if (strcmp(p, fstype) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}


/*
 * ensure_essential_environment()
 *
 * Replaces this entire shell block with direct mount(2) calls:
 *
 *   mount -t proc proc /proc
 *   mount -o remount,rw /
 *   mount -t sysfs sysfs /sys
 *   mount -t devtmpfs devtmpfs /dev
 *   mount -t devpts devpts /dev/pts
 *   mount -t tmpfs tmpfs /run
 *   modprobe fuse
 *   mknod /dev/fuse c 10 229
 *   modprobe binfmt_misc
 *   depmod (if modules.dep is empty)
 *
 * All of these are syscalls or at most one fork each — no grep, no awk,
 * no shell string splitting.
 *
 * Uses is_mounted() to skip already-mounted filesystems (first call).
 */
void ensure_essential_environment(void)
{
    // proc
    if (!is_mounted("/proc", "proc")) {
        ensure_dir("/proc");
        if (mount("proc", "/proc", "proc",
                  MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0)
            /* Non-fatal: kernel may have already done this */
            notice(COL_YELLOW "warn" COL_RESET ": mount proc: %s", strerror(errno));
    }

    /* Remount root read-write */
    if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) < 0) {
        /* Non-fatal on some setups (already rw) */
        (void)0;
    }

    /* /sys*/
    if (!is_mounted("/sys", "sysfs")) {
        ensure_dir("/sys");
        mount("sysfs", "/sys", "sysfs",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    }

    /* /dev */
    if (!is_mounted("/dev", "devtmpfs")) {
        ensure_dir("/dev");
        if (mount("devtmpfs", "/dev", "devtmpfs",
                  MS_NOSUID | MS_STRICTATIME,
                  "mode=0755,size=10M") < 0) {
            notice(COL_YELLOW "warn" COL_RESET ": mount devtmpfs: %s", strerror(errno));
        }
    }

    /* /dev/pts */
    if (!is_mounted("/dev/pts", "devpts")) {
        ensure_dir("/dev/pts");
        mount("devpts", "/dev/pts", "devpts",
              MS_NOSUID | MS_NOEXEC,
              "mode=0620,gid=5,ptmxmode=0666");
    }

    /* /run */
    if (!is_mounted("/run", "tmpfs")) {
        ensure_dir("/run");
        mount("tmpfs", "/run", "tmpfs",
              MS_NOSUID | MS_NODEV | MS_STRICTATIME,
              "mode=0755,size=20%");
    }

    /* modules.dep sanity check */
    {
        struct utsname uts;
        uname(&uts);

        char mdep[MAX_PATH_LEN];
        snprintf(mdep, sizeof(mdep),
                 "/lib/modules/%s/modules.dep", uts.release);

        struct stat st;
        if (stat(mdep, &st) == 0 && st.st_size == 0) {
            notice("Regenerating modules.dep (was empty)...");
            pid_t pid = fork();
            if (pid == 0) {
                execv("/sbin/depmod", (char *[]){ "depmod", NULL });
                _exit(1);
            }
            if (pid > 0) waitpid(pid, NULL, 0);
        }
    }

    /* fuse  */
    if (!fs_available("fuse"))
        modprobe("fuse");

    if (access("/dev/fuse", F_OK) != 0) {
        ensure_dir("/dev");
        /* mknod /dev/fuse c 10 229 */
        mknod("/dev/fuse", S_IFCHR | 0660, makedev(10, 229));
    }

    /* binfmt_misc */
    if (!fs_available("binfmt_misc"))
        modprobe("binfmt_misc");

    /* Kill leftover /run/systemd from initrd confusion */
    {
        struct stat st;
        if (stat("/run/systemd", &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Simple recursive rmdir — just the top level is enough for
             * breaking the initrd-systemd→userland-systemd link.
             * A full recursive rm would need more code; the original
             * shell used `rm -r` so we replicate that via a fork. */
            pid_t pid = fork();
            if (pid == 0) {
                execv("/bin/rm",
                      (char *[]){ "rm", "-rf", "/run/systemd", NULL });
                _exit(1);
            }
            if (pid > 0) waitpid(pid, NULL, 0);
        }
    }

    /* resolv.conf symlink cleanup */
    {
        struct stat lst;
        if (lstat("/etc/resolv.conf", &lst) == 0 &&
            S_ISLNK(lst.st_mode)) {
            unlink("/etc/resolv.conf");
        }
    }
}

/*
 * remount_essential_after_pivot()
 *
 * Called after pivot_root to force-remount /proc, /sys, /dev, /run
 * unconditionally. After pivot the old root's mounts are gone or stale,
 * and /proc/mounts may not reflect the new root. Unlike
 * ensure_essential_environment(), this does NOT check is_mounted() —
 * it always tries to mount and accepts EBUSY as success.
 */
void remount_essential_after_pivot(void)
{
    /* Always try to re-establish /proc — do NOT check is_mounted
     * because /proc/mounts may be stale or missing after pivot. */
    ensure_dir("/proc");
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
        if (errno != EBUSY)
            notice(COL_YELLOW "warn" COL_RESET ": mount proc: %s", strerror(errno));
    }

    /* /sys */
    ensure_dir("/sys");
    if (mount("sysfs", "/sys", "sysfs",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
        if (errno != EBUSY)
            notice(COL_YELLOW "warn" COL_RESET ": mount sysfs: %s", strerror(errno));
    }

    /* /dev */
    ensure_dir("/dev");
    if (mount("devtmpfs", "/dev", "devtmpfs",
              MS_NOSUID | MS_STRICTATIME,
              "mode=0755,size=10M") < 0) {
        if (errno != EBUSY)
            notice(COL_YELLOW "warn" COL_RESET ": mount devtmpfs: %s", strerror(errno));
    }

    /* /dev/pts */
    ensure_dir("/dev/pts");
    if (mount("devpts", "/dev/pts", "devpts",
              MS_NOSUID | MS_NOEXEC,
              "mode=0620,gid=5,ptmxmode=0666") < 0) {
        if (errno != EBUSY)
            notice(COL_YELLOW "warn" COL_RESET ": mount devpts: %s", strerror(errno));
    }

    /* /run */
    ensure_dir("/run");
    if (mount("tmpfs", "/run", "tmpfs",
              MS_NOSUID | MS_NODEV | MS_STRICTATIME,
              "mode=0755,size=20%") < 0) {
        if (errno != EBUSY)
            notice(COL_YELLOW "warn" COL_RESET ": mount tmpfs: %s", strerror(errno));
    }
}

/*
 * setup_term()
 *
 * Replaces:
 *   /bedrock/libexec/plymouth-quit
 *   /bedrock/libexec/manage_tty_lock unlock
 *   reset; stty sane; stty cooked; reset
 *
 * We still need to fork for plymouth-quit (external binary).
 * The stty work is done with tcsetattr() directly — no subshells.
 */
void setup_term(void)
{
    /* Ask Plymouth to quit*/
    pid_t pid = fork();
    if (pid == 0) {
        execv("/bedrock/libexec/plymouth-quit",
              (char *[]){ "plymouth-quit", NULL });
        _exit(0); /* if not present, that's fine */
    }
    if (pid > 0) waitpid(pid, NULL, 0);

    /* Unlock TTY (manage_tty_lock)*/
    pid = fork();
    if (pid == 0) {
        execv("/bedrock/libexec/manage_tty_lock",
              (char *[]){ "manage_tty_lock", "unlock", NULL });
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);

    /* Apply sane terminal settings via tcsetattr*/
    struct termios t;
    int fd = open("/dev/tty1", O_RDWR | O_NOCTTY);
    if (fd < 0) fd = STDIN_FILENO;

    if (tcgetattr(fd, &t) == 0) {
        /* cfmakeraw then flip back to sane cooked mode */
        t.c_iflag |= ICRNL | IXON | BRKINT;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
        t.c_cflag |= CS8 | CREAD;
        tcsetattr(fd, TCSANOW, &t);
    }

    if (fd != STDIN_FILENO) close(fd);

    /* clear */
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
}
