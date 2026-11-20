#include "log.h"

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>

/* Keep the on-disk log small. Per-frame logging used to grow junkbot.log
   without bound and every line forced an fflush(), which wore on the SD card.
   We cap the file at a fixed number of lines and only flush periodically. */
#define LOG_MAX_LINES 400
#define LOG_FLUSH_EVERY 20

static FILE *s_log = NULL;
static int s_lines = 0;

void log_init(void) {
    if (s_log) return;
    s_log = fopen("sdmc:/junkbot.log", "w");
    if (!s_log) s_log = fopen("sdmc:/switch/junkbot.log", "w");
    if (s_log) {
        fprintf(s_log, "=== Junkbot startup ===\n");
        s_lines = 1;
        fflush(s_log);
    }
}

void log_exit(void) {
    if (s_log) {
        fprintf(s_log, "=== Junkbot exit ===\n");
        fflush(s_log);
        fclose(s_log);
        s_log = NULL;
    }
}

void log_printf(const char *fmt, ...) {
    if (!s_log || s_lines >= LOG_MAX_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    s_lines++;
    if (s_lines % LOG_FLUSH_EVERY == 0 || s_lines >= LOG_MAX_LINES)
        fflush(s_log);
}
