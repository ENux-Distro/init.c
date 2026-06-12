/*
 * init.c - bedrock.conf parser
 *
 * Replaces common-code's cfg_preparse/cfg_value/cfg_values awk pipelines.
 *
 * The file is read once into a static buffer (no malloc) and preparsed in
 * place: backslash-newline continuations are joined and '#'/';' comments
 * are stripped, mirroring cfg_preparse exactly.  Queries then scan the
 * buffer line by line.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "init.h"
#include "bedrock_conf.h"

/* bedrock.conf ships at ~10KB; 64KB leaves generous headroom. */
static char cfg_buf[65536];
static size_t cfg_len    = 0;
static int    cfg_loaded = 0;   /* 0 = not tried, 1 = ok, -1 = failed */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

static int cfg_load(void)
{
    if (cfg_loaded)
        return cfg_loaded;

    int fd = open(BEDROCK_CONF, O_RDONLY);
    if (fd < 0) {
        warn("cannot open %s: %s", BEDROCK_CONF, strerror(errno));
        cfg_loaded = -1;
        return -1;
    }

    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, cfg_buf + total, sizeof(cfg_buf) - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            warn("read %s: %s", BEDROCK_CONF, strerror(errno));
            close(fd);
            cfg_loaded = -1;
            return -1;
        }
        if (n == 0)
            break;
        total += (size_t)n;
        if (total >= sizeof(cfg_buf) - 1) {
            warn("%s larger than %zu bytes; truncated",
                 BEDROCK_CONF, sizeof(cfg_buf) - 1);
            break;
        }
    }
    close(fd);
    cfg_buf[total] = '\0';

    /* Preparse pass 1: join "\<newline>" continuations. */
    size_t w = 0;
    for (size_t r = 0; r < total; r++) {
        if (cfg_buf[r] == '\\' && r + 1 < total && cfg_buf[r + 1] == '\n') {
            r++; /* drop both characters */
            continue;
        }
        cfg_buf[w++] = cfg_buf[r];
    }

    /* Preparse pass 2: strip '#' and ';' comments to end of line. */
    size_t w2 = 0;
    int in_comment = 0;
    for (size_t r = 0; r < w; r++) {
        char c = cfg_buf[r];
        if (c == '\n')
            in_comment = 0;
        else if (c == '#' || c == ';')
            in_comment = 1;
        if (!in_comment)
            cfg_buf[w2++] = c;
    }
    cfg_buf[w2] = '\0';
    cfg_len     = w2;

    cfg_loaded = 1;
    return 1;
}

/* Walk preparsed config lines.  For each key=value line inside [section]
 * whose key matches, invoke cb(value).  Returns number of matches. */
typedef void (*cfg_cb)(const char *value, void *ud);

static int cfg_foreach(const char *section, const char *key,
                       cfg_cb cb, void *ud)
{
    if (cfg_load() < 0)
        return 0;

    char line[1024];
    char cur_section[MAX_NAME_LEN] = "";
    int  matches = 0;

    size_t pos = 0;
    while (pos < cfg_len) {
        /* Extract one line into a local buffer we can mutate. */
        size_t end = pos;
        while (end < cfg_len && cfg_buf[end] != '\n')
            end++;
        size_t linelen = end - pos;
        if (linelen >= sizeof(line))
            linelen = sizeof(line) - 1;
        memcpy(line, cfg_buf + pos, linelen);
        line[linelen] = '\0';
        pos = end + 1;

        char *p = trim(line);
        if (*p == '\0')
            continue;

        if (*p == '[') {
            char *close = strchr(p + 1, ']');
            if (close) {
                *close = '\0';
                char *name = trim(p + 1);
                snprintf(cur_section, sizeof(cur_section), "%s", name);
            }
            continue;
        }

        if (asm_strcmp(cur_section, section) != 0)
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);

        if (asm_strcmp(k, key) == 0) {
            cb(v, ud);
            matches++;
        }
    }
    return matches;
}

/* cfg_value */

struct value_ud {
    char  *out;
    size_t outsz;
    int    found;
};

static void value_cb(const char *value, void *ud)
{
    struct value_ud *u = ud;
    if (u->found)
        return; /* first match wins */
    snprintf(u->out, u->outsz, "%s", value);
    u->found = 1;
}

int cfg_value(const char *section, const char *key, char *out, size_t outsz)
{
    struct value_ud u = { out, outsz, 0 };
    out[0] = '\0';
    cfg_foreach(section, key, value_cb, &u);
    return u.found ? 0 : -1;
}

/* cfg_values */

struct values_ud {
    char (*out)[MAX_PATH_LEN];
    int    max;
    int    count;
};

static void values_cb(const char *value, void *ud)
{
    struct values_ud *u = ud;

    /* Split on commas, trimming each piece. */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", value);

    char *p = tmp;
    while (p && u->count < u->max) {
        char *comma = strchr(p, ',');
        if (comma)
            *comma = '\0';
        char *piece = trim(p);
        if (*piece != '\0')
            snprintf(u->out[u->count++], MAX_PATH_LEN, "%s", piece);
        p = comma ? comma + 1 : NULL;
    }
}

int cfg_values(const char *section, const char *key,
               char out[][MAX_PATH_LEN], int max)
{
    struct values_ud u = { out, max, 0 };
    cfg_foreach(section, key, values_cb, &u);
    return u.count;
}
