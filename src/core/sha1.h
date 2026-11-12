#ifndef SHA1_H
#define SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, size_t len);
/* Writes the digest as 40 lowercase hex characters plus a terminator. */
void sha1_final_hex(Sha1 *s, char out[41]);

#endif
