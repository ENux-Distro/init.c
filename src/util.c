/*
 * init.c - utility functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "init.h"

void panic(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    asm_write_str(STDERR_FILENO, "\n");
    asm_write_str(STDERR_FILENO, COL_RED "FATAL: " COL_RESET);
    asm_write_str(STDERR_FILENO, buf);
    asm_write_str(STDERR_FILENO, "\n\n");
    asm_write_str(STDERR_FILENO, "Starting emergency shell. Good luck.\n\n");

    /* exec sh — last resort */
    execv("/bedrock/libexec/busybox",
          (char *[]){ "sh", NULL });
    execv("/bin/sh",  (char *[]){ "sh", NULL });
    execv("/sbin/sh", (char *[]){ "sh", NULL });

    /* If even that failed, we get a kernel panic — appropriate. */
    _exit(1);
}

void notice(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    write(STDOUT_FILENO, "  ", 2);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\n", 1);
}

void step(int n, int total, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char prefix[64];
    snprintf(prefix, sizeof(prefix),
             COL_DIM "[%d/%d]" COL_RESET " ", n, total);

    write(STDOUT_FILENO, prefix, strlen(prefix));
    write(STDOUT_FILENO, buf,    strlen(buf));
    write(STDOUT_FILENO, "\n",   1);
}
