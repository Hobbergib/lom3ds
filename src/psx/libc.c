/* The C library subset the game links from the SDK. */
#include "psx.h"

#include <stdlib.h>
#include <string.h>

static uint32_t s_rand_state = 1;

void *psx_memcpy(void *dst, const void *src, int n)
{
    return memmove(dst, src, (size_t)n);
}

void *psx_memset(void *dst, int value, int n)
{
    return memset(dst, value, (size_t)n);
}

void *psx_bcopy(const void *src, void *dst, int n)
{
    return memmove(dst, src, (size_t)n);
}

void *psx_bzero(void *dst, int n)
{
    return memset(dst, 0, (size_t)n);
}

char *psx_strcat(char *dst, const char *src)
{
    return strcat(dst, src);
}

char *psx_strcpy(char *dst, const char *src)
{
    return strcpy(dst, src);
}

char *psx_strncpy(char *dst, const char *src, int n)
{
    return strncpy(dst, src, (size_t)n);
}

int psx_strlen(const char *s)
{
    return (int)strlen(s);
}

int psx_strncmp(const char *a, const char *b, int n)
{
    return strncmp(a, b, (size_t)n);
}

int psx_abs(int v)
{
    return v < 0 ? -v : v;
}

/* The SDK's generator; the game's random events depend on its sequence. */
int psx_rand(void)
{
    s_rand_state = s_rand_state * 1103515245u + 12345u;
    return (int)((s_rand_state >> 16) & 0x7FFF);
}

void psx_srand(unsigned int seed)
{
    s_rand_state = seed;
}

void psx_exit(int code)
{
    exit(code);
}
