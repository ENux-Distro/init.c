/* init.c
 * brl.h — native (pure C) reimplementation of the hot-path Bedrock brl
 *         tooling that runs while strata are enabled at boot.
 *
 * Scope (phase 1): a native replacement for `brl-enable --skip-crossfs`,
 * the per-stratum work that the shell forks once per show_boot stratum.
 * It replaces that script's fork-storm (every `stinit busybox mount` is a
 * fork→strat→fork→busybox chain) with direct syscalls, and hoists the two
 * O(strata^2) helpers — enforce_shells and enforce_id_ranges — out of the
 * per-stratum loop so they run once instead of once per stratum.
 *
 * brl-repair (bedrock + init stratum) and brl-apply remain shelled out for
 * now; see stratum.c.  The native path is gated behind the `enux_native`
 * kernel command-line flag, with the shell brl-enable kept as a live
 * fallback: native_enable_stratum() returns non-zero for any stratum it
 * cannot fully handle (e.g. a non-native CPU architecture needing QEMU),
 * and the caller then runs the shell brl-enable for that stratum.
 *
 * The native path assumes a clean boot: strata start disabled, so the
 * shell's disable_stratum() pre-pass (kill chrooted procs, unmount) is a
 * no-op and is skipped.  It is NOT a general-purpose `brl enable`.
 */

#ifndef ENUX_BRL_H
#define ENUX_BRL_H

#include "init.h"

/* True if the `enux_native` flag is present on the kernel command line. */
extern int native_brl_enabled;
void       native_brl_init(void);

/* Enable one stratum natively (mounts, etcfs, symlinks, enabled flag).
 * `name` is the dereferenced stratum name, `root` its /bedrock/strata/<name>
 * path.  Returns 0 on success, non-zero if the caller should fall back to
 * the shell brl-enable for this stratum.  Intended to run in a forked child. */
int  native_enable_stratum(const char *name, const char *root);

/* Per-mnt bedrock-side propagation setup, done once in the parent before
 * forking the per-stratum children so concurrent children never race on the
 * shared /bedrock mount tree. */
void native_prepare_shared_mounts(void);

/* The two O(strata^2) helpers, run once across all strata after the parallel
 * enable batch instead of once per stratum. */
void enforce_shells_all(InitState *st);
void enforce_id_ranges_all(InitState *st);

/* Native replacement for `brl-apply --skip-repair`.  Runs a copy of the stock
 * brl-apply with its two slow sections (login-shell passwd rewrite, restricted
 * commands) stripped out, then performs those two in C.  If the script's
 * section anchors can't be found (Bedrock changed them) it runs the stock
 * brl-apply unmodified.  Returns the script's exit status. */
int native_apply_run(InitState *st);

#endif /* ENUX_BRL_H */
