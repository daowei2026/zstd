/* Reproducible capacity and codec measurements, repository BSD license. */
#define _POSIX_C_SOURCE 200809L
#include "dictionary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define FRAME 1514
static uint64_t rng = 0x873bad93281ULL;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static void random_bytes(void* dst, size_t n) { size_t i; for (i = 0; i < n; ++i) ((unsigned char*)dst)[i] = (unsigned char)random32(); }
static double now(void) { struct timespec ts; CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static long rss(void) {
    FILE* f = fopen("/proc/self/status", "r"); char line[256]; long k = 0;
    if (f) { while (fgets(line, sizeof(line), f)) if (sscanf(line, "VmRSS: %ld kB", &k) == 1) break; fclose(f); }
    return k;
}
static int compare(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }

int main(int argc, char** argv)
{
    uint32_t cap = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 30000000;
    unsigned frames = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 10000;
    int level = argc > 3 ? atoi(argv[3]) : 3;
    unsigned char scratch[65536], frame[FRAME], output[FRAME], compressed[2048];
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    double *ct, *dt, init_start = now(), generation = 0, setup, csum = 0, dsum = 0;
    uint64_t bytes[2] = {0, 0}, cases[2] = {0, 0};
    unsigned p, i; long initialized_rss;
    struct rusage usage;
#ifdef GD_BASELINE
    unsigned char *tx, *rx; ZSTD_CDict* cd; ZSTD_DDict* dd;
    ZSTD_compressionParameters params = ZSTD_getCParams(3, 0, (size_t)cap * 6);
    const char* kind = level == 0 ? "upstream_no_dict" : level == 3 ? "upstream_level3" : "upstream_wide_index";
#else
    GD_Store *tx, *rx; GD_FrameView view;
    const char* kind = "segmented";
    (void)level;
#endif
    CHECK(cap >= FRAME && cap <= 100000000 && frames >= 10 && frames <= 1000000 && cc && dc);
    ct = (double*)calloc(frames, sizeof(*ct)); dt = (double*)calloc(frames, sizeof(*dt)); CHECK(ct && dt);
#ifdef GD_BASELINE
    tx = (unsigned char*)malloc((size_t)cap * 6); rx = (unsigned char*)malloc((size_t)cap * 6); CHECK(tx && rx);
#else
    tx = GD_create(cap, 1); rx = GD_create(cap, 0); CHECK(tx && rx);
#endif
    for (p = 0; p < 6; ++p) {
        uint32_t offset = 0;
        while (offset < cap) {
            size_t n = cap - offset; double t;
            if (n > sizeof(scratch)) n = sizeof(scratch);
            t = now(); random_bytes(scratch, n); generation += now() - t;
#ifdef GD_BASELINE
            memcpy(tx + (size_t)p * cap + offset, scratch, n); memcpy(rx + (size_t)p * cap + offset, scratch, n);
#else
            CHECK(GD_write(tx, p, GD_epoch(tx, p), offset, scratch, n) == GD_OK);
            CHECK(GD_write(rx, p, GD_epoch(rx, p), offset, scratch, n) == GD_OK);
#endif
            offset += (uint32_t)n;
        }
    }
#ifdef GD_BASELINE
    if (level != 3 && level != 0) { params.windowLog = 29; params.hashLog = 25; params.chainLog = 25; params.strategy = ZSTD_greedy; }
    cd = level ? ZSTD_createCDict_advanced(tx, (size_t)cap * 6, ZSTD_dlm_byRef, ZSTD_dct_rawContent, params, ZSTD_defaultCMem) : NULL;
    dd = level ? ZSTD_createDDict_advanced(rx, (size_t)cap * 6, ZSTD_dlm_byRef, ZSTD_dct_rawContent, ZSTD_defaultCMem) : NULL;
    CHECK(!level || (cd && dd));
#endif
    setup = now() - init_start - generation; initialized_rss = rss();
    for (i = 0; i < frames; ++i) {
        size_t size, decoded; double t; unsigned category = i % 3 != 0;
        random_bytes(frame, sizeof(frame));
        if (category) for (p = 0; p < 3; ++p) {
            unsigned slot = 2 * p + random32() % 2; uint32_t at = random32() % (cap - 480);
#ifdef GD_BASELINE
            memcpy(frame + p * 480, tx + (size_t)slot * cap + at, 480);
#else
            CHECK(GD_read(tx, slot, GD_epoch(tx, slot), at, frame + p * 480, 480) == GD_OK);
#endif
        }
        t = now();
#ifdef GD_BASELINE
        size = level ? ZSTD_compress_usingCDict(cc, compressed, sizeof(compressed), frame, sizeof(frame), cd) :
                       ZSTD_compressCCtx(cc, compressed, sizeof(compressed), frame, sizeof(frame), 3);
#else
        size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
#endif
        ct[i] = (now() - t) * 1e6; CHECK(!ZSTD_isError(size));
        t = now();
#ifdef GD_BASELINE
        decoded = level ? ZSTD_decompress_usingDDict(dc, output, sizeof(output), compressed, size, dd) :
                          ZSTD_decompressDCtx(dc, output, sizeof(output), compressed, size);
#else
        decoded = GD_decompress(rx, dc, output, sizeof(output), compressed, size, &view);
#endif
        dt[i] = (now() - t) * 1e6;
        CHECK(decoded == sizeof(frame) && memcmp(frame, output, sizeof(frame)) == 0);
        bytes[category] += size; ++cases[category]; csum += ct[i]; dsum += dt[i];
    }
    qsort(ct, frames, sizeof(*ct), compare); qsort(dt, frames, sizeof(*dt), compare); CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
    printf("{\"kind\":\"%s\",\"partition_bytes\":%u,\"payload_bytes\":%llu,\"frames\":%u,\"setup_seconds_without_rng\":%.6f,"
           "\"initialized_rss_kib\":%ld,\"final_rss_kib\":%ld,\"peak_rss_kib\":%ld,"
           "\"compress_mbps\":%.3f,\"decompress_mbps\":%.3f,\"compress_us_p50_p95_p99\":[%.3f,%.3f,%.3f],"
           "\"decompress_us_p50_p95_p99\":[%.3f,%.3f,%.3f],\"random_ratio\":%.6f,\"dictionary_mix_ratio\":%.6f",
           kind, cap, (unsigned long long)cap * 12, frames, setup, initialized_rss, rss(), usage.ru_maxrss,
           frames * FRAME * 8 / csum, frames * FRAME * 8 / dsum, ct[frames/2], ct[frames*95/100], ct[frames*99/100],
           dt[frames/2], dt[frames*95/100], dt[frames*99/100], (double)bytes[0]/(cases[0]*FRAME), (double)bytes[1]/(cases[1]*FRAME));
#ifdef GD_BASELINE
    printf(",\"cdict_bytes\":%zu,\"ddict_bytes\":%zu,\"window_log\":%u,\"hash_log\":%u,\"chain_log\":%u", ZSTD_sizeof_CDict(cd), ZSTD_sizeof_DDict(dd), params.windowLog, params.hashLog, params.chainLog);
    ZSTD_freeCDict(cd); ZSTD_freeDDict(dd); free(tx); free(rx);
#else
    printf(",\"block_bytes\":%u,\"index_bytes\":%llu,\"metadata_bytes\":%llu,\"payload_allocated\":%llu,\"payload_relocated\":%llu,\"matches_by_tier\":[%llu,%llu,%llu]", GD_BLOCK_SIZE,
           (unsigned long long)GD_stats(tx)->index_allocated,
           (unsigned long long)(GD_stats(tx)->metadata_allocated + GD_stats(rx)->metadata_allocated),
           (unsigned long long)(GD_stats(tx)->payload_allocated + GD_stats(rx)->payload_allocated),
           (unsigned long long)(GD_stats(tx)->payload_relocated + GD_stats(rx)->payload_relocated),
           (unsigned long long)GD_stats(tx)->matches[0], (unsigned long long)GD_stats(tx)->matches[1], (unsigned long long)GD_stats(tx)->matches[2]);
    GD_free(tx); GD_free(rx);
#endif
    puts("}"); free(ct); free(dt); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); return 0;
}
