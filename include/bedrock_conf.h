/* init.c
 * bedrock_conf.h — /bedrock/etc/bedrock.conf parser
 *
 * C replacement for common-code's cfg_preparse / cfg_value / cfg_values.
 * Semantics match the shell versions:
 *   - lines ending in backslash-newline are joined
 *   - '#' and ';' start comments
 *   - cfg_value returns a whole value for a [section] key
 *   - cfg_values splits a comma-separated value list, one value per entry;
 *     repeated keys accumulate
 */

#ifndef ENUX_BEDROCK_CONF_H
#define ENUX_BEDROCK_CONF_H

#include <stddef.h>
#include "init.h"

/* Fetch the value of `key` in `[section]`.  First match wins.
 * Returns 0 and fills `out` on success, -1 if absent/unreadable. */
int cfg_value(const char *section, const char *key, char *out, size_t outsz);

/* Fetch all comma-separated values of `key` in `[section]`.
 * Fills up to `max` entries of `out`; returns the count (0 if none). */
int cfg_values(const char *section, const char *key,
               char out[][MAX_PATH_LEN], int max);

/* List every key in `[section]`, in file order, not deduplicated — matches
 * the shell cfg_keys.  Fills up to `max` entries; returns the count. */
int cfg_keys(const char *section, char out[][MAX_PATH_LEN], int max);

#endif /* ENUX_BEDROCK_CONF_H */
