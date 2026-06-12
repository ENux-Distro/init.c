# Installing init.c

init.c is installed **alongside** the original Bedrock Linux init, never in
place of it. The original shell init at `/bedrock/strata/bedrock/sbin/init`
stays byte-for-byte intact: it is option 2 in the ENux Boot Manager, the
automatic fallback for every pre-pivot failure, and your manual rollback
path. `make install` refuses to write to that location.

## 1. Build

```bash
make
make check
```

Produces `build/init` — statically linked, stripped ELF64. `make check`
verifies ELF type, static linkage, forbidden-symbol absence, and the ASM
helpers.

For a host-side debug binary with ASan/UBSan (not bootable — ASan requires
dynamic linking):

```bash
make DEBUG=1        # → build/debug/init.debug
```

## 2. Install

```bash
sudo make install
```

Installs to `/bedrock/strata/bedrock/sbin/enux-init` (override with
`INSTALL_PATH=`). Nothing else on the system is modified.

## 3. Activate

Point the kernel at the new init via the `init=` parameter. On ENux/Debian
with GRUB:

```bash
# /etc/default/grub
GRUB_CMDLINE_LINUX="init=/bedrock/strata/bedrock/sbin/enux-init"
```

```bash
sudo update-grub
```

Recommended: add a second GRUB menu entry *without* the `init=` parameter
before rebooting, so the original Bedrock init is one menu choice away even
if init.c fails before showing its selector.

## 4. What boots

1. **ENux Boot Manager** — 5-second selector:
   - `1. ENux init (init.c)` — default, continues below
   - `2. Bedrock Linux init` — execs the untouched shell init immediately;
     Bedrock handles the entire boot from there
2. Hijack/upgrade completion (first boot only), bedrock.conf `[init]`
   parsing, init selection menu (or `bedrock_init=stratum:cmd` from the
   kernel command line), fstab, pivot, parallel strata enable, `brl-apply`,
   exec of the chosen init.

Per-stratum `brl-enable` output is logged to `/bedrock/run/enux-init/` —
check there if a stratum fails to enable.

## 5. Rollback

Any one of these returns you to a stock Bedrock boot:

- **At boot, transient:** pick option 2 in the ENux Boot Manager, or edit
  the GRUB entry (`e`) and delete the `init=` parameter.
- **Permanent:** remove `init=` from `GRUB_CMDLINE_LINUX` in
  `/etc/default/grub`, run `update-grub`.
- **Cleanup (optional):** `sudo rm /bedrock/strata/bedrock/sbin/enux-init`.

Because the original init was never moved or modified, rollback never
involves restoring a backup.

## 6. Failure behavior

| Phase | On fatal error |
|---|---|
| Not PID 1 | exec `/bedrock/strata/bedrock/sbin/init.sh.bak` (Bedrock's standard backup) |
| Before pivot_root | exec the original Bedrock shell init |
| From pivot_root on | emergency shell (matching the shell init's `fatal_error`) |

Non-fatal problems (a stratum failing to enable, fstab mount errors, LVM
absence) are warned and boot continues, matching the shell init's `set +e`
behavior.
