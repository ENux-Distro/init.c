/*
 * enux_conf.h — /enux/etc/enux.conf parser
 *
 * INI-style: [section] headers, then `key = value` lines. '#' and ';'
 * start comments; backslash-newline continues a line. The file is read
 * once into a static buffer (no malloc) and scanned per query.
 */

#ifndef ENUX_CONF_H
#define ENUX_CONF_H

#include "init.h"

/* First value for key in section. 0 and fills out, -1 if absent. */
int cfg_value(const char *section, const char *key, char *out, size_t outsz);

/* Comma-split values for key in section. Returns the count written. */
int cfg_values(const char *section, const char *key,
               char out[][MAX_PATH_LEN], int max);

/* Every key in section. Returns the count written. */
int cfg_keys(const char *section, char out[][MAX_PATH_LEN], int max);

#endif /* ENUX_CONF_H */
