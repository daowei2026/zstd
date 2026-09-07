/* Workload experiment, not a product admission or wire contract. BSD license. */
#include "dictionary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CAP (32 * GD_BLOCK_SIZE)
#define FRAME 1514
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static GD_Store *tx, *rx;
static uint64_t rotations[3], maintenance, copies, repairs, lost, delivered;
static uint64_t rng = 0x142637812ULL;
static unsigned random32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }
static void bytes(void* p, size_t n) { size_t i; for (i = 0; i < n; ++i) ((unsigned char*)p)[i] = (unsigned char)random32(); }
static void rotate(unsigned tier)
{
    unsigned source = GD_committed(tx, tier), dest, b;
    uint64_t epoch = GD_epoch(tx, source);
    GD_Move moves[8]; size_t n = 0;
    uint32_t offset;
    /* Retain at most one quarter of the retiring partition, so turnover always
     * leaves room for new patterns. Rank by logical-frame hits, never copies. */
    for (b = 0; b < 32; ++b) {
        uint64_t hits = GD_blockHits(tx, source, b); size_t at = n, j;
        if (hits < 2) continue;
        if (at == 8) { if (hits <= GD_blockHits(tx, source, moves[7].source_block)) continue; at = 7; }
        while (at && hits > GD_blockHits(tx, source, moves[at-1].source_block)) --at;
        for (j = n < 8 ? n : 7; j > at; --j) moves[j] = moves[j-1];
        moves[at].source_block = b;
        if (n < 8) ++n;
    }
    dest = GD_prepare(tx, tier ? tier - 1 : 0);
    offset = (GD_extent(tx, dest) + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE;
    /* Keep P's second half available for retention from its old committed. */
    if (tier && offset + n * GD_BLOCK_SIZE > (tier == 1 ? CAP / 2 : CAP)) {
        rotate(tier - 1); dest = GD_prepare(tx, tier - 1);
        offset = (GD_extent(tx, dest) + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE;
    }
    if (!tier && n > (CAP - offset) / GD_BLOCK_SIZE) n = (CAP - offset) / GD_BLOCK_SIZE;
    for (b = 0; b < n; ++b) moves[b].destination_offset = offset + b * GD_BLOCK_SIZE;
    { uint64_t const target_epoch = GD_epoch(tx, dest);
      CHECK(GD_rotate(tx, tier, epoch, dest, target_epoch, moves, n) == GD_OK);
      CHECK(GD_rotate(rx, tier, epoch, dest, target_epoch, moves, n) == GD_OK); }
    maintenance += (32 + n * 8) * 3; ++rotations[tier];
}
static void admit(const unsigned char* frame)
{
    unsigned p = GD_prepare(tx, 2); uint32_t offset;
    size_t at;
    if (GD_extent(tx, p) + FRAME > CAP) { rotate(2); p = GD_prepare(tx, 2); }
    CHECK(GD_append(tx, 2, frame, FRAME, &offset) == GD_OK);
    for (at = 0; at < FRAME; at += 1000) {
        size_t n = FRAME - at; if (n > 1000) n = 1000;
        CHECK(GD_write(rx, p, GD_epoch(rx, p), offset + (uint32_t)at, frame + at, n) == GD_OK);
        maintenance += (n + 32) * 3; /* R=2, first coefficient=1.5, max_r=5. */
    }
}
int main(void)
{
    unsigned char templates[1024][FRAME], frame[FRAME], output[FRAME], compressed[2048];
    unsigned seen[1024] = {0}, phase, i;
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    tx = GD_create(CAP, 1); rx = GD_create(CAP, 0); CHECK(tx && rx && cc && dc);
    bytes(templates, sizeof(templates));
    for (phase = 0; phase < 3; ++phase) {
        uint64_t wire = 0, raw = 0, start_maintenance = maintenance;
        uint64_t start_matches[3]; unsigned tier;
        for (tier = 0; tier < 3; ++tier) start_matches[tier] = GD_stats(tx)->matches[tier];
        for (i = 0; i < 20000; ++i) {
            unsigned id = random32() % 1024, is_random = random32() % 10 == 0;
            GD_FrameView view; size_t size, decoded; unsigned copy;
            /* Mostly persistent hot set, then a phase change, then hot traffic
             * mixed with more churn. This input is intentionally compressible. */
            if (random32() % 10 < (phase == 2 ? 5U : 8U)) id = (phase == 0 ? 0 : 64) + random32() % 16;
            memcpy(frame, templates[id], FRAME);
            if (is_random) bytes(frame, FRAME);
            size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, FRAME, &view);
            CHECK(!ZSTD_isError(size));
            /* Only repeated, unrepresented samples enter adhoc. Hit counts and
             * learning run once per original frame, outside the copy schedule. */
            if (!is_random && !view.used_mask && ++seen[id] % 2 == 0) admit(frame);
            raw += (FRAME + 32) * 2;
            for (copy = 0; copy < 2; ++copy) {
                wire += size + 32; ++copies;
                decoded = GD_decompress(rx, dc, output, sizeof(output), compressed, size, &view);
                if (ZSTD_isError(decoded)) {
                    /* A rotation during admission can invalidate an old view.
                     * Repair only the already planned next copy. */
                    if (!copy) { size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, FRAME, &view); CHECK(!ZSTD_isError(size)); ++repairs; }
                    else ++lost;
                } else { CHECK(decoded == FRAME && memcmp(frame, output, FRAME) == 0); if (!copy) ++delivered; }
            }
        }
        printf("{\"phase\":%u,\"frames\":20000,\"business_bytes\":%llu,\"maintenance_bytes\":%llu,\"wire_over_plain_R2\":%.6f,\"matches_by_tier\":[%llu,%llu,%llu],\"rotations\":[%llu,%llu,%llu]}\n", phase,
            (unsigned long long)wire, (unsigned long long)(maintenance - start_maintenance),
            (double)(wire + maintenance - start_maintenance) / raw,
            (unsigned long long)(GD_stats(tx)->matches[0]-start_matches[0]), (unsigned long long)(GD_stats(tx)->matches[1]-start_matches[1]), (unsigned long long)(GD_stats(tx)->matches[2]-start_matches[2]),
            (unsigned long long)rotations[0], (unsigned long long)rotations[1], (unsigned long long)rotations[2]);
    }
    CHECK(copies == 120000 && lost == 0);
    printf("{\"planned_copies\":120000,\"sent_copies\":%llu,\"rewritten_unsent_copies\":%llu,\"unrecoverable_copies\":%llu,\"payload_written\":%llu,\"payload_transferred\":%llu,\"transferred_referenced_upper\":%llu,\"transferred_cold_lower\":%llu,\"transferred_padding\":%llu,\"payload_relocated\":%llu}\n",
        (unsigned long long)copies, (unsigned long long)repairs, (unsigned long long)lost,
        (unsigned long long)GD_stats(tx)->payload_written, (unsigned long long)GD_stats(tx)->payload_transferred,
        (unsigned long long)GD_stats(tx)->transferred_referenced_upper,
        (unsigned long long)(GD_stats(tx)->payload_transferred - GD_stats(tx)->transferred_referenced_upper),
        (unsigned long long)GD_stats(tx)->transferred_padding, (unsigned long long)GD_stats(tx)->payload_relocated);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); return 0;
}
