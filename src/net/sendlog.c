#include "sendlog.h"

#include "../plat/plat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG_PORT 28771
#define DISCOVER "LOM3DS-LOG-DISCOVER"
#define DISCOVER_REPLY "LOM3DS-LOG-HERE"
#define DISCOVER_TRIES 20 /* tenths of a second */

/* Asks the local network who is collecting logs. */
static bool discover(struct in_addr *host)
{
    struct sockaddr_in to = {0}, from;
    int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1, i;
    bool found = false;

    if (s < 0) {
        return false;
    }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    to.sin_family = AF_INET;
    to.sin_port = htons(LOG_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    for (i = 0; i < DISCOVER_TRIES && !found; i++) {
        char reply[32];
        socklen_t len = sizeof from;
        long n;

        if (i % 5 == 0) {
            sendto(s, DISCOVER, sizeof DISCOVER - 1, 0, (struct sockaddr *)&to, sizeof to);
        }
        plat_sleep_us(100000);
        n = recvfrom(s, reply, sizeof reply, 0, (struct sockaddr *)&from, &len);
        if (n == (long)sizeof DISCOVER_REPLY - 1 && !memcmp(reply, DISCOVER_REPLY, (size_t)n)) {
            *host = from.sin_addr;
            found = true;
        }
    }
    close(s);
    return found;
}

static bool send_all(int s, const char *data, size_t size)
{
    while (size > 0) {
        long n = send(s, data, size, 0);

        if (n <= 0) {
            return false;
        }
        data += n;
        size -= (size_t)n;
    }
    return true;
}

/* One file: a header line with its name and size, then its bytes. */
static bool send_file(int s, const char *dir, const char *name)
{
    static char buffer[4096];
    char path[512], header[96];
    long size;
    size_t n;
    bool ok = true;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    snprintf(header, sizeof header, "LOM3DS-LOG %s %ld\n", name, size);
    ok = send_all(s, header, strlen(header));
    while (ok && size > 0 && (n = fread(buffer, 1, sizeof buffer, f)) > 0) {
        if ((long)n > size) {
            n = (size_t)size; /* the log grew while it was being read */
        }
        ok = send_all(s, buffer, n);
        size -= (long)n;
    }
    fclose(f);
    return ok && size == 0;
}

bool sendlog_run(const char *dir, char *result, size_t result_size)
{
    struct sockaddr_in to = {0};
    char path[512], text[64] = "";
    int s, sent = 0;
    FILE *f;

    if (!plat_net_start(result, result_size)) {
        return false;
    }
    to.sin_family = AF_INET;
    to.sin_port = htons(LOG_PORT);
    snprintf(path, sizeof path, "%s/loghost.txt", dir);
    f = fopen(path, "r");
    if (f) {
        if (fgets(text, sizeof text, f)) {
            text[strcspn(text, " \r\n")] = '\0';
        }
        fclose(f);
    }
    if (text[0] ? inet_pton(AF_INET, text, &to.sin_addr) != 1 : !discover(&to.sin_addr)) {
        snprintf(result, result_size, "No computer is collecting logs. Run \"make logs\" on it first.");
        return false;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&to, sizeof to) != 0) {
        if (s >= 0) {
            close(s);
        }
        snprintf(result, result_size, "Could not connect to the computer collecting logs.");
        return false;
    }
    sent += send_file(s, dir, "log.prev.txt");
    sent += send_file(s, dir, "log.txt");
    close(s);
    snprintf(result, result_size, sent ? "Sent %d log file%s to the computer." : "The logs could not be sent.", sent, sent == 1 ? "" : "s");
    return sent > 0;
}
