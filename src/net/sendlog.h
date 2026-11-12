/* Sends the log files to a computer on the same network, so a tester need
 * not copy them off the SD card. The computer runs tools/logserver.py
 * (`make logs`); the console finds it with a broadcast, or at the address
 * in <data dir>/loghost.txt if that file exists. Nothing leaves the local
 * network. */
#ifndef SENDLOG_H
#define SENDLOG_H

#include <stdbool.h>
#include <stddef.h>

/* Sends log.prev.txt and log.txt from `dir`. Writes what happened to
 * `result` either way. */
bool sendlog_run(const char *dir, char *result, size_t result_size);

#endif
