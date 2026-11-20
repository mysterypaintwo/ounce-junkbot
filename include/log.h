#ifndef JUNKBOT_LOG_H
#define JUNKBOT_LOG_H

/* Writes diagnostics to sdmc:/junkbot.log (flushed after every line) so a
   failure can be inspected after pulling the SD card. */
void log_init(void);
void log_exit(void);
void log_printf(const char *fmt, ...);

#define LOG(...) log_printf(__VA_ARGS__)

#endif
