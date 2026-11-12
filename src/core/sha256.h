#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
/* Writes the digest as 64 lowercase hex characters plus a terminator. */
void sha256_final_hex(Sha256 *s, char out[65]);

#endif
