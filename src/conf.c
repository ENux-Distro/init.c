/*
 * init.c - bedrock.conf parser
 *
 * Replaces cfg_values() shell function which piped through awk.
 * Simple hand-rolled INI parser; no dynamic allocation.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include "init.h"

/* Strip leading and trailing whitespace in-place */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end-1))) end--;
    *end = '\0';
    return s;
}

/*
 * parse_bedrock_conf()
 *
 * We need from bedrock.conf:
 *   [init]
 *   default = stratum:cmd
 *   timeout = N
 *
 * The file is a simple INI. We parse it linearly.
 */
int parse_bedrock_conf(InitState *st)
{
    FILE *f = fopen(BEDROCK_CONF, "r");
    if (!f) {
        notice(COL_YELLOW "warn" COL_RESET
               ": cannot open %s: %s (using defaults)",
               BEDROCK_CONF, strerror(errno));
        st->timeout = 30;
        st->default_tuple[0] = '\0';
        return 0;
    }

    char line[1024];
    char section[64] = {0};
    int  in_init = 0;

    st->timeout = 30; /* default */
    st->default_tuple[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);

        /* Skip comments and empty lines */
        if (*p == '\0' || *p == '#') continue;

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p + 1, ']');
            if (end) {
                *end = '\0';
                strncpy(section, p + 1, sizeof(section) - 1);
                in_init = (strcmp(section, "init") == 0);
            }
            continue;
        }

        if (!in_init) continue;

        /* key = value */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (strcmp(key, "default") == 0) {
            strncpy(st->default_tuple, val, MAX_PATH_LEN - 1);
        } else if (strcmp(key, "timeout") == 0) {
            st->timeout = atoi(val);
        }
    }

    fclose(f);
    return 0;
}
