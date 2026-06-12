/* init.c
 * stratum.h — stratum listing, xattr attributes, in-stratum path resolution
 *
 * C replacements for common-code's list_strata, deref, has_attr, get_attr,
 * set_attr, and the chroot+realpath dance used by list_init_options.
 */

#ifndef ENUX_STRATUM_H
#define ENUX_STRATUM_H

#include <stddef.h>
#include "init.h"

/* list_strata: fill st->strata from /bedrock/strata.
 * Directories only — symlinks are aliases and are excluded, matching
 * `find /bedrock/strata -maxdepth 1 -type d`.
 * Returns stratum count, -1 on error. */
int scan_strata(InitState *st);

/* deref: resolve a stratum alias to its real stratum name.  A non-alias
 * name resolves to itself.  Returns 0 on success, -1 on failure. */
int deref(const char *name, char *out, size_t outsz);

/* has_attr / get_attr / set_attr on the user.bedrock.* xattr namespace.
 * `attr` is the short name, e.g. "show_boot". */
int br_has_attr(const char *path, const char *attr);
int br_get_attr(const char *path, const char *attr, char *out, size_t outsz);
int br_set_attr(const char *path, const char *attr, const char *val);

/* Resolve `path` as seen from inside `root` (symlinks resolved relative to
 * the stratum root, exactly like chroot+realpath).  Every component must
 * exist.  On success writes the in-stratum absolute path (e.g.
 * "/lib/systemd/systemd") to `out` and returns 0; -1 on failure. */
int stratum_realpath(const char *root, const char *path,
                     char *out, size_t outsz);

/* Enable bedrock + init stratum via brl-repair (sequential), then all other
 * show_boot strata via brl-enable — all forked in parallel. */
void enable_strata_parallel(InitState *st);

#endif /* ENUX_STRATUM_H */
