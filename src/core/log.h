/* The port's log file: <data dir>/log.txt, rewritten each launch, with the
 * previous one kept as log.prev.txt. It is what a tester sends back, so
 * every line is flushed as written and survives a crash. */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>

void log_open(const char *dir);
/* Gives the log a clock in microseconds; lines then start with the seconds
 * since this call. */
void log_set_clock(uint64_t (*now_us)(void));
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
