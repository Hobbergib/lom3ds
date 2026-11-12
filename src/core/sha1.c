/* SHA-1 (FIPS 180-1), used only to recognise the user's disc dump. */
#include "sha1.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->len = 0;
    s->fill = 0;
}

void sha1_update(Sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;

    s->len += len;
    while (len) {
        size_t n = 64 - s->fill;
        if (n > len) {
            n = len;
        }
        memcpy(s->buf + s->fill, p, n);
        s->fill += n;
        p += n;
        len -= n;
        if (s->fill == 64) {
            sha1_block(s, s->buf);
            s->fill = 0;
        }
    }
}

void sha1_final_hex(Sha1 *s, char out[41])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = {0x80};
    size_t padlen = (s->fill < 56 ? 56 : 120) - s->fill;
    int i;

    for (i = 0; i < 8; i++) {
        pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(s, pad, padlen + 8);
    for (i = 0; i < 20; i++) {
        uint8_t byte = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
        out[i * 2] = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 15];
    }
    out[40] = 0;
}
