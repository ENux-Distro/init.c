# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

init.c is a statically linked C + x86_64 NASM replacement for Bedrock Linux's shell-based init. It runs as PID 1: mounts essential filesystems, presents a stratum/init selection menu, pivots root into the chosen stratum, enables all strata **in parallel** (the key performance win over the sequential shell script), and `execv()`s the real init. Built for ENux, x86_64 Linux only.

## Commands

```bash
make            # release build → build/init (static, stripped)
make DEBUG=1    # → build/debug/init.debug (-g3, ASan/UBSan; host testing only, not bootable)
make check      # smoke tests: ELF64, static linkage, forbidden symbols, ASM symbols
make clean
sudo make install  # installs ALONGSIDE the original init at /bedrock/strata/bedrock/sbin/enux-init
make size       # section sizes per object
make disasm     # objdump disassembly
```

The original Bedrock shell init at `/bedrock/strata/bedrock/sbin/init` must never be overwritten — it is the dual-boot selector's option 2 and the pre-pivot fallback. `make install` refuses to target it. Activation is via the kernel `init=` parameter (see INSTALL.md). Reference copies of the upstream shell `init` and `common-code` live in the repo root; behavior changes must stay parity-checked against them.

There is no unit test suite; `make check` is the only automated verification. Real testing requires booting a Bedrock Linux system (developed/tested on ENux 5.3.3). Requires gcc, nasm, binutils.

## Architecture

Boot flow lives in `src/main.c` and runs top to bottom, mirroring the upstream shell init step for step:

1. not PID 1 → exec `/bedrock/strata/bedrock/sbin/init.sh.bak`
2. `env.c` — essential mounts (/proc, /sys, /dev, /dev/pts, /run), fuse/binfmt_misc modules, modules.dep regen, /run/systemd + resolv.conf cleanup; TTY setup (plymouth-quit, manage_tty_lock)
3. `menu.c` `run_enux_selector()` — ENux Boot Manager: option 2 execs the original shell init; runs **before any state mutation** so the handoff is clean
4. `hijack.c` — `complete_hijack()` (first boot after hijack install, flag `/bedrock/complete-hijack-install`) and `complete_upgrade()`
5. `bedrock_conf.c` — INI parser matching cfg_preparse semantics (line continuations, `#`/`;` comments, comma-split multi-values)
6. `menu.c` `run_menu()` — init candidates = `[init] paths` × `show_init` strata, resolved by `stratum_realpath()`; invalid `[init] default` means wait-forever; `bedrock_init=stratum:cmd` on the kernel cmdline skips the menu
7. `mount.c` — fstab (dmsetup/lvm/`busybox mount -a` via fork), `pivot_root_to()` (non-recursive binds + MS_MOVE of old root onto /bedrock/strata/bedrock), `preenable_mounts()`
8. `stratum.c` — `enable_strata_parallel()`: brl-repair bedrock + init stratum sequential foreground, then one forked `brl-enable` per show_boot stratum reaped via `waitpid(-1)` (ignore unknown PIDs — orphans reparent to PID 1); children log to `/bedrock/run/enux-init/*.log`
9. `brl-apply --skip-repair`, then `execv()` of the *configured* cmd (e.g. `/sbin/init`), resolved naturally post-pivot — never returns

Failure policy: pre-pivot fatal → `fallback_to_bedrock_init()` (exec the shell init); post-pivot fatal → `panic()` (emergency shell, like upstream `fatal_error`). Non-fatal problems warn and continue (upstream runs `set +e`).

Shared state is the `InitState`/`Stratum` structs in `include/init.h` (fixed-size arrays, no malloc anywhere, `MAX_STRATA` = 16). `asm/asm_util.asm` provides `asm_memzero`, `asm_strcmp`, `asm_write_str` (async-signal-safe panic output), `asm_poll_read` (menu keyboard wait + countdown), and `asm_usleep`.

## Hard constraints

- **No `system()` or `popen()`** — `make check` fails the build if they appear. External programs (`brl-enable`, `brl-repair`, `brl-apply`, dmsetup/lvm) run via `fork()` + `execv()`.
- **Must stay fully static** (`-static`); this runs before ld.so is usable. Don't add dynamic library dependencies.
- Output in signal-safe/panic paths must use direct `write(2)` or `asm_write_str`, not stdio.
- Failure paths should fall back to the backup init (`/bedrock/strata/bedrock/sbin/init.sh.bak` / `.bin.bak`), never hang PID 1.
- The Makefile must never download anything during `make install`; network fetch is only in the separate `install-fallback` target.

## Code conventions

- C11 with `-D_GNU_SOURCE`; builds with `-Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes` — keep it warning-clean
- Allman brace style, descriptive variable names, static helpers where possible
- New C files must be added to `C_SRCS` in the Makefile and prototyped in `include/init.h`
