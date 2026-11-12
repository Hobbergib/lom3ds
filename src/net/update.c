#include "update.h"

#include "../core/sha256.h"
#include "../plat/plat.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#ifndef UPDATE_REPO
#error "UPDATE_REPO (owner/name on GitHub) is set by the Makefile"
#endif
#define RELEASE_URL "https://github.com/" UPDATE_REPO "/releases/latest/download/"
#define MAX_PROGRAM_SIZE (64ul << 20)

typedef struct {
    char *text; /* collects a small text file, or NULL */
    size_t text_size, text_len;
    FILE *file; /* receives a download, or NULL */
    Sha256 hash;
    unsigned long done, total;
    UpdateProgress progress;
} Transfer;

static size_t receive(char *data, size_t size, size_t count, void *user)
{
    Transfer *t = user;
    size_t n = size * count;

    if (t->text) {
        if (t->text_len + n >= t->text_size) {
            return 0; /* larger than any version.txt: not ours */
        }
        memcpy(t->text + t->text_len, data, n);
        t->text_len += n;
        t->text[t->text_len] = '\0';
    }
    if (t->file) {
        if (t->done + n > t->total || fwrite(data, 1, n, t->file) != n) {
            return 0;
        }
        sha256_update(&t->hash, data, n);
        t->done += (unsigned long)n;
        if (t->progress) {
            t->progress(t->done, t->total);
        }
    }
    return n;
}

static bool fetch(const char *name, Transfer *t, char *err, size_t err_size)
{
    char url[256], reason[CURL_ERROR_SIZE] = "";
    size_t ca_size;
    const void *ca = plat_ca_bundle(&ca_size);
    long status = 0;
    CURLcode rc;
    CURL *curl;

    if (!plat_net_start(err, err_size)) {
        return false;
    }
    curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_size, "could not start the download library");
        return false;
    }
    snprintf(url, sizeof url, RELEASE_URL "%s", name);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lom3ds-updater");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (ca) {
        struct curl_blob blob = {(void *)ca, ca_size, CURL_BLOB_NOCOPY};

        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, reason);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc == CURLE_OK) {
        return true;
    }
    if (status == 404) {
        snprintf(err, err_size, "no release has been published yet");
    } else {
        snprintf(err, err_size, "%s", reason[0] ? reason : curl_easy_strerror(rc));
    }
    return false;
}

bool update_check(UpdateInfo *info, char *err, size_t err_size)
{
    char text[512] = "";
    Transfer t = {.text = text, .text_size = sizeof text};

    if (!fetch("version.txt", &t, err, err_size)) {
        return false;
    }
    memset(info, 0, sizeof *info);
    if (sscanf(text, "build %23[0-9-] size %lu sha256 %64[0-9a-f]", info->build, &info->size, info->sha256) != 3 ||
        strlen(info->sha256) != 64 || info->size == 0 || info->size > MAX_PROGRAM_SIZE) {
        snprintf(err, err_size, "the release's version.txt could not be understood");
        return false;
    }
    /* Later releases also carry an installable package. */
    {
        const char *cia = strstr(text, "cia_size ");

        if (!cia || sscanf(cia, "cia_size %lu cia_sha256 %64[0-9a-f]", &info->cia_size, info->cia_sha256) != 2 ||
            strlen(info->cia_sha256) != 64 || info->cia_size == 0 || info->cia_size > MAX_PROGRAM_SIZE) {
            info->cia_size = 0;
            info->cia_sha256[0] = '\0';
        }
    }
    return true;
}

bool update_download(const char *name, unsigned long size, const char *sha256, const char *target, UpdateProgress progress,
                     char *err, size_t err_size)
{
    static char buffer[1 << 16];
    Transfer t = {.total = size, .progress = progress};
    char fresh[512], old[512], digest[65];
    bool ok;

    snprintf(fresh, sizeof fresh, "%s.new", target);
    snprintf(old, sizeof old, "%s.old", target);
    t.file = fopen(fresh, "wb");
    if (!t.file) {
        snprintf(err, err_size, "could not create %s", fresh);
        return false;
    }
    setvbuf(t.file, buffer, _IOFBF, sizeof buffer);
    sha256_init(&t.hash);
    ok = fetch(name, &t, err, err_size);
    if (fclose(t.file) != 0 && ok) {
        snprintf(err, err_size, "could not write %s (is the SD card full?)", fresh);
        ok = false;
    }
    if (ok) {
        sha256_final_hex(&t.hash, digest);
        if (t.done != size || strcmp(digest, sha256) != 0) {
            snprintf(err, err_size, "the download does not match the release; nothing was changed");
            ok = false;
        }
    }
    if (!ok) {
        remove(fresh);
        return false;
    }
    /* The SD card's file system does not rename over an existing file. */
    remove(old);
    if (rename(target, old) != 0) {
        remove(target);
    }
    if (rename(fresh, target) != 0) {
        rename(old, target);
        snprintf(err, err_size, "could not replace %s", target);
        return false;
    }
    remove(old);
    return true;
}
