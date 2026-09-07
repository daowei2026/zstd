/* Behavioral tests for the fork prototype; repository BSD license. */
#include "dictionary.h"
#include "../../lib/zstd_segmented.h"
#include "../../lib/zstd_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CAP (2 * GD_BLOCK_SIZE)
static uint64_t rng = 0x712d379ae5b60142ULL;
static unsigned random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 16);
}
static void random_bytes(void* dst, size_t size)
{
    size_t i;
    unsigned char* p = (unsigned char*)dst;
    for (i = 0; i < size; ++i) p[i] = (unsigned char)random32();
}
static void write_part(GD_Store* store, unsigned slot, const void* data, size_t size)
{
    CHECK(GD_write(store, slot, GD_epoch(store, slot), 0, data, size) == GD_OK);
}
static void test_append_and_conflict(void)
{
    unsigned char data[CAP], output[CAP], old[1024];
    unsigned char compressed[2048];
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    GD_FrameView view;
    const void* address;
    size_t size, decoded;
    uint32_t offset;
    uint64_t indexed, written;
    CHECK(tx && rx && cctx && dctx);
    random_bytes(data, sizeof(data));
    CHECK(GD_append(tx, 2, data, 4093, &offset) == GD_OK && offset == 0);
    write_part(rx, 5, data, 4093);
    memcpy(old, data + 41, sizeof(old));
    size = GD_compress(tx, cctx, compressed, sizeof(compressed), old, sizeof(old), &view);
    CHECK(!ZSTD_isError(size) && size < sizeof(old));
    address = GD_blockAddress(tx, 5, 0);
    indexed = GD_stats(tx)->indexed_positions;
    written = GD_stats(tx)->payload_written;
    CHECK(GD_append(tx, 2, data + 4093, 99, &offset) == GD_OK && offset == 4093);
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 4093, data + 4093, 99) == GD_OK);
    CHECK(GD_blockAddress(tx, 5, 0) == address);
    CHECK(GD_stats(tx)->payload_written - written == 99);
    CHECK(GD_stats(tx)->indexed_positions - indexed <= 106);
    CHECK(GD_stats(tx)->payload_relocated == 0);
    decoded = GD_decompress(rx, dctx, output, sizeof(output), compressed, size, &view);
    CHECK(decoded == sizeof(old) && memcmp(output, old, sizeof(old)) == 0);
    /* Match starts before the previous append boundary and crosses a page. */
    size = GD_compress(tx, cctx, compressed, sizeof(compressed), data + 4088, 92, &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 5));
    decoded = GD_decompress(rx, dctx, output, sizeof(output), compressed, size, &view);
    CHECK(decoded == 92 && memcmp(output, data + 4088, 92) == 0);
    written = GD_stats(rx)->payload_written;
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 0, data, 4192) == GD_OK);
    CHECK(GD_stats(rx)->payload_written == written);
    memcpy(output, data + 4180, 30);
    output[3] ^= 1;
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 4180, output, 30) == GD_CONFLICT);
    CHECK(GD_stats(rx)->payload_written == written);
    CHECK(GD_read(rx, 5, GD_epoch(rx, 5), 4192, output, 1) == GD_MISSING);
    CHECK(GD_append(tx, 2, data, CAP, &offset) == GD_CAPACITY);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cctx); ZSTD_freeDCtx(dctx);
}

static void test_mixed_holes_and_retirement(void)
{
    unsigned char a[CAP], b[CAP], c[CAP], frame[1600], output[1600], compressed[2048];
    unsigned char saved[2048];
    GD_FrameView view, old_view;
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    size_t length = 0, size, saved_size, decoded;
    uint64_t epoch, allocated;
    const void* retained;
    GD_Move keep = {0, GD_BLOCK_SIZE};
    CHECK(tx && rx && cctx && dctx);
    random_bytes(a, CAP); random_bytes(b, CAP); random_bytes(c, CAP);
    write_part(tx, 1, a, CAP); write_part(tx, 3, b, CAP); write_part(tx, 5, c, CAP);
    write_part(rx, 3, b, CAP); write_part(rx, 5, c, CAP);
    /* Out-of-order growth, leaving one interior hole. */
    CHECK(GD_write(rx, 1, GD_epoch(rx, 1), 200, a + 200, CAP - 200) == GD_OK);
    CHECK(GD_write(rx, 1, GD_epoch(rx, 1), 0, a, 100) == GD_OK);
    memcpy(frame + length, a + 32, 240); length += 240;
    memcpy(frame + length, b + 3900, 400); length += 400;
    memcpy(frame + length, c + 4064, 512); length += 512;
    random_bytes(frame + length, 17); length += 17;
    size = GD_compress(tx, cctx, compressed, sizeof(compressed), frame, length, &view);
    CHECK(!ZSTD_isError(size));
    CHECK(view.used_mask == ((1U << 1) | (1U << 3) | (1U << 5)));
    CHECK(ZSTD_isError(GD_decompress(rx, dctx, output, sizeof(output), compressed, size, &view)));
    CHECK(GD_lastResult(rx) == GD_MISSING);
    CHECK(GD_missing(rx)->partition == 1 && GD_missing(rx)->offset >= 100 && GD_missing(rx)->offset < 200);
    CHECK(GD_write(rx, 1, GD_epoch(rx, 1), 100, a + 100, 100) == GD_OK);
    decoded = GD_decompress(rx, dctx, output, sizeof(output), compressed, size, &view);
    CHECK(decoded == length && memcmp(frame, output, length) == 0);
    memcpy(saved, compressed, size); saved_size = size; old_view = view;

    /* Keep the old payload intact while copying retained bytes into the
     * current prepare's remaining space, then retire the old committed. */
    epoch = GD_epoch(tx, 0);
    CHECK(GD_rotate(tx, 0, epoch, 0, 0, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 0, epoch, 0, 0, NULL, 0) == GD_OK);
    write_part(tx, 0, b, GD_BLOCK_SIZE); write_part(rx, 0, b, GD_BLOCK_SIZE);
    retained = GD_blockAddress(tx, 1, 0);
    allocated = GD_stats(tx)->payload_allocated;
    epoch = GD_epoch(tx, 1);
    CHECK(GD_rotate(tx, 0, epoch, 1, epoch, &keep, 1) == GD_INVALID);
    CHECK(GD_epoch(tx, 1) == epoch && !memcmp(retained, a, GD_BLOCK_SIZE));
    CHECK(GD_rotate(tx, 0, epoch, 0, GD_epoch(tx, 0), &keep, 1) == GD_OK);
    CHECK(GD_rotate(rx, 0, epoch, 0, GD_epoch(rx, 0), &keep, 1) == GD_OK);
    CHECK(GD_blockAddress(tx, 0, 1) != retained);
    CHECK(!memcmp(retained, a, GD_BLOCK_SIZE));
    CHECK(!memcmp(GD_blockAddress(tx, 0, 1), a, GD_BLOCK_SIZE));
    CHECK(GD_blockAddress(tx, 1, 1) == NULL);
    CHECK(GD_stats(tx)->payload_allocated == allocated);
    CHECK(GD_stats(tx)->payload_relocated == GD_BLOCK_SIZE);
    CHECK(GD_rotate(rx, 0, epoch, 1, epoch, &keep, 1) == GD_STALE);
    CHECK(ZSTD_isError(GD_decompress(rx, dctx, output, sizeof(output), saved, saved_size, &old_view)));
    CHECK(GD_lastResult(rx) == GD_STALE);
    CHECK(GD_write(rx, 1, epoch, 0, a, 100) == GD_STALE);
    size = GD_compress(tx, cctx, compressed, sizeof(compressed), frame, length, &view);
    CHECK(!ZSTD_isError(size));
    CHECK(view.used_mask & (1U << 0)); /* Retained a matches perpetual. */
    decoded = GD_decompress(rx, dctx, output, sizeof(output), compressed, size, &view);
    CHECK(decoded == length && memcmp(frame, output, length) == 0);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cctx); ZSTD_freeDCtx(dctx);
}

static void test_promotion_with_missing_block(void)
{
    unsigned char data[CAP], output[GD_BLOCK_SIZE];
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    GD_Move move = {1, 0}, invalid[2] = {{1, 0}, {1, GD_BLOCK_SIZE}};
    uint64_t epoch, allocated, written;
    const void* address;
    CHECK(tx && rx);
    random_bytes(data, CAP);
    write_part(tx, 5, data, CAP);
    write_part(rx, 5, data, GD_BLOCK_SIZE);
    epoch = GD_epoch(tx, 4);
    CHECK(GD_rotate(tx, 2, epoch, 3, 0, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, 3, 0, NULL, 0) == GD_OK);
    epoch = GD_epoch(tx, 5);
    allocated = GD_stats(tx)->payload_allocated;
    written = GD_stats(tx)->payload_written;
    address = GD_blockAddress(tx, 5, 1);
    CHECK(GD_rotate(tx, 2, epoch, 3, GD_epoch(tx, 3) + 6, &move, 1) == GD_STALE);
    CHECK(GD_epoch(tx, 5) == epoch && GD_blockAddress(tx, 5, 1) == address);
    CHECK(GD_rotate(tx, 2, epoch, 3, GD_epoch(tx, 3), invalid, 2) == GD_INVALID);
    CHECK(GD_epoch(tx, 5) == epoch && GD_stats(tx)->payload_allocated == allocated);
    CHECK(GD_stats(tx)->payload_written == written);
    CHECK(GD_read(tx, 3, GD_epoch(tx, 3), 0, output, 1) == GD_MISSING);
    CHECK(GD_rotate(tx, 2, epoch, 3, GD_epoch(tx, 3), &move, 1) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, 3, GD_epoch(tx, 3), &move, 1) == GD_OK);
    CHECK(GD_blockAddress(tx, 3, 0) != address);
    CHECK(!memcmp(address, data + GD_BLOCK_SIZE, GD_BLOCK_SIZE));
    CHECK(GD_blockAddress(tx, 5, 1) == NULL);
    CHECK(GD_stats(tx)->payload_allocated == allocated);
    CHECK(GD_stats(rx)->payload_allocated == GD_PARTITIONS * CAP);
    CHECK(GD_stats(tx)->payload_relocated == GD_BLOCK_SIZE);
    CHECK(GD_read(rx, 3, GD_epoch(rx, 3), 0, output, sizeof(output)) == GD_MISSING);
    CHECK(GD_read(tx, 3, GD_epoch(tx, 3), 0, output, sizeof(output)) == GD_OK);
    CHECK(GD_write(rx, 3, GD_epoch(rx, 3), 0, output, sizeof(output)) == GD_OK);
    CHECK(memcmp(output, data + GD_BLOCK_SIZE, GD_BLOCK_SIZE) == 0);
    GD_free(tx); GD_free(rx);
}

static void test_random_roundtrips(void)
{
    unsigned char dictionaries[3][CAP], frame[1514], output[1514], compressed[2048], damaged[2048];
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    unsigned iteration, tier;
    CHECK(tx && rx && cctx && dctx);
    for (tier = 0; tier < 3; ++tier) {
        random_bytes(dictionaries[tier], CAP);
        write_part(tx, 2 * tier + 1, dictionaries[tier], CAP);
        write_part(rx, 2 * tier + 1, dictionaries[tier], CAP);
    }
    for (iteration = 0; iteration < 3000; ++iteration) {
        size_t length = random32() % (sizeof(frame) + 1), at = 0, size, result;
        GD_FrameView view;
        random_bytes(frame, length);
        while (at + 8 < length) {
            size_t n = 8 + random32() % 240;
            tier = random32() % 3;
            if (n > length - at) n = length - at;
            if (iteration % 3) memcpy(frame + at, dictionaries[tier] + random32() % (CAP - n + 1), n);
            at += n;
        }
        size = GD_compress(tx, cctx, compressed, sizeof(compressed), frame, length, &view);
        CHECK(!ZSTD_isError(size));
        result = GD_decompress(rx, dctx, output, sizeof(output), compressed, size, &view);
        CHECK(result == length && memcmp(frame, output, length) == 0);
        CHECK(ZSTD_isError(GD_decompress(rx, dctx, output, sizeof(output), compressed, size - 1, &view)));
        if (size) {
            memcpy(damaged, compressed, size);
            damaged[random32() % size] ^= (unsigned char)(1U << (random32() & 7));
            result = GD_decompress(rx, dctx, output, sizeof(output), damaged, size, &view);
            CHECK(ZSTD_isError(result) || result <= sizeof(output));
        }
        if (length) CHECK(ZSTD_isError(GD_compress(tx, cctx, compressed, 1, frame, length, &view)));
    }
    /* Ordinary zstd still works after external dictionary errors and reuse. */
    random_bytes(frame, sizeof(frame));
    { size_t size = ZSTD_compressCCtx(cctx, compressed, sizeof(compressed), frame, sizeof(frame), 3);
      CHECK(!ZSTD_isError(size));
      CHECK(ZSTD_decompressDCtx(dctx, output, sizeof(output), compressed, size) == sizeof(frame));
      CHECK(memcmp(frame, output, sizeof(frame)) == 0); }
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cctx); ZSTD_freeDCtx(dctx);
}

static void test_borrowed_continuous_payload(void)
{
    const uint32_t capacities[3] = {CAP + 13, CAP + 7, CAP + 3};
    void* buffers[GD_PARTITIONS];
    unsigned char* generations[3];
    unsigned char data[CAP + 13], output[CAP + 13];
    unsigned tier, slot, mode;
    uint32_t offset;
    GD_Store* store;
    GD_PartitionStats observed;
    uint64_t total = 0;
    random_bytes(data, sizeof(data));
    for (tier = 0; tier < 3; ++tier) {
        generations[tier] = (unsigned char*)malloc(2 * capacities[tier]);
        CHECK(generations[tier]);
        buffers[2 * tier] = generations[tier];
        buffers[2 * tier + 1] = generations[tier] + capacities[tier];
        total += 2 * capacities[tier];
    }
    for (mode = 0; mode < 2; ++mode) {
        for (tier = 0; tier < 3; ++tier) memset(generations[tier], 0xa5, 2 * capacities[tier]);
        store = GD_createWithBuffers(capacities, buffers, mode);
        CHECK(store && GD_stats(store)->payload_allocated == total);
        for (slot = 0; slot < GD_PARTITIONS; ++slot) {
            size_t i;
            for (i = 0; i < capacities[slot / 2]; ++i) CHECK(((unsigned char*)buffers[slot])[i] == 0xa5);
            CHECK(GD_observePartition(store, slot, &observed) == GD_OK && observed.present_bytes == 0);
            CHECK(GD_read(store, slot, GD_epoch(store, slot), 0, output, 1) == GD_MISSING);
        }
        /* Appends cross metadata regions and an unaligned final capacity. */
        if (mode) {
            CHECK(GD_append(store, 0, data, GD_BLOCK_SIZE - 3, &offset) == GD_OK);
            CHECK(GD_append(store, 0, data + GD_BLOCK_SIZE - 3,
                sizeof(data) - GD_BLOCK_SIZE + 3, &offset) == GD_OK);
        } else write_part(store, 1, data, sizeof(data));
        CHECK(!memcmp(buffers[1], data, sizeof(data)));
        CHECK(GD_read(store, 1, GD_epoch(store, 1), 0, output, sizeof(output)) == GD_OK && !memcmp(output, data, sizeof(data)));
        CHECK((const unsigned char*)GD_blockAddress(store, 1, 1) == (const unsigned char*)buffers[1] + GD_BLOCK_SIZE);
        CHECK(GD_rotate(store, 0, GD_epoch(store, 0), 0, 0, NULL, 0) == GD_OK);
        CHECK(GD_rotate(store, 0, GD_epoch(store, 1), 0, 0, NULL, 0) == GD_OK);
        CHECK(GD_read(store, 1, GD_epoch(store, 1), 0, output, 1) == GD_MISSING);
        CHECK(!memcmp(buffers[1], data, sizeof(data)));
        GD_free(store);
        CHECK(!memcmp(buffers[1], data, sizeof(data)));
    }
    /* Adjacent half ranges are legal; overlapping ownership is rejected. */
    buffers[1] = generations[0] + capacities[0] - 1;
    CHECK(!GD_createWithBuffers(capacities, buffers, 1));
    buffers[1] = NULL;
    CHECK(!GD_createWithBuffers(capacities, buffers, 0));
    for (tier = 0; tier < 3; ++tier) free(generations[tier]);
}

static void test_partial_source_copy_and_destination_repair(void)
{
    unsigned char data[GD_BLOCK_SIZE], output[GD_BLOCK_SIZE];
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    GD_Move move = {0, GD_BLOCK_SIZE};
    uint64_t epoch, writes;
    GD_PartitionStats observed;
    CHECK(tx && rx);
    random_bytes(data, sizeof(data));
    write_part(tx, 5, data, sizeof(data));
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 100, data + 100, 60) == GD_OK);
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 900, data + 900, 80) == GD_OK);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, 4), 0, 0, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 2, GD_epoch(rx, 4), 0, 0, NULL, 0) == GD_OK);
    epoch = GD_epoch(tx, 5); writes = GD_stats(rx)->payload_written;
    CHECK(GD_rotate(tx, 2, epoch, 3, GD_epoch(tx, 3), &move, 1) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, 3, GD_epoch(rx, 3), &move, 1) == GD_OK);
    CHECK(GD_stats(rx)->payload_written == writes + 140);
    CHECK(GD_observePartition(rx, 3, &observed) == GD_OK && observed.present_bytes == 140 && observed.extent == CAP);
    CHECK(GD_read(rx, 3, GD_epoch(rx, 3), GD_BLOCK_SIZE + 100, output, 60) == GD_OK && !memcmp(output, data + 100, 60));
    CHECK(GD_read(rx, 3, GD_epoch(rx, 3), GD_BLOCK_SIZE + 160, output, 800) == GD_MISSING);
    CHECK(GD_missing(rx)->offset == GD_BLOCK_SIZE + 160 && GD_missing(rx)->length == 740);
    CHECK(GD_read(tx, 5, epoch, 0, output, 1) == GD_STALE);
    CHECK(GD_read(tx, 3, GD_epoch(tx, 3), GD_BLOCK_SIZE, output, sizeof(output)) == GD_OK);
    CHECK(GD_write(rx, 3, GD_epoch(rx, 3), GD_BLOCK_SIZE, output, sizeof(output)) == GD_OK);
    CHECK(GD_read(rx, 3, GD_epoch(rx, 3), GD_BLOCK_SIZE, output, sizeof(output)) == GD_OK && !memcmp(output, data, sizeof(data)));
    CHECK(GD_read(rx, 3, GD_epoch(rx, 3), 0, output, 1) == GD_MISSING);
    CHECK(GD_observePartition(rx, 3, &observed) == GD_OK && observed.present_bytes == sizeof(data));
    GD_free(tx); GD_free(rx);
}

static void test_standard_bitstream_and_bounds(void)
{
    unsigned char flat[6 * CAP], frame[1514], output[1514], compressed[2048];
    GD_Store* tx = GD_create(CAP, 1);
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    ZSTD_DDict* dd;
    GD_FrameView view;
    ZSTD_Sequence sequence[2];
    size_t size;
    unsigned p;
    CHECK(tx && cc && dc);
    random_bytes(flat, sizeof(flat));
    for (p = 0; p < 6; ++p) write_part(tx, p, flat + p * CAP, CAP);
    for (p = 0; p < 3; ++p) memcpy(frame + p * 480, flat + (2 * p + 1) * CAP - 480, 480);
    random_bytes(frame + 1440, sizeof(frame) - 1440);
    size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == 0x15);
    /* Flattening is a test oracle only: an unmodified ordinary decoder can
     * consume the fork's standard bitstream when given the same address space. */
    dd = ZSTD_createDDict_advanced(flat, sizeof(flat), ZSTD_dlm_byRef, ZSTD_dct_rawContent, ZSTD_defaultCMem);
    CHECK(dd && ZSTD_decompress_usingDDict(dc, output, sizeof(output), compressed, size, dd) == sizeof(frame));
    CHECK(memcmp(frame, output, sizeof(frame)) == 0);
    memset(sequence, 0, sizeof(sequence));
    sequence[0].matchLength = 16; sequence[0].offset = sizeof(flat) + 1;
    CHECK(!ZSTD_isError(ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters)));
    CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed), sequence, 2, frame, 16, sizeof(flat), 0)));
    CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed), sequence, 2, frame, 65536, sizeof(flat), 0)));
    CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed), NULL, 1, frame, 16, sizeof(flat), 0)));
    /* No-dictionary fallback retains intra-frame repetition and overlap. */
    memset(frame, 'a', sizeof(frame));
    size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && size < 30 && view.used_mask == 0);
    CHECK(GD_decompress(tx, dc, output, sizeof(output), compressed, size, &view) == sizeof(frame));
    CHECK(memcmp(frame, output, sizeof(frame)) == 0);
    ZSTD_freeDDict(dd); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx);
}

typedef struct {
    size_t offsets[3];
    unsigned char bytes[3][128];
    unsigned calls;
} SparseHistory;

static size_t read_sparse_history(void* opaque, size_t offset, void* dst, size_t length)
{
    SparseHistory* history = (SparseHistory*)opaque;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        if (offset >= history->offsets[i] && offset - history->offsets[i] <= sizeof(history->bytes[i]) &&
            length <= sizeof(history->bytes[i]) - (offset - history->offsets[i])) {
            memcpy(dst, history->bytes[i] + offset - history->offsets[i], length);
            ++history->calls;
            return 0;
        }
    }
    return (size_t)-1;
}

static void test_large_external_history(void)
{
    /* Exercise real sequence offsets without allocating or reading GiB of
     * synthetic payload. The callback admits only these three exact ranges. */
    size_t const maximum = (size_t)UINT32_MAX - 65535 - 3;
    size_t const sizes[] = {(size_t)1 << 30, ((size_t)1 << 30) + 1, 2500000000U, maximum};
    SparseHistory history;
    unsigned char frame[3 * 128], output[sizeof(frame)], compressed[1024];
    ZSTD_Sequence seq[4];
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    size_t n, size = 0;
    unsigned i;
    CHECK(cc && dc);
    random_bytes(history.bytes, sizeof(history.bytes));
    memcpy(frame, history.bytes, sizeof(frame));
    for (n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n) {
        size_t const dict = sizes[n];
        memset(seq, 0, sizeof(seq));
        history.calls = 0;
        history.offsets[0] = 0;
        history.offsets[1] = dict / 2;
        history.offsets[2] = dict - 128;
        for (i = 0; i < 3; ++i) {
            seq[i].matchLength = 128;
            seq[i].offset = (unsigned)(dict + i * 128 - history.offsets[i]);
        }
        CHECK(!ZSTD_isError(ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters)));
        CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cc, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters)));
        size = ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed),
            seq, 4, frame, sizeof(frame), dict, 0);
        if (ZSTD_isError(size)) fprintf(stderr, "external history %zu: %s\n", dict, ZSTD_getErrorName(size));
        CHECK(!ZSTD_isError(size) && size < sizeof(frame));
        CHECK(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), compressed, size,
            dict, 0, read_sparse_history, &history) == sizeof(frame));
        CHECK(history.calls == 3 && !memcmp(frame, output, sizeof(frame)));
    }
    {
        unsigned char* large = (unsigned char*)malloc(65535);
        unsigned char* decoded = (unsigned char*)malloc(65535);
        unsigned char* encoded = (unsigned char*)malloc(ZSTD_compressBound(65535));
        size_t encoded_size;
        CHECK(large && decoded && encoded);
        memset(large, 'q', 65535 - 128);
        memcpy(large + 65535 - 128, history.bytes[0], 128);
        memset(seq, 0, sizeof(seq));
        seq[0].litLength = 65535 - 128;
        seq[0].matchLength = 128;
        seq[0].offset = (unsigned)(maximum + seq[0].litLength);
        history.calls = 0;
        CHECK(!ZSTD_isError(ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters)));
        CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cc, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters)));
        encoded_size = ZSTD_compressSequencesWithExternalDictSize(cc, encoded, ZSTD_compressBound(65535),
            seq, 2, large, 65535, maximum, 0);
        CHECK(!ZSTD_isError(encoded_size));
        CHECK(ZSTD_decompressWithExternalDict(dc, decoded, 65535, encoded, encoded_size,
            maximum, 0, read_sparse_history, &history) == 65535);
        CHECK(history.calls == 1 && !memcmp(large, decoded, 65535));
        seq[0].offset = UINT32_MAX;
        CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, encoded, ZSTD_compressBound(65535),
            seq, 2, large, 65535, maximum, 0)));
        free(large); free(decoded); free(encoded);
    }
    memset(seq, 0, sizeof(seq));
    seq[0].matchLength = 128; seq[0].offset = 128;
    seq[1].litLength = sizeof(frame) - 128;
    CHECK(ZSTD_getErrorCode(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed),
        seq, 2, frame, sizeof(frame), maximum + 1, 0)) == ZSTD_error_parameter_outOfBound);
    CHECK(ZSTD_getErrorCode(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), compressed, size,
        maximum + 1, 0, read_sparse_history, &history)) == ZSTD_error_parameter_outOfBound);
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
}

typedef struct {
    unsigned char bytes[1024];
    size_t extent;
    unsigned calls;
    int missing;
} LocalDictionary;

static size_t read_local_dictionary(void* opaque, size_t offset, void* dst, size_t length)
{
    LocalDictionary* dict = (LocalDictionary*)opaque;
    ++dict->calls;
    if (dict->missing || offset > dict->extent || length > dict->extent - offset)
        return (size_t)0 - ZSTD_error_dictionary_wrong;
    memcpy(dst, dict->bytes + offset, length);
    return 0;
}

static void test_identified_independent_frames(void)
{
    uint64_t const saved_rng = rng;
    size_t const capacities[] = {250000000, 200000000};
    unsigned const ids[] = {32768, 32769};
    size_t const lengths[] = {311, 197};
    LocalDictionary dictionaries[2];
    ZSTD_CCtx* cc[2] = {ZSTD_createCCtx(), ZSTD_createCCtx()};
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char joined[1024], output[1024], raw[64], raw_frame[128];
    size_t sizes[2], offsets[2], total = 0, raw_size, i;
    CHECK(cc[0] && cc[1] && dc);
    /* This case must not change the established randomized overlap fixtures. */
    rng = 0x4c8dab0297501e63ULL;
    memset(dictionaries, 0, sizeof(dictionaries));
    {
        /* Standard zstd frame: ID=32768, content size=3, one final raw block.
         * This fixed decoder vector is independent of the fork encoder. */
        static const unsigned char vector[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x22, 0x00, 0x80, 0x03,
            0x19, 0x00, 0x00, 0x61, 0x62, 0x63
        };
        CHECK(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), vector, sizeof(vector),
            capacities[0], ids[0], read_local_dictionary, &dictionaries[0]) == 3);
        CHECK(!memcmp(output, "abc", 3) && dictionaries[0].calls == 0);
        CHECK(ZSTD_getErrorCode(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), vector, sizeof(vector),
            capacities[0], 0, read_local_dictionary, &dictionaries[0])) == ZSTD_error_dictionary_wrong);
        CHECK(dictionaries[0].calls == 0);
    }
    for (i = 0; i < 2; ++i) {
        ZSTD_Sequence seq[2] = {{0}, {0}};
        random_bytes(dictionaries[i].bytes, sizeof(dictionaries[i].bytes));
        dictionaries[i].extent = 700;
        seq[0].offset = (unsigned)(capacities[i] - 101);
        seq[0].matchLength = (unsigned)lengths[i];
        CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cc[i], ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters)));
        offsets[i] = total;
        sizes[i] = ZSTD_compressSequencesWithExternalDictSize(cc[i], joined + total, sizeof(joined) - total,
            seq, 2, dictionaries[i].bytes + 101, lengths[i], capacities[i], ids[i]);
        CHECK(!ZSTD_isError(sizes[i]) && sizes[i] < lengths[i]);
        CHECK(ZSTD_getDictID_fromFrame(joined + total, sizes[i]) == ids[i]);
        CHECK(ZSTD_getFrameContentSize(joined + total, sizes[i]) == lengths[i]);
        total += sizes[i];
    }
    CHECK(ZSTD_findFrameCompressedSize(joined, total) == sizes[0]);
    CHECK(ZSTD_findFrameCompressedSize(joined + sizes[0], total - sizes[0]) == sizes[1]);
    CHECK(ZSTD_findDecompressedSize(joined, total) == lengths[0] + lengths[1]);
    /* Ordinary frame discovery supplies the boundaries; no private segment header. */
    for (i = 2; i > 0; --i) {
        size_t const p = i - 1;
        unsigned calls = dictionaries[p].calls;
        CHECK(ZSTD_getErrorCode(ZSTD_decompressWithExternalDict(dc, output, sizeof(output),
            joined + offsets[p], sizes[p], capacities[p], ids[p] + 100,
            read_local_dictionary, &dictionaries[p])) == ZSTD_error_dictionary_wrong);
        CHECK(dictionaries[p].calls == calls);
        CHECK(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), joined + offsets[p], sizes[p],
            capacities[p], ids[p], read_local_dictionary, &dictionaries[p]) == lengths[p]);
        CHECK(!memcmp(output, dictionaries[p].bytes + 101, lengths[p]));
        dictionaries[p].extent += 123;
        CHECK(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), joined + offsets[p], sizes[p],
            capacities[p], ids[p], read_local_dictionary, &dictionaries[p]) == lengths[p]);
        CHECK(!memcmp(output, dictionaries[p].bytes + 101, lengths[p]));
        dictionaries[p].missing = 1;
        CHECK(ZSTD_isError(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), joined + offsets[p], sizes[p],
            capacities[p], ids[p], read_local_dictionary, &dictionaries[p])));
        dictionaries[p].missing = 0;
        CHECK(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), joined + offsets[p], sizes[p],
            capacities[p], ids[p], read_local_dictionary, &dictionaries[p]) == lengths[p]);
        CHECK(!memcmp(output, dictionaries[p].bytes + 101, lengths[p]));
    }
    CHECK(ZSTD_isError(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), joined, total,
        capacities[0], ids[0], read_local_dictionary, &dictionaries[0])));
    CHECK(ZSTD_isError(ZSTD_decompressWithExternalDict(dc, output, sizeof(output), joined, sizes[0] - 1,
        capacities[0], ids[0], read_local_dictionary, &dictionaries[0])));
    /* A previous identified read must not affect a later ordinary frame. */
    random_bytes(raw, sizeof(raw));
    raw_size = ZSTD_compress(raw_frame, sizeof(raw_frame), raw, sizeof(raw), 3);
    CHECK(!ZSTD_isError(raw_size));
    CHECK(ZSTD_decompressDCtx(dc, output, sizeof(output), raw_frame, raw_size) == sizeof(raw));
    CHECK(!memcmp(raw, output, sizeof(raw)));
    {
        ZSTD_Sequence seq[2] = {{0}, {0}};
        seq[0].matchLength = 64; seq[0].offset = (unsigned)capacities[0];
        CHECK(!ZSTD_isError(ZSTD_CCtx_reset(cc[0], ZSTD_reset_session_and_parameters)));
        CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cc[0], ZSTD_c_dictIDFlag, 0)));
        CHECK(ZSTD_getErrorCode(ZSTD_compressSequencesWithExternalDictSize(cc[0], joined, sizeof(joined),
            seq, 2, dictionaries[0].bytes, 64, capacities[0], ids[0])) == ZSTD_error_parameter_combination_unsupported);
    }
    ZSTD_freeCCtx(cc[0]); ZSTD_freeCCtx(cc[1]); ZSTD_freeDCtx(dc);
    rng = saved_rng;
}

static void test_learning_after_initial_loss(void)
{
    GD_Store *tx = GD_create(65536, 1), *rx = GD_create(65536, 0);
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char pattern[1400], frame[1514], admitted[1514], output[1514], compressed[2048];
    unsigned phase, learned = 0, initial_lost = 0, settled = 0;
    CHECK(tx && rx && cc && dc);
    for (phase = 0; phase < 3; ++phase) {
        unsigned step, slot = GD_prepare(tx, 2); uint32_t offset = 0;
        uint64_t written = GD_stats(tx)->payload_written;
        random_bytes(pattern, sizeof(pattern));
        for (step = 0; step < 32; ++step) {
            GD_FrameView view; size_t size, decoded;
            memcpy(frame, pattern, sizeof(pattern)); random_bytes(frame + 1400, 114);
            /* The initial maintenance was sent, but lost/delayed in this fixture.
             * A maintenance retry arrives after five original frames. */
            if (step == 5) CHECK(GD_write(rx, slot, GD_epoch(tx, slot), offset, admitted, sizeof(admitted)) == GD_OK);
            size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
            CHECK(!ZSTD_isError(size));
            if (step == 0) {
                CHECK(view.used_mask == 0 && size + 32 > 1200);
                /* Learning commits from the sender's original sample even though
                 * this original frame's business delivery has failed. */
                memcpy(admitted, frame, sizeof(admitted));
                CHECK(GD_append(tx, 2, admitted, sizeof(admitted), &offset) == GD_OK);
                ++learned; ++initial_lost; continue;
            }
            CHECK(view.used_mask && size + 32 <= 1200);
            decoded = GD_decompress(rx, dc, output, sizeof(output), compressed, size, &view);
            if (step < 5) { CHECK(ZSTD_isError(decoded) && GD_lastResult(rx) == GD_MISSING); ++initial_lost; }
            else { CHECK(decoded == sizeof(frame) && memcmp(frame, output, sizeof(frame)) == 0); ++settled; }
        }
        CHECK(GD_stats(tx)->payload_written - written == sizeof(admitted));
    }
    CHECK(learned == 3 && initial_lost == 15 && settled == 81);
    printf("{\"unseen_pattern_phases\":3,\"initial_failed_frames\":15,\"settled_delivered_frames\":81,\"learned_samples\":3,\"payload_written\":%llu,\"payload_relocated\":%llu}\n",
        (unsigned long long)GD_stats(tx)->payload_written, (unsigned long long)GD_stats(tx)->payload_relocated);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
}

static void test_tier_capacities_and_observation(void)
{
    uint32_t capacities[3] = {5 * GD_BLOCK_SIZE + 13, 3 * GD_BLOCK_SIZE + 7, 2 * GD_BLOCK_SIZE + 9};
    GD_Store *tx = GD_createWithCapacities(capacities, 1), *rx = GD_createWithCapacities(capacities, 0);
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char data[5 * GD_BLOCK_SIZE + 13], frame[3 * 256], output[3 * 256], compressed[2048];
    GD_FrameView view; GD_PartitionStats snapshot;
    unsigned tier, source, destination; uint32_t offset; uint64_t epoch, target;
    size_t size; const void* address; GD_Move move;
    CHECK(tx && rx && cc && dc && GD_capacity(tx) == capacities[0]);
    for (tier = 0; tier < 3; ++tier) {
        unsigned p = GD_prepare(tx, tier);
        random_bytes(data, capacities[tier]);
        /* Use the most recently indexed suffix for this counter fixture.
         * The fixed-width index is allowed to evict older candidates. */
        memcpy(frame + tier * 256, data + capacities[tier] - 256, 256);
        CHECK(GD_append(tx, tier, data, capacities[tier], &offset) == GD_OK && offset == 0);
        CHECK(GD_write(rx, p, GD_epoch(tx, p), 0, data, capacities[tier]) == GD_OK);
        CHECK(GD_append(tx, tier, data, 1, &offset) == GD_CAPACITY);
        CHECK(GD_partitionCapacity(tx, p) == capacities[tier]);
        CHECK(GD_observePartition(rx, p, &snapshot) == GD_OK);
        CHECK(snapshot.extent == capacities[tier] && snapshot.present_bytes == capacities[tier]);
        CHECK(snapshot.payload_allocated == capacities[tier]);
        CHECK(GD_read(rx, p, GD_epoch(rx, p), capacities[tier], output, 1) == GD_INVALID);
    }
    size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == ((1U << 1) | (1U << 3) | (1U << 5)));
    CHECK(GD_decompress(rx, dc, output, sizeof(output), compressed, size, &view) == sizeof(frame));
    CHECK(memcmp(frame, output, sizeof(frame)) == 0);
    for (tier = 0; tier < 3; ++tier) CHECK(GD_stats(tx)->matched_bytes[tier] == 256);
    /* Rotate empty committed slots before promoting into a differently sized
     * older partition. A rejected promotion must leave both payloads intact. */
    for (tier = 1; tier < 3; ++tier) {
        source = GD_committed(tx, tier); epoch = GD_epoch(tx, source);
        CHECK(GD_rotate(tx, tier, epoch, 0, 0, NULL, 0) == GD_OK);
        CHECK(GD_rotate(rx, tier, epoch, 0, 0, NULL, 0) == GD_OK);
    }
    source = GD_committed(tx, 2); destination = GD_prepare(tx, 1);
    epoch = GD_epoch(tx, source); target = GD_epoch(tx, destination);
    address = GD_blockAddress(tx, source, 0);
    move.source_block = 0; move.destination_offset = 3 * GD_BLOCK_SIZE;
    CHECK(GD_rotate(tx, 2, epoch, destination, target, &move, 1) == GD_CAPACITY);
    CHECK(GD_epoch(tx, source) == epoch && GD_blockAddress(tx, source, 0) == address);
    move.destination_offset = 0;
    CHECK(GD_rotate(tx, 2, epoch, destination, target, &move, 1) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, destination, target, &move, 1) == GD_OK);
    CHECK(GD_blockAddress(tx, destination, 0) != address);
    CHECK(!memcmp(address, data, GD_BLOCK_SIZE));
    CHECK(GD_observePartition(tx, source, &snapshot) == GD_OK && snapshot.present_bytes == 0 && snapshot.payload_allocated == capacities[2]);
    CHECK(GD_observePartition(tx, destination, &snapshot) == GD_OK && snapshot.present_bytes == GD_BLOCK_SIZE);
    CHECK(GD_read(rx, source, epoch, 0, output, 1) == GD_STALE);
    CHECK(GD_read(rx, destination, target, 0, output, 256) == GD_OK && !memcmp(output, data, 256));
    CHECK(GD_stats(tx)->payload_relocated == GD_BLOCK_SIZE);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
}

static void test_reencoding_does_not_amplify_retention(void)
{
    unsigned char data[512], first[1024], again[1024], output[256];
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    GD_FrameView view;
    GD_PartitionStats observed;
    uint32_t offset;
    unsigned p, i;
    size_t size, next;
    CHECK(tx && rx && cc && dc);
    random_bytes(data, sizeof(data));
    p = GD_prepare(tx, 2);
    CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK);
    write_part(rx, p, data, sizeof(data));
    size = GD_compressTracked(tx, cc, first, sizeof(first), data + 256, 256, &view, 0);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << p));
    CHECK(GD_blockHits(tx, p, 0) == 0);
    CHECK(GD_observePartition(tx, p, &observed) == GD_OK && observed.referenced_upper == 0);
    next = GD_compressTracked(tx, cc, again, sizeof(again), data + 256, 256, &view, 1);
    CHECK(next == size && !memcmp(first, again, size));
    CHECK(GD_blockHits(tx, p, 0) == 1);
    for (i = 0; i < 32; ++i) {
        next = GD_compressTracked(tx, cc, again, sizeof(again), data + 256, 256, &view, 0);
        CHECK(next == size && !memcmp(first, again, size));
    }
    CHECK(GD_blockHits(tx, p, 0) == 1);
    CHECK(GD_stats(tx)->matched_bytes[2] == 34 * 256);
    CHECK(GD_observePartition(tx, p, &observed) == GD_OK && observed.referenced_upper == 256);
    CHECK(GD_decompress(rx, dc, output, sizeof(output), first, size, &view) == 256);
    CHECK(!memcmp(output, data + 256, 256));
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
}

static void test_retention_by_bytes_and_continuity(void)
{
    unsigned char data[4 * GD_BLOCK_SIZE], coded[2 * GD_BLOCK_SIZE];
    GD_Store* tx = GD_create(sizeof(data), 1);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    GD_FrameView view;
    GD_Move moves[2];
    uint32_t offset;
    unsigned i, p, dst;
    size_t n;
    CHECK(tx && cc);
    random_bytes(data, sizeof(data));
    CHECK(GD_append(tx, 2, data, 2 * GD_BLOCK_SIZE, &offset) == GD_OK);
    p = GD_committed(tx, 2); dst = GD_prepare(tx, 1);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, p), dst, GD_epoch(tx, dst), NULL, 0) == GD_OK);
    for (i = 0; i < 10; ++i) {
        n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 0);
        CHECK(!ZSTD_isError(n));
    }
    CHECK(GD_selectMoves(tx, 2, 0, moves, 1) == 0);
    for (i = 0; i < 10; ++i) {
        n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 1);
        CHECK(!ZSTD_isError(n));
    }
    for (i = 0; i < 100; ++i) {
        n = GD_compressTracked(tx, cc, coded, sizeof(coded), data + GD_BLOCK_SIZE, 64, &view, 1);
        CHECK(!ZSTD_isError(n));
    }
    CHECK(GD_selectMoves(tx, 2, 0, moves, 1) == 1 && moves[0].source_block == 0);
    p = GD_committed(tx, 2);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, p), dst, GD_epoch(tx, dst), moves, 1) == GD_OK);
    p = GD_committed(tx, 1); dst = GD_prepare(tx, 0);
    CHECK(GD_rotate(tx, 1, GD_epoch(tx, p), dst, GD_epoch(tx, dst), NULL, 0) == GD_OK);
    CHECK(GD_selectMoves(tx, 1, 0, moves, 1) == 1); /* Retention inherits real reuse. */
    CHECK(GD_blockHits(tx, GD_committed(tx, 1), 0) == 10);
    GD_free(tx);

    tx = GD_create(sizeof(data), 1);
    CHECK(tx);
    CHECK(GD_append(tx, 0, data, sizeof(data), &offset) == GD_OK);
    p = GD_committed(tx, 0);
    CHECK(GD_rotate(tx, 0, GD_epoch(tx, p), p, GD_epoch(tx, p), NULL, 0) == GD_OK);
    n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, 64, &view, 1);
    CHECK(!ZSTD_isError(n));
    n = GD_compressTracked(tx, cc, coded, sizeof(coded), data + 3 * GD_BLOCK_SIZE - 64, 128, &view, 1);
    CHECK(!ZSTD_isError(n));
    CHECK(GD_selectMoves(tx, 0, 0, moves, 2) == 2);
    CHECK(moves[0].source_block != moves[1].source_block && moves[0].source_block + moves[1].source_block == 5);
    GD_free(tx); ZSTD_freeCCtx(cc);
}

static void test_compaction_preserves_source_and_reserves_actual_bytes(void)
{
    unsigned reverse, fits;
    for (reverse = 0; reverse < 2; ++reverse) for (fits = 0; fits < 2; ++fits) {
        unsigned char data[GD_BLOCK_SIZE + 128] = {0}, padding[4 * GD_BLOCK_SIZE] = {0};
        unsigned char united[192], coded[256], readback[sizeof(data)];
        GD_Store* tx = GD_create(sizeof(padding), 1);
        ZSTD_CCtx* cc = ZSTD_createCCtx();
        GD_FrameView view;
        GD_Move moves[2] = {{0, 0}, {1, GD_BLOCK_SIZE}};
        GD_Missing appended;
        uint32_t offset, required;
        uint64_t epoch, written;
        unsigned source, destination, i;
        size_t count = 2, n;
        CHECK(tx && cc);
        random_bytes(united, sizeof(united));
        memcpy(data, united + (reverse ? 64 : 0), 128);
        memcpy(data + GD_BLOCK_SIZE, united + (reverse ? 0 : 64), 128);
        CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK);
        source = GD_committed(tx, 2); destination = GD_prepare(tx, 1);
        CHECK(GD_rotate(tx, 2, GD_epoch(tx, source), destination, GD_epoch(tx, destination), NULL, 0) == GD_OK);
        source = GD_committed(tx, 2); epoch = GD_epoch(tx, source);
        for (i = 0; i < 2; ++i) {
            n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, 128, &view, 1);
            CHECK(!ZSTD_isError(n));
            n = GD_compressTracked(tx, cc, coded, sizeof(coded), data + GD_BLOCK_SIZE, 128, &view, 1);
            CHECK(!ZSTD_isError(n));
        }
        CHECK(GD_append(tx, 1, padding, sizeof(padding) - sizeof(united) + !fits, &offset) == GD_OK);
        written = GD_stats(tx)->payload_written;
        if (!fits) {
            CHECK(GD_compactMoves(tx, 2, epoch, destination, GD_epoch(tx, destination), moves, &count, &appended, &required) == GD_CAPACITY);
            CHECK(required == sizeof(united) && count == 2 && !appended.length);
            CHECK(GD_stats(tx)->payload_written == written && moves[1].source_block == 1);
            i = GD_committed(tx, 1);
            CHECK(GD_rotate(tx, 1, GD_epoch(tx, i), GD_prepare(tx, 0), GD_epoch(tx, GD_prepare(tx, 0)), NULL, 0) == GD_OK);
            destination = GD_prepare(tx, 1);
        }
        CHECK(GD_compactMoves(tx, 2, epoch, destination, GD_epoch(tx, destination), moves, &count, &appended, &required) == GD_OK);
        CHECK(count == 0 && required == sizeof(united) && appended.length == sizeof(united));
        CHECK(GD_stats(tx)->payload_written == written + sizeof(united));
        CHECK(GD_stats(tx)->payload_relocated == sizeof(united));
        CHECK(GD_read(tx, source, epoch, 0, readback, sizeof(data)) == GD_OK && !memcmp(readback, data, sizeof(data)));
        CHECK(GD_read(tx, destination, appended.epoch, appended.offset, readback, appended.length) == GD_OK && !memcmp(readback, united, sizeof(united)));
        CHECK(GD_rotate(tx, 2, epoch, destination, GD_epoch(tx, destination), moves, count) == GD_OK);
        CHECK(GD_read(tx, source, epoch, 0, readback, 1) == GD_STALE);
        CHECK(GD_stats(tx)->payload_peak_allocated == GD_stats(tx)->payload_allocated);
        GD_free(tx); ZSTD_freeCCtx(cc);
    }
}

static void* encode_on_small_thread_stack(void* unused)
{
    GD_Store *tx = GD_create(CAP, 1), *rx = GD_create(CAP, 0);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char *data = (unsigned char*)malloc(GD_MAX_FRAME);
    unsigned char *coded = (unsigned char*)malloc(ZSTD_compressBound(GD_MAX_FRAME));
    unsigned char *output = (unsigned char*)malloc(GD_MAX_FRAME);
    GD_FrameView view;
    size_t size;
    uint32_t offset;
    (void)unused;
    CHECK(tx && rx && cc && dc && data && coded && output);
    random_bytes(data, GD_MAX_FRAME);
    size = GD_compress(tx, cc, coded, ZSTD_compressBound(GD_MAX_FRAME), data, GD_MAX_FRAME, &view);
    CHECK(!ZSTD_isError(size));
    CHECK(GD_decompress(rx, dc, output, GD_MAX_FRAME, coded, size, &view) == GD_MAX_FRAME);
    CHECK(!memcmp(output, data, GD_MAX_FRAME));
    CHECK(GD_append(tx, 2, data, 512, &offset) == GD_OK);
    write_part(rx, GD_prepare(tx, 2), data, 512);
    size = GD_compress(tx, cc, coded, ZSTD_compressBound(GD_MAX_FRAME), data, GD_MAX_FRAME, &view);
    CHECK(!ZSTD_isError(size) && view.used_mask != 0);
    CHECK(GD_decompress(rx, dc, output, GD_MAX_FRAME, coded, size, &view) == GD_MAX_FRAME);
    CHECK(!memcmp(output, data, GD_MAX_FRAME));
    free(data); free(coded); free(output);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
    return NULL;
}

static void test_committed_match_precedes_longer_prepare_match(void)
{
    GD_Store* tx = GD_create(CAP, 1);
    unsigned char data[512];
    ZSTD_Sequence sequence[8];
    GD_FrameView view;
    uint32_t offset;
    size_t count;
    CHECK(tx);
    random_bytes(data, sizeof(data));
    CHECK(GD_append(tx, 2, data, 256, &offset) == GD_OK);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, 4), 3, GD_epoch(tx, 3), NULL, 0) == GD_OK);
    CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK);
    CHECK(GD_sequences(tx, data, sizeof(data), sequence, 8, &count, &view) == GD_OK);
    CHECK(count == 3 && sequence[0].matchLength == 256 && sequence[1].matchLength == 256);
    CHECK(sequence[0].offset == GD_PARTITIONS * CAP - 5 * CAP);
    CHECK(view.used_mask == ((1U << 5) | (1U << 4)));
    GD_free(tx);
}

static void test_learning_only_novel_spans(void)
{
    GD_Store* tx = GD_create(CAP, 1);
    unsigned char old[256], frame[328], learned_bytes[72], filler[CAP];
    GD_Missing learned;
    uint32_t offset;
    uint64_t heat, written;
    CHECK(tx);
    random_bytes(old, sizeof(old)); random_bytes(frame, sizeof(frame));
    memcpy(frame + 32, old, sizeof(old));
    CHECK(GD_append(tx, 0, old, sizeof(old), &offset) == GD_OK);
    heat = GD_blockHits(tx, GD_prepare(tx, 0), 0);
    written = GD_stats(tx)->payload_written;
    CHECK(GD_learn(tx, frame, sizeof(frame), &learned) == GD_OK);
    CHECK(learned.partition == 5 && learned.epoch == 6 && learned.offset == 0 && learned.length == 72);
    CHECK(GD_stats(tx)->payload_written - written == 72);
    CHECK(GD_blockHits(tx, GD_prepare(tx, 0), 0) == heat);
    CHECK(GD_read(tx, 5, 6, 0, learned_bytes, sizeof(learned_bytes)) == GD_OK);
    CHECK(!memcmp(learned_bytes, frame, 32) && !memcmp(learned_bytes + 32, frame + 288, 40));
    CHECK(GD_learn(tx, frame, sizeof(frame), &learned) == GD_OK && learned.length == 0);
    CHECK(GD_stats(tx)->payload_written - written == 72);
    CHECK(GD_learn(tx, old, sizeof(old), &learned) == GD_OK && learned.length == 0);
    random_bytes(filler, sizeof(filler));
    CHECK(GD_append(tx, 2, filler, CAP - 72, &offset) == GD_OK);
    random_bytes(frame, sizeof(frame));
    written = GD_stats(tx)->payload_written;
    CHECK(GD_learn(tx, frame, sizeof(frame), &learned) == GD_CAPACITY && learned.length == 0);
    CHECK(GD_stats(tx)->payload_written == written && GD_extent(tx, 5) == CAP);
    GD_free(tx);
}

static void test_small_thread_stack(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstacksize(&attr, 128 * 1024) == 0);
    CHECK(pthread_create(&thread, &attr, encode_on_small_thread_stack, NULL) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
}

int main(void)
{
    test_identified_independent_frames();
    test_append_and_conflict();
    test_mixed_holes_and_retirement();
    test_promotion_with_missing_block();
    test_borrowed_continuous_payload();
    test_partial_source_copy_and_destination_repair();
    test_random_roundtrips();
    test_standard_bitstream_and_bounds();
    test_large_external_history();
    test_learning_after_initial_loss();
    test_tier_capacities_and_observation();
    test_reencoding_does_not_amplify_retention();
    test_retention_by_bytes_and_continuity();
    test_compaction_preserves_source_and_reserves_actual_bytes();
    test_small_thread_stack();
    test_committed_match_precedes_longer_prepare_match();
    test_learning_only_novel_spans();
    puts("PASS append, immutable overlap, mixed partitions, holes, promotion, retire, 3000 seeded roundtrips and mutations, standard bitstream, bounds");
    return 0;
}
