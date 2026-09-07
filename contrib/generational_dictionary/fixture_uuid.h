/* UUID generation/formatting for standalone POSIX research fixtures only.
 * Product owners provide IDs through their existing secure random source. */
#ifndef GD_FIXTURE_UUID_H
#define GD_FIXTURE_UUID_H
#include "dictionary.h"
#include <stdio.h>
#include <stdlib.h>

static inline GD_Epoch fixture_newEpoch(void)
{
    GD_Epoch epoch;
    FILE* random = fopen("/dev/urandom", "rb");
    if (!random || fread(epoch.bytes, 1, sizeof(epoch.bytes), random) != sizeof(epoch.bytes)) {
        fputs("fixture UUID random input failed\n", stderr);
        exit(1);
    }
    if (fclose(random)) { fputs("fixture UUID random close failed\n", stderr); exit(1); }
    epoch.bytes[6] = (unsigned char)((epoch.bytes[6] & 0x0f) | 0x40);
    epoch.bytes[8] = (unsigned char)((epoch.bytes[8] & 0x3f) | 0x80);
    return epoch;
}

static inline void fixture_initialEpochs(GD_Epoch epochs[GD_PARTITIONS])
{
    unsigned p;
    for (p = 0; p < GD_PARTITIONS; ++p) epochs[p] = fixture_newEpoch();
}

static inline void fixture_formatEpoch(GD_Epoch epoch, char text[37])
{
    static const char hex[] = "0123456789abcdef";
    unsigned i, at = 0;
    for (i = 0; i < sizeof(epoch.bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[at++] = '-';
        text[at++] = hex[epoch.bytes[i] >> 4];
        text[at++] = hex[epoch.bytes[i] & 15];
    }
    text[at] = 0;
}
#endif
