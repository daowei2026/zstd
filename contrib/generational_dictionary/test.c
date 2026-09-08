/* Behavioral tests for the fork prototype; repository BSD license. */
#define _POSIX_C_SOURCE 200809L
#include "dictionary.h"
#include "fixture_uuid.h"
#include "../../lib/zstd_segmented.h"
#include "../../lib/zstd_errors.h"
#include "../../lib/common/mem.h"
#include "../../lib/common/xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CAP (2 * GD_BLOCK_SIZE)
static GD_Epoch test_epochs[GD_PARTITIONS], next_epochs[GD_PARTITIONS];
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
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
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
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    size_t length = 0, size, saved_size, decoded;
    GD_Epoch epoch;
    uint64_t allocated;
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
    CHECK(GD_rotate(tx, 0, epoch, next_epochs[GD_committed(tx, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 0, epoch, next_epochs[GD_committed(rx, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    write_part(tx, 0, b, GD_BLOCK_SIZE); write_part(rx, 0, b, GD_BLOCK_SIZE);
    retained = GD_blockAddress(tx, 1, 0);
    allocated = GD_stats(tx)->payload_allocated;
    epoch = GD_epoch(tx, 1);
    CHECK(GD_rotate(tx, 0, epoch, next_epochs[GD_committed(tx, 0)], 1, epoch, &keep, 1) == GD_INVALID);
    CHECK(GD_epochEqual(GD_epoch(tx, 1), epoch) && !memcmp(retained, a, GD_BLOCK_SIZE));
    CHECK(GD_rotate(tx, 0, epoch, next_epochs[GD_committed(tx, 0)], 0, GD_epoch(tx, 0), &keep, 1) == GD_OK);
    CHECK(GD_rotate(rx, 0, epoch, next_epochs[GD_committed(rx, 0)], 0, GD_epoch(rx, 0), &keep, 1) == GD_OK);
    CHECK(GD_blockAddress(tx, 0, 1) != retained);
    CHECK(!memcmp(retained, a, GD_BLOCK_SIZE));
    CHECK(!memcmp(GD_blockAddress(tx, 0, 1), a, GD_BLOCK_SIZE));
    CHECK(GD_blockAddress(tx, 1, 1) == NULL);
    CHECK(GD_stats(tx)->payload_allocated == allocated);
    CHECK(GD_stats(tx)->payload_relocated == GD_BLOCK_SIZE);
    CHECK(GD_rotate(rx, 0, epoch, next_epochs[GD_committed(rx, 0)], 1, epoch, &keep, 1) == GD_STALE);
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
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
    GD_Move move = {1, 0}, invalid[2] = {{1, 0}, {1, GD_BLOCK_SIZE}};
    GD_Epoch epoch;
    uint64_t allocated, written;
    const void* address;
    CHECK(tx && rx);
    random_bytes(data, CAP);
    write_part(tx, 5, data, CAP);
    write_part(rx, 5, data, GD_BLOCK_SIZE);
    epoch = GD_epoch(tx, 4);
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], 3, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, next_epochs[GD_committed(rx, 2)], 3, GD_NO_EPOCH, NULL, 0) == GD_OK);
    epoch = GD_epoch(tx, 5);
    allocated = GD_stats(tx)->payload_allocated;
    written = GD_stats(tx)->payload_written;
    address = GD_blockAddress(tx, 5, 1);
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], 3, next_epochs[3], &move, 1) == GD_STALE);
    CHECK(GD_epochEqual(GD_epoch(tx, 5), epoch) && GD_blockAddress(tx, 5, 1) == address);
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], 3, GD_epoch(tx, 3), invalid, 2) == GD_INVALID);
    CHECK(GD_epochEqual(GD_epoch(tx, 5), epoch) && GD_stats(tx)->payload_allocated == allocated);
    CHECK(GD_stats(tx)->payload_written == written);
    CHECK(GD_read(tx, 3, GD_epoch(tx, 3), 0, output, 1) == GD_MISSING);
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], 3, GD_epoch(tx, 3), &move, 1) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, next_epochs[GD_committed(rx, 2)], 3, GD_epoch(tx, 3), &move, 1) == GD_OK);
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
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
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
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]}, {0}, {1,3,5}, NULL, 0, NULL, 0};
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
        store = GD_createWithBuffers(capacities, buffers, mode, &layout);
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
        CHECK(GD_rotate(store, 0, GD_epoch(store, 0), next_epochs[GD_committed(store, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
        CHECK(GD_rotate(store, 0, GD_epoch(store, 1), next_epochs[GD_committed(store, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
        CHECK(GD_read(store, 1, GD_epoch(store, 1), 0, output, 1) == GD_MISSING);
        CHECK(!memcmp(buffers[1], data, sizeof(data)));
        GD_free(store);
        CHECK(!memcmp(buffers[1], data, sizeof(data)));
    }
    /* Adjacent half ranges are legal; overlapping ownership is rejected. */
    buffers[1] = generations[0] + capacities[0] - 1;
    CHECK(!GD_createWithBuffers(capacities, buffers, 1, &layout));
    buffers[1] = NULL;
    CHECK(!GD_createWithBuffers(capacities, buffers, 0, &layout));
    for (tier = 0; tier < 3; ++tier) free(generations[tier]);
}

static void test_partial_source_copy_and_destination_repair(void)
{
    unsigned char data[GD_BLOCK_SIZE], output[GD_BLOCK_SIZE];
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
    GD_Move move = {0, GD_BLOCK_SIZE};
    GD_Epoch epoch;
    uint64_t writes;
    GD_PartitionStats observed;
    CHECK(tx && rx);
    random_bytes(data, sizeof(data));
    write_part(tx, 5, data, sizeof(data));
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 100, data + 100, 60) == GD_OK);
    CHECK(GD_write(rx, 5, GD_epoch(rx, 5), 900, data + 900, 80) == GD_OK);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, 4), next_epochs[GD_committed(tx, 2)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 2, GD_epoch(rx, 4), next_epochs[GD_committed(rx, 2)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    epoch = GD_epoch(tx, 5); writes = GD_stats(rx)->payload_written;
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], 3, GD_epoch(tx, 3), &move, 1) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, next_epochs[GD_committed(rx, 2)], 3, GD_epoch(rx, 3), &move, 1) == GD_OK);
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
    unsigned char payloads[GD_PARTITIONS][CAP], frame[1514], output[1514], compressed[2048];
    GD_Store* tx = GD_create(CAP, 1, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    ZSTD_DDict* dd;
    GD_FrameView view;
    ZSTD_Sequence sequence[2];
    size_t size;
    unsigned p;
    CHECK(tx && cc && dc);
    random_bytes(payloads, sizeof(payloads));
    for (p = 0; p < 6; ++p) write_part(tx, p, payloads[p], CAP);
    for (p = 0; p < 3; ++p) memcpy(frame + p * 480, payloads[2 * p] + CAP - 480, 480);
    random_bytes(frame + 1440, sizeof(frame) - 1440);
    size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == 0x15);
    /* Decode each independent frame with the ordinary raw-dictionary decoder.
     * Raw dictionaries have ID 0: remove only the native ID field in this test
     * oracle, leaving the compressed block bytes unchanged. No joined history. */
    {
        size_t at = 0, restored = 0;
        while (at < size) {
            unsigned char local[2048];
            size_t const n = ZSTD_findFrameCompressedSize(compressed + at, size - at);
            unsigned const id = ZSTD_getDictID_fromFrame(compressed + at, n);
            size_t decoded, local_size = n;
            CHECK(!ZSTD_isError(n) && n <= sizeof(local));
            memcpy(local, compressed + at, n);
            dd = NULL;
            if (id) {
                size_t const id_at = 5 + !(local[4] & 32);
                unsigned const slot = id - GD_DICTIONARY_ID_BASE;
                CHECK(slot < GD_PARTITIONS && (local[4] & 3) == 2);
                local[4] &= (unsigned char)~3U;
                memmove(local + id_at, local + id_at + 2, n - id_at - 2);
                local_size -= 2;
                dd = ZSTD_createDDict_advanced(payloads[slot], CAP,
                    ZSTD_dlm_byRef, ZSTD_dct_rawContent, ZSTD_defaultCMem);
                CHECK(dd);
            }
            decoded = ZSTD_decompress_usingDDict(dc, output + restored,
                sizeof(output) - restored, local, local_size, dd);
            CHECK(!ZSTD_isError(decoded));
            restored += decoded; at += n; ZSTD_freeDDict(dd);
        }
        CHECK(restored == sizeof(frame));
    }
    CHECK(memcmp(frame, output, sizeof(frame)) == 0);
    memset(sequence, 0, sizeof(sequence));
    sequence[0].matchLength = 16; sequence[0].offset = CAP + 1;
    CHECK(!ZSTD_isError(ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters)));
    CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed), sequence, 2, frame, 16, CAP, 0)));
    CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed), sequence, 2, frame, 65536, CAP, 0)));
    CHECK(ZSTD_isError(ZSTD_compressSequencesWithExternalDictSize(cc, compressed, sizeof(compressed), NULL, 1, frame, 16, CAP, 0)));
    /* No-dictionary fallback retains intra-frame repetition and overlap. */
    memset(frame, 'a', sizeof(frame));
    size = GD_compress(tx, cc, compressed, sizeof(compressed), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && size < 30 && view.used_mask == 0);
    CHECK(GD_decompress(tx, dc, output, sizeof(output), compressed, size, &view) == sizeof(frame));
    CHECK(memcmp(frame, output, sizeof(frame)) == 0);
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx);
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
    GD_Store *tx = GD_create(65536, 1, test_epochs), *rx = GD_create(65536, 0, test_epochs);
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
    GD_Store *tx = GD_createWithCapacities(capacities, 1, test_epochs), *rx = GD_createWithCapacities(capacities, 0, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char data[5 * GD_BLOCK_SIZE + 13], frame[3 * 256], output[3 * 256], compressed[2048];
    GD_FrameView view; GD_PartitionStats snapshot;
    unsigned tier, source, destination; uint32_t offset; GD_Epoch epoch, target;
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
        CHECK(GD_rotate(tx, tier, epoch, next_epochs[GD_committed(tx, tier)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
        CHECK(GD_rotate(rx, tier, epoch, next_epochs[GD_committed(rx, tier)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    }
    source = GD_committed(tx, 2); destination = GD_prepare(tx, 1);
    epoch = GD_epoch(tx, source); target = GD_epoch(tx, destination);
    address = GD_blockAddress(tx, source, 0);
    move.source_block = 0; move.destination_offset = 3 * GD_BLOCK_SIZE;
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], destination, target, &move, 1) == GD_CAPACITY);
    CHECK(GD_epochEqual(GD_epoch(tx, source), epoch) && GD_blockAddress(tx, source, 0) == address);
    move.destination_offset = 0;
    CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], destination, target, &move, 1) == GD_OK);
    CHECK(GD_rotate(rx, 2, epoch, next_epochs[GD_committed(rx, 2)], destination, target, &move, 1) == GD_OK);
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
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
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
    GD_Store* tx = GD_create(sizeof(data), 1, test_epochs);
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
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, p), next_epochs[GD_committed(tx, 2)], dst, GD_epoch(tx, dst), NULL, 0) == GD_OK);
    for (i = 0; i < 10; ++i) {
        n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 0);
        CHECK(!ZSTD_isError(n));
    }
    CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE, moves, 1) == 0);
    for (i = 0; i < 10; ++i) {
        n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 1);
        CHECK(!ZSTD_isError(n));
    }
    for (i = 0; i < 100; ++i) {
        n = GD_compressTracked(tx, cc, coded, sizeof(coded), data + GD_BLOCK_SIZE, 64, &view, 1);
        CHECK(!ZSTD_isError(n));
    }
    CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE, moves, 1) == 1 && moves[0].source_block == 0);
    p = GD_committed(tx, 2);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, p), next_epochs[GD_committed(tx, 2)], dst, GD_epoch(tx, dst), moves, 1) == GD_OK);
    p = GD_committed(tx, 1); dst = GD_prepare(tx, 0);
    CHECK(GD_rotate(tx, 1, GD_epoch(tx, p), next_epochs[GD_committed(tx, 1)], dst, GD_epoch(tx, dst), NULL, 0) == GD_OK);
    CHECK(GD_selectMoves(tx, 1, 0, GD_BLOCK_SIZE, moves, 1) == 1); /* Retention inherits real reuse. */
    CHECK(GD_blockHits(tx, GD_committed(tx, 1), 0) == 10);
    GD_free(tx);

    tx = GD_create(sizeof(data), 1, test_epochs);
    CHECK(tx);
    CHECK(GD_append(tx, 0, data, sizeof(data), &offset) == GD_OK);
    p = GD_committed(tx, 0);
    CHECK(GD_rotate(tx, 0, GD_epoch(tx, p), next_epochs[GD_committed(tx, 0)], p, GD_epoch(tx, p), NULL, 0) == GD_OK);
    n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, 64, &view, 1);
    CHECK(!ZSTD_isError(n));
    n = GD_compressTracked(tx, cc, coded, sizeof(coded), data + 3 * GD_BLOCK_SIZE - 64, 128, &view, 1);
    CHECK(!ZSTD_isError(n));
    CHECK(GD_selectMoves(tx, 0, 0, 2 * GD_BLOCK_SIZE, moves, 2) == 2);
    CHECK(moves[0].source_block != moves[1].source_block && moves[0].source_block + moves[1].source_block == 5);
    GD_free(tx); ZSTD_freeCCtx(cc);
}

static void test_compaction_preserves_source_and_reserves_actual_bytes(void)
{
    unsigned reverse, fits;
    for (reverse = 0; reverse < 2; ++reverse) for (fits = 0; fits < 3; ++fits) {
        unsigned char data[GD_BLOCK_SIZE + 128] = {0}, padding[4 * GD_BLOCK_SIZE] = {0};
        unsigned char united[192], coded[256], readback[sizeof(data)];
        GD_Store* tx = GD_create(sizeof(padding), 1, test_epochs);
        ZSTD_CCtx* cc = ZSTD_createCCtx();
        GD_FrameView view;
        GD_Move moves[2] = {{0, 0}, {1, GD_BLOCK_SIZE}};
        GD_Missing appended;
        GD_PartitionStats source_state, target_state;
        uint32_t offset, required;
        GD_Epoch epoch;
        uint64_t written, inherited_hits, actual_matches, target_hits;
        uint64_t const saved_rng = rng;
        unsigned source, destination, i;
        size_t count = 2, n;
        CHECK(tx && cc);
        CHECK(GD_setHeatPolicy(tx, 10, 0) == GD_OK);
        GD_setTime(tx, 100);
        random_bytes(united, sizeof(united));
        memcpy(data, united + (reverse ? 64 : 0), 128);
        memcpy(data + GD_BLOCK_SIZE, united + (reverse ? 0 : 64), 128);
        CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK);
        source = GD_committed(tx, 2); destination = GD_prepare(tx, 1);
        CHECK(GD_rotate(tx, 2, GD_epoch(tx, source), next_epochs[GD_committed(tx, 2)], destination, GD_epoch(tx, destination), NULL, 0) == GD_OK);
        source = GD_committed(tx, 2); epoch = GD_epoch(tx, source);
        for (i = 0; i < 2; ++i) {
            n = GD_compressTracked(tx, cc, coded, sizeof(coded), data, 128, &view, 1);
            CHECK(!ZSTD_isError(n));
            n = GD_compressTracked(tx, cc, coded, sizeof(coded), data + GD_BLOCK_SIZE, 128, &view, 1);
            CHECK(!ZSTD_isError(n));
        }
        GD_setTime(tx, 110);
        CHECK(GD_observePartition(tx, source, &source_state) == GD_OK && source_state.heat > 0);
        inherited_hits = GD_blockHits(tx, source, 0) + GD_blockHits(tx, source, 1);
        actual_matches = GD_stats(tx)->matched_bytes[2];
        CHECK(GD_append(tx, 1, padding, fits == 2 ? GD_BLOCK_SIZE - 40 : sizeof(padding) - sizeof(united) + !fits, &offset) == GD_OK);
        written = GD_stats(tx)->payload_written;
        if (!fits) {
            CHECK(GD_compactMoves(tx, 2, epoch, destination, GD_epoch(tx, destination), sizeof(padding), moves, &count, &appended, &required) == GD_CAPACITY);
            CHECK(required == sizeof(united) && count == 2 && !appended.length);
            CHECK(GD_stats(tx)->payload_written == written && moves[1].source_block == 1);
            i = GD_committed(tx, 1);
            CHECK(GD_rotate(tx, 1, GD_epoch(tx, i), next_epochs[GD_committed(tx, 1)], GD_prepare(tx, 0), GD_epoch(tx, GD_prepare(tx, 0)), NULL, 0) == GD_OK);
            destination = GD_prepare(tx, 1);
        }
        CHECK(GD_compactMoves(tx, 2, epoch, destination, GD_epoch(tx, destination), sizeof(padding) + 1, moves, &count, &appended, &required) == GD_INVALID);
        CHECK(GD_compactMoves(tx, 2, epoch, destination, GD_epoch(tx, destination), GD_extent(tx, destination) + sizeof(united) - 1, moves, &count, &appended, &required) == GD_CAPACITY);
        CHECK(count == 2 && !appended.length && GD_stats(tx)->payload_written == written);
        CHECK(moves[0].source_block == 0 && moves[1].source_block == 1);
        CHECK(GD_compactMoves(tx, 2, epoch, destination, GD_epoch(tx, destination), GD_extent(tx, destination) + sizeof(united), moves, &count, &appended, &required) == GD_OK);
        CHECK(count == 0 && required == sizeof(united) && appended.length == sizeof(united));
        CHECK(GD_stats(tx)->payload_written == written + sizeof(united));
        CHECK(GD_stats(tx)->payload_relocated == sizeof(united));
        CHECK(GD_observePartition(tx, destination, &target_state) == GD_OK && fabs(target_state.heat - source_state.heat) < 0.001);
        target_hits = 0;
        for (i = 0; i < 4; ++i) target_hits += GD_blockHits(tx, destination, i);
        CHECK(target_hits == inherited_hits && GD_stats(tx)->matched_bytes[2] == actual_matches);
        GD_setTime(tx, 120);
        CHECK(GD_observePartition(tx, destination, &target_state) == GD_OK && fabs(target_state.heat - source_state.heat / 2) < 0.001);
        CHECK(GD_read(tx, source, epoch, 0, readback, sizeof(data)) == GD_OK && !memcmp(readback, data, sizeof(data)));
        CHECK(GD_read(tx, destination, appended.epoch, appended.offset, readback, appended.length) == GD_OK && !memcmp(readback, united, sizeof(united)));
        CHECK(GD_rotate(tx, 2, epoch, next_epochs[GD_committed(tx, 2)], destination, GD_epoch(tx, destination), moves, count) == GD_OK);
        CHECK(GD_read(tx, source, epoch, 0, readback, 1) == GD_STALE);
        CHECK(GD_stats(tx)->payload_peak_allocated == GD_stats(tx)->payload_allocated);
        GD_free(tx); ZSTD_freeCCtx(cc);
        if (fits == 2) rng = saved_rng;
    }
}

static void* encode_on_small_thread_stack(void* unused)
{
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
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
    GD_Store* tx = GD_create(CAP, 1, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    unsigned char data[512], coded[1024];
    const GD_Match* matches;
    GD_FrameView view;
    uint32_t offset;
    size_t count;
    CHECK(tx && cc);
    random_bytes(data, sizeof(data));
    CHECK(GD_append(tx, 2, data, 256, &offset) == GD_OK);
    CHECK(GD_rotate(tx, 2, GD_epoch(tx, 4), next_epochs[GD_committed(tx, 2)], 3, GD_epoch(tx, 3), NULL, 0) == GD_OK);
    CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data, sizeof(data), &view)));
    matches = GD_matches(tx, &count);
    CHECK(count == 2 && matches[0].length == 256 && matches[1].length == 256);
    CHECK(matches[0].partition == 5 && matches[0].dictionary_offset == 0);
    CHECK(matches[1].partition == 4 && matches[1].dictionary_offset == 256);
    CHECK(view.used_mask == ((1U << 5) | (1U << 4)));
    ZSTD_freeCCtx(cc); GD_free(tx);
}

static void test_learning_only_novel_spans(void)
{
    GD_Store* tx = GD_create(CAP, 1, test_epochs);
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
    CHECK(learned.partition == 5 && GD_epochEqual(learned.epoch, test_epochs[5]) && learned.offset == 0 && learned.length == 72);
    CHECK(GD_stats(tx)->payload_written - written == 72);
    CHECK(GD_blockHits(tx, GD_prepare(tx, 0), 0) == heat);
    CHECK(GD_read(tx, 5, test_epochs[5], 0, learned_bytes, sizeof(learned_bytes)) == GD_OK);
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

static void test_readonly_recovery(void)
{
    uint32_t capacities[3] = {512, 512, 512};
    unsigned char payloads[GD_PARTITIONS][512], output[100];
    void* buffers[GD_PARTITIONS];
    GD_Missing ranges[2] = {{1, test_epochs[1], 17, 100}, {1, test_epochs[1], 203, 90}};
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]}, {0,400,0,0,0,0}, {0,3,4}, ranges, 2, NULL, 0};
    GD_Store* store;
    unsigned i;
    memset(payloads, 0xa7, sizeof(payloads));
    for (i = 0; i < GD_PARTITIONS; ++i) buffers[i] = payloads[i];
    store = GD_createWithBuffers(capacities, buffers, 1, &layout);
    CHECK(store);
    CHECK(GD_read(store, 1, test_epochs[1], 17, output, sizeof(output)) == GD_OK);
    CHECK(!memcmp(output, payloads[1] + 17, sizeof(output)));
    CHECK(GD_read(store, 1, test_epochs[1], 117, output, 1) == GD_MISSING);
    CHECK(GD_read(store, 1, test_epochs[1], 400, output, 1) == GD_MISSING);
    CHECK(GD_prepare(store, 0) == 0 && GD_prepare(store, 2) == 4 && GD_extent(store, 1) == 400);
    CHECK(GD_stats(store)->payload_written == 0 && GD_stats(store)->indexed_positions == 0);
    GD_free(store);
    for (i = 0; i < sizeof(payloads); ++i) CHECK(((unsigned char*)payloads)[i] == 0xa7);
    /* Invalid metadata cannot turn tails/overlaps into ready bytes. */
    layout.prepare[0] = 2;
    CHECK(!GD_createWithBuffers(capacities, buffers, 1, &layout));
    layout.prepare[0] = 0; ranges[1].offset = 100;
    CHECK(!GD_createWithBuffers(capacities, buffers, 1, &layout));
    ranges[1].offset = 203; ranges[1].epoch = next_epochs[1];
    CHECK(!GD_createWithBuffers(capacities, buffers, 1, &layout));
    ranges[1].epoch = test_epochs[1]; layout.extent[1] = 513;
    CHECK(!GD_createWithBuffers(capacities, buffers, 1, &layout));
}

static void test_file_mappings_are_not_modified_by_recovery(void)
{
    uint32_t capacities[3] = {CAP, CAP, CAP};
    unsigned char original[3][2 * CAP], output[400], coded[800];
    void *mapped[3], *buffers[GD_PARTITIONS];
    GD_Missing ranges[4] = {{1, test_epochs[1],211,200}, {1, test_epochs[1],601,400}, {3, test_epochs[3],97,800}, {5, test_epochs[5],1,1100}};
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]}, {0,1200,0,1000,0,1500}, {1,3,5}, ranges, 4, NULL, 0};
    GD_Store *tx, *rx;
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    GD_FrameView view;
    GD_PartitionStats observed;
    GD_Missing learned;
    GD_IndexSnapshot indexes[GD_PARTITIONS] = {{0}};
    void* snapshot;
    size_t snapshot_size, written;
    size_t size;
    unsigned i;
    uint64_t const saved_rng = rng;
    const char* tmp = getenv("TMPDIR");
    CHECK(cc && dc && tmp && tmp[0] == '/');
    rng = 0x43ea079a028694acULL; random_bytes(original, sizeof(original)); rng = saved_rng;
    for (i = 0; i < 3; ++i) {
        char path[4096];
        int fd, n = snprintf(path, sizeof(path), "%s/gd-payload-XXXXXX", tmp);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        fd = mkstemp(path); CHECK(fd >= 0);
        /* Unlink the task-owned temporary name immediately; the fd and mappings
         * keep the file alive, and all exit paths release it automatically. */
        CHECK(unlink(path) == 0);
        CHECK(write(fd, original[i], sizeof(original[i])) == sizeof(original[i]));
        CHECK(fsync(fd) == 0);
        mapped[i] = mmap(NULL, sizeof(original[i]), PROT_READ, MAP_SHARED, fd, 0);
        CHECK(mapped[i] != MAP_FAILED && close(fd) == 0);
        buffers[2 * i] = mapped[i]; buffers[2 * i + 1] = (unsigned char*)mapped[i] + CAP;
    }
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout);
    rx = GD_createWithBuffers(capacities, buffers, 0, &layout);
    CHECK(tx && rx && GD_stats(tx)->indexed_positions == 0);
    CHECK(GD_observePartition(tx, 1, &observed) == GD_OK && observed.unclaimed_bytes == 600);
    CHECK(observed.unclaimed_ranges == 2 && observed.present_bytes == 600);
    CHECK(GD_observePartition(rx, 1, &observed) == GD_OK && observed.unclaimed_bytes == 0);
    size = GD_compress(tx, cc, coded, sizeof(coded), (unsigned char*)buffers[1] + 601, 300, &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 1));
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == 300);
    CHECK(!memcmp(output, (unsigned char*)buffers[1] + 601, 300));
    CHECK(GD_observePartition(tx, 1, &observed) == GD_OK && observed.unclaimed_bytes == 300);
    CHECK(observed.unclaimed_ranges == 2 && GD_stats(tx)->payload_recognized == 300);
    {
        GD_Layout candidates = layout;
        GD_Store* verified;
        GD_Missing claims[2] = {{1, test_epochs[1], 601, 300}, {3, next_epochs[3], 97, 800}};
        size_t count;
        uint64_t const checksum = XXH64(original[0] + CAP + 601, 300, 0);
        candidates.ranges = NULL; candidates.range_count = 0; candidates.extent[5] = CAP;
        verified = GD_createWithBuffers(capacities, buffers, 0, &candidates); CHECK(verified);
        CHECK(GD_read(verified, 1, test_epochs[1], 601, output, 300) == GD_MISSING);
        CHECK(GD_claimRanges(tx, claims, 1) == GD_INVALID);
        CHECK(GD_claimRanges(NULL, claims, 1) == GD_INVALID);
        CHECK(GD_claimRanges(verified, NULL, 1) == GD_INVALID);
        CHECK(GD_claimRanges(verified, NULL, 0) == GD_OK);
        /* A later invalid/stale member cannot adopt an earlier valid member. */
        CHECK(GD_claimRanges(verified, claims, 2) == GD_STALE);
        claims[1] = (GD_Missing){3, test_epochs[3], UINT32_MAX, 800};
        CHECK(GD_claimRanges(verified, claims, 2) == GD_INVALID);
        claims[1] = (GD_Missing){3, test_epochs[3], 1000, 1};
        CHECK(GD_claimRanges(verified, claims, 2) == GD_INVALID);
        claims[1] = (GD_Missing){3, test_epochs[3], 97, 0};
        CHECK(GD_claimRanges(verified, claims, 2) == GD_INVALID);
        claims[1] = (GD_Missing){GD_PARTITIONS, test_epochs[3], 97, 1};
        CHECK(GD_claimRanges(verified, claims, 2) == GD_INVALID);
        CHECK(GD_exportRanges(verified, NULL, 0, &count) == GD_OK && !count);
        CHECK(GD_read(rx, 1, test_epochs[1], 601, output, 300) == GD_OK);
        /* The caller compares actual mapped bytes first. Claiming is a separate
         * metadata operation and must work even though all files are PROT_READ. */
        CHECK(XXH64((unsigned char*)buffers[1] + 601, 300, 0) == checksum);
        CHECK(GD_claimRanges(verified, claims, 1) == GD_OK);
        CHECK(GD_decompress(verified, dc, output, sizeof(output), coded, size, &view) == 300);
        CHECK(!memcmp(output, original[0] + CAP + 601, 300));
        CHECK(GD_read(verified, 1, test_epochs[1], 211, output, 200) == GD_MISSING);
        claims[1] = (GD_Missing){1, test_epochs[1], 700, 301};
        CHECK(GD_claimRanges(verified, claims, 2) == GD_OK);
        CHECK(GD_claimRanges(verified, claims, 2) == GD_OK);
        CHECK(GD_observePartition(verified, 1, &observed) == GD_OK && observed.present_bytes == 400);
        CHECK(!observed.heat && !observed.unclaimed_bytes);
        CHECK(!GD_stats(verified)->payload_written && !GD_stats(verified)->payload_relocated);
        CHECK(!GD_stats(verified)->index_allocated && !GD_stats(verified)->indexed_positions);
        CHECK(!GD_stats(verified)->payload_recognized && !GD_stats(verified)->unclaimed_scanned);
        CHECK(GD_blockAddress(verified, 1, 0) == buffers[1]);
        CHECK(GD_rotate(verified, 0, test_epochs[0], next_epochs[0], 1, test_epochs[1], NULL, 0) == GD_OK);
        CHECK(GD_rotate(verified, 0, test_epochs[1], next_epochs[1], 0, next_epochs[0], NULL, 0) == GD_OK);
        CHECK(GD_claimRanges(verified, claims, 1) == GD_STALE);
        CHECK(GD_exportRanges(verified, NULL, 0, &count) == GD_OK && !count);
        /* Independent validity views of the same bytes do not change each other. */
        CHECK(GD_read(rx, 1, test_epochs[1], 601, output, 300) == GD_OK);
        claims[0] = (GD_Missing){5, test_epochs[5], GD_BLOCK_SIZE - 3, 17};
        CHECK(GD_claimRanges(verified, claims, 1) == GD_OK);
        CHECK(GD_read(verified, 5, test_epochs[5], GD_BLOCK_SIZE - 3, output, 17) == GD_OK);
        CHECK(!memcmp(output, original[2] + CAP + GD_BLOCK_SIZE - 3, 17));
        CHECK(GD_read(verified, 5, test_epochs[5], GD_BLOCK_SIZE - 4, output, 1) == GD_MISSING);
        CHECK(GD_read(verified, 5, test_epochs[5], GD_BLOCK_SIZE + 14, output, 1) == GD_MISSING);
        CHECK(!GD_stats(verified)->payload_written && !GD_stats(verified)->index_allocated);
        GD_free(verified);
    }
    /* The index section is an ordinary file, read into RAM. Restoring it over
     * the PROT_READ payload mappings must preserve coverage without discovery. */
    snapshot_size = GD_indexSnapshotSize(tx, 1);
    snapshot = malloc(snapshot_size); CHECK(snapshot && snapshot_size);
    CHECK(GD_saveIndex(tx, 1, snapshot, snapshot_size, &written) == GD_OK && written == snapshot_size);
    {
        char path[4096];
        int fd, n = snprintf(path, sizeof(path), "%s/gd-index-XXXXXX", tmp);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        fd = mkstemp(path); CHECK(fd >= 0 && unlink(path) == 0);
        CHECK(write(fd, snapshot, snapshot_size) == (ssize_t)snapshot_size && fsync(fd) == 0);
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        memset(snapshot, 0, snapshot_size);
        CHECK(read(fd, snapshot, snapshot_size) == (ssize_t)snapshot_size && close(fd) == 0);
    }
    GD_free(tx);
    indexes[1] = (GD_IndexSnapshot){snapshot, snapshot_size}; layout.indexes = indexes;
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_observePartition(tx, 1, &observed) == GD_OK && observed.unclaimed_bytes == 300);
    CHECK(observed.recovery.index_result == GD_OK && observed.recovery.restored_positions > 0);
    CHECK(observed.recovery.checksum_bytes == 600 && !observed.recovery.checksum_failures);
    CHECK(GD_setUnclaimedWindow(tx, 0) == GD_OK);
    size = GD_compressTracked(tx, cc, coded, sizeof(coded), (unsigned char*)buffers[1] + 601, 300, &view, 0);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 1));
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == 300);
    CHECK(!memcmp(output, (unsigned char*)buffers[1] + 601, 300));
    CHECK(!GD_stats(tx)->payload_written && !GD_stats(tx)->unclaimed_scanned && !GD_stats(tx)->payload_recognized);
    free(snapshot); layout.indexes = NULL;
    GD_free(tx);
    /* Lose the entire RAM index, then learn directly from the same read-only
     * file mapping. Recognizing existing bytes must not attempt adhoc append. */
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_learn(tx, (unsigned char*)buffers[1] + 601, 300, &learned) == GD_OK && learned.length == 0);
    CHECK(GD_stats(tx)->payload_recognized == 300 && GD_blockHits(tx, 1, 0) == 0);
    CHECK(GD_stats(tx)->payload_written == 0 && GD_stats(tx)->payload_relocated == 0);
    CHECK(GD_stats(rx)->payload_written == 0 && GD_stats(rx)->payload_relocated == 0);
    GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
    for (i = 0; i < 3; ++i) {
        CHECK(!memcmp(mapped[i], original[i], sizeof(original[i])));
        CHECK(munmap(mapped[i], sizeof(original[i])) == 0);
    }
}

static void test_unclaimed_order_and_cursor(void)
{
    uint32_t capacities[3] = {CAP, CAP, CAP};
    unsigned char data[GD_PARTITIONS][CAP], pattern[512], coded[800];
    void* buffers[GD_PARTITIONS];
    GD_Missing ranges[3] = {{1, test_epochs[1],0,512}, {3, test_epochs[3],0,512}, {5, test_epochs[5],0,512}};
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]}, {0,512,0,512,0,512}, {1,3,5}, ranges, 3, NULL, 0};
    GD_Store* tx;
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    GD_FrameView view;
    GD_PartitionStats state;
    GD_Missing learned;
    size_t size;
    uint32_t offset;
    unsigned i;
    uint64_t const saved_rng = rng;
    rng = 0xb6c44e59d74b1a20ULL; random_bytes(data, sizeof(data)); random_bytes(pattern, sizeof(pattern)); rng = saved_rng;
    CHECK(cc);
    for (i = 0; i < GD_PARTITIONS; ++i) buffers[i] = data[i];
    for (i = 0; i < 3; ++i) memcpy(data[2 * i + 1], pattern, sizeof(pattern));
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_append(tx, 2, pattern, sizeof(pattern), &offset) == GD_OK);
    size = GD_compress(tx, cc, coded, sizeof(coded), pattern, sizeof(pattern), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 5));
    CHECK(!GD_stats(tx)->unclaimed_scanned);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.unclaimed_bytes == 512);
    GD_free(tx);
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_learn(tx, pattern, sizeof(pattern), &learned) == GD_OK && !learned.length);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && !state.unclaimed_bytes);
    CHECK(GD_observePartition(tx, 3, &state) == GD_OK && state.unclaimed_bytes == 512);
    CHECK(GD_observePartition(tx, 5, &state) == GD_OK && state.unclaimed_bytes == 512);
    CHECK(GD_stats(tx)->payload_written == 0 && GD_blockHits(tx, 1, 0) == 0);
    size = GD_compress(tx, cc, coded, sizeof(coded), pattern, sizeof(pattern), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 1));
    GD_free(tx);

    /* A small window moves through old payload over subsequent business frames.
     * Scanned but unmatched bytes stay unclaimed; no fixed compression slices. */
    memcpy(data[1] + 2500, pattern, sizeof(pattern));
    ranges[0].length = 3500; layout.extent[1] = 3500; layout.range_count = 1;
    /* Remove the previous leading copy, retaining a byte-distinct old prefix. */
    memcpy(data[1], data[0], sizeof(pattern));
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_setUnclaimedWindow(tx, GD_MAX_FRAME + 1U) == GD_INVALID);
    CHECK(GD_setUnclaimedWindow(tx, 0) == GD_OK);
    size = GD_compress(tx, cc, coded, sizeof(coded), pattern, sizeof(pattern), &view);
    CHECK(!ZSTD_isError(size) && !view.used_mask && !GD_stats(tx)->unclaimed_scanned);
    CHECK(GD_setUnclaimedWindow(tx, 64) == GD_OK);
    for (i = 0; i < 64; ++i) {
        uint64_t const scanned = GD_stats(tx)->unclaimed_scanned;
        size = GD_compress(tx, cc, coded, sizeof(coded), pattern, sizeof(pattern), &view);
        CHECK(!ZSTD_isError(size) && GD_stats(tx)->unclaimed_scanned - scanned <= 64);
        CHECK(GD_stats(tx)->payload_written == 0);
        if (view.used_mask) break;
        CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.unclaimed_bytes == 3500);
        CHECK(state.scan_cursor == (i + 1) * 64);
    }
    CHECK(i < 64 && view.used_mask == (1U << 1));
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.unclaimed_bytes == 3500 - sizeof(pattern));
    CHECK(state.unclaimed_ranges == 2);
    CHECK(GD_rotate(tx, 0, test_epochs[0], next_epochs[GD_committed(tx, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_rotate(tx, 0, test_epochs[1], next_epochs[GD_committed(tx, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && !state.unclaimed_bytes && !state.unclaimed_ranges && !state.scan_cursor);
    CHECK(!memcmp(data[1] + 2500, pattern, sizeof(pattern)));
    GD_free(tx);
    {
        GD_Missing old_ranges[2] = {{0, test_epochs[0],0,512}, {1, test_epochs[1],0,512}};
        layout.ranges = old_ranges; layout.range_count = 2;
        layout.extent[0] = layout.extent[1] = 512;
        memcpy(data[0], pattern, sizeof(pattern)); memcpy(data[1], pattern, sizeof(pattern));
        tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
        size = GD_compress(tx, cc, coded, sizeof(coded), pattern, sizeof(pattern), &view);
        CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 0));
        CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.unclaimed_bytes == 512);
        GD_free(tx);
    }
    ZSTD_freeCCtx(cc);
}

static void test_sender_ignores_recovered_invalid_ranges(void)
{
    uint32_t capacities[3] = {CAP, CAP, CAP};
    unsigned char data[GD_PARTITIONS][CAP], added[32], output[100];
    void* buffers[GD_PARTITIONS];
    GD_Missing ranges[1] = {{5, test_epochs[5],0,100}};
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]}, {0,0,0,0,0,400}, {1,3,5}, ranges, 1, NULL, 0};
    GD_Store* tx;
    GD_Move move = {0, 0};
    uint32_t offset;
    unsigned i;
    uint64_t written;
    memset(data, 0xa6, sizeof(data)); memset(added, 0x3c, sizeof(added));
    for (i = 0; i < GD_PARTITIONS; ++i) buffers[i] = data[i];
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_append(tx, 2, added, sizeof(added), &offset) == GD_OK && offset == 400);
    CHECK(GD_read(tx, 5, test_epochs[5], 100, output, 1) == GD_MISSING);
    CHECK(GD_read(tx, 5, test_epochs[5], 399, output, 1) == GD_MISSING);
    CHECK(GD_read(tx, 5, test_epochs[5], 400, output, sizeof(added)) == GD_OK && !memcmp(output, added, sizeof(added)));
    CHECK(GD_stats(tx)->payload_written == sizeof(added));
    written = GD_stats(tx)->payload_written;
    CHECK(GD_rotate(tx, 2, test_epochs[4], next_epochs[GD_committed(tx, 2)], 3, test_epochs[3], NULL, 0) == GD_OK);
    CHECK(GD_rotate(tx, 2, test_epochs[5], next_epochs[GD_committed(tx, 2)], 3, test_epochs[3], &move, 1) == GD_OK);
    /* The existing copy operation copies only ready bytes. Invalid source
     * bytes are ignored and retire with the source; no TX repair is introduced. */
    CHECK(GD_stats(tx)->payload_written - written == 100 + sizeof(added));
    CHECK(GD_read(tx, 3, test_epochs[3], 100, output, 1) == GD_MISSING);
    CHECK(GD_read(tx, 3, test_epochs[3], 399, output, 1) == GD_MISSING);
    CHECK(GD_read(tx, 3, test_epochs[3], 400, output, sizeof(added)) == GD_OK && !memcmp(output, added, sizeof(added)));
    CHECK(data[5][100] == 0xa6 && data[5][399] == 0xa6);
    GD_free(tx);
}

static void test_dynamic_regions_and_selection(void)
{
    GD_Store *tx = GD_create(CAP, 1, test_epochs), *rx = GD_create(CAP, 0, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char payload[3][1400], frame[1500], coded[2048], output[1500];
    GD_FrameView view, wrong;
    const GD_Match* matches;
    size_t size, i, count, at, restored;
    unsigned p_frames;
    uint64_t heat, written, matched;
    uint64_t const saved_rng = rng;
    CHECK(tx && rx && cc && dc);
    rng = 0xa093fb4267758123ULL;
    random_bytes(payload, sizeof(payload)); random_bytes(frame, sizeof(frame));
    rng = saved_rng;
    for (i = 0; i < 3; ++i) {
        write_part(tx, (unsigned)(2 * i + 1), payload[i], sizeof(payload[i]));
        write_part(rx, (unsigned)(2 * i + 1), payload[i], sizeof(payload[i]));
    }
    memcpy(frame + 193, payload[0] + 73, 377);
    memcpy(frame + 587, payload[0] + 631, 151);
    memcpy(frame + 805, payload[1] + 19, 413);
    memcpy(frame + 1249, payload[2] + 67, 179);
    written = GD_stats(tx)->payload_written;
    size = GD_compress(tx, cc, coded, sizeof(coded), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == 0x2a && size < 500);
    matches = GD_matches(tx, &count);
    CHECK(count == 4 && matches[0].source_offset == 193 && matches[0].length == 377);
    CHECK(matches[1].source_offset == 587 && matches[1].length == 151);
    CHECK(matches[2].partition == 3 && matches[2].length == 413);
    CHECK(matches[3].partition == 5 && matches[3].length == 179);
    CHECK(GD_stats(tx)->payload_written == written);
    at = restored = 0; p_frames = 0;
    while (at < size) {
        ZSTD_frameHeader h;
        size_t n = ZSTD_findFrameCompressedSize(coded + at, size - at);
        CHECK(!ZSTD_isError(n) && !ZSTD_getFrameHeader(&h, coded + at, n));
        if (h.dictID) CHECK(n * 100 <= h.frameContentSize * 50);
        if (h.dictID == GD_DICTIONARY_ID_BASE + 1) {
            /* Includes two matches and the 17-byte business gap, exceeding
             * the longest individual match; no cut at a storage/block grid. */
            CHECK(restored == 193 && h.frameContentSize == 545); ++p_frames;
        }
        restored += (size_t)h.frameContentSize; at += n;
    }
    CHECK(p_frames == 1 && restored == sizeof(frame));
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(frame));
    CHECK(!memcmp(output, frame, sizeof(frame)));
    wrong = view; wrong.used_mask &= ~(1U << 3);
    CHECK(ZSTD_isError(GD_decompress(rx, dc, output, sizeof(output), coded, size, &wrong)));
    wrong = view; wrong.used_mask |= 1U << 0;
    CHECK(ZSTD_isError(GD_decompress(rx, dc, output, sizeof(output), coded, size, &wrong)));
    CHECK(ZSTD_isError(GD_decompress(rx, dc, output, sizeof(output) - 1, coded, size, &view)));
    for (i = 1; i < size; ++i) {
        size_t const n = GD_decompress(rx, dc, output, sizeof(output), coded, i, &view);
        /* A complete prefix of native frames may decode, but the business
         * owner must still enforce its advertised complete-frame length. */
        CHECK(ZSTD_isError(n) || n < sizeof(frame));
    }

    /* Several disconnected high-gain regions can use the same half. */
    memcpy(frame + 587, payload[1] + 631, 151);
    memcpy(frame + 1249, payload[0] + 631, 151);
    size = GD_compress(tx, cc, coded, sizeof(coded), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size));
    at = 0; p_frames = 0;
    while (at < size) {
        size_t n = ZSTD_findFrameCompressedSize(coded + at, size - at);
        CHECK(!ZSTD_isError(n));
        p_frames += ZSTD_getDictID_fromFrame(coded + at, n) == GD_DICTIONARY_ID_BASE + 1;
        at += n;
    }
    CHECK(p_frames == 2);
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(frame));
    CHECK(!memcmp(output, frame, sizeof(frame)));

    /* Threshold changes affect selected output; discarded/re-encoded plans
     * never create independent reuse or new dictionary payload. */
    heat = GD_blockHits(tx, 1, 0); matched = GD_stats(tx)->matched_bytes[0];
    size = GD_compress(tx, cc, coded, 1, frame, sizeof(frame), &view);
    CHECK(ZSTD_isError(size) && !view.used_mask);
    GD_matches(tx, &count); CHECK(!count);
    CHECK(GD_blockHits(tx, 1, 0) == heat && GD_stats(tx)->matched_bytes[0] == matched);
    CHECK(GD_setSegmentRatio(tx, 0) == GD_INVALID && GD_setSegmentRatio(tx, 101) == GD_INVALID);
    size = GD_compress(tx, cc, coded, sizeof(coded), payload[0] + 73, 32, &view);
    CHECK(!ZSTD_isError(size) && !view.used_mask && GD_blockHits(tx, 1, 0) == heat);
    CHECK(GD_setSegmentRatio(tx, 100) == GD_OK);
    size = GD_compressTracked(tx, cc, coded, sizeof(coded), payload[0] + 73, 32, &view, 0);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 1) && size <= 32);
    CHECK(GD_blockHits(tx, 1, 0) == heat);
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == 32);
    CHECK(!memcmp(output, payload[0] + 73, 32));
    CHECK(GD_stats(tx)->payload_written == written);
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx); GD_free(rx);
}

static void test_large_local_capacities(void)
{
    const uint32_t capacities[3] = {250000000, 200000000, 50000000};
    GD_Store *tx = GD_createWithCapacities(capacities, 1, test_epochs), *rx = GD_createWithCapacities(capacities, 0, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char data[3][1024], frame[1200], coded[1600], output[1200];
    GD_FrameView view;
    size_t i, size;
    uint64_t const saved_rng = rng;
    CHECK(tx && rx && cc && dc);
    rng = 0x254adcfdd4a317d9ULL; random_bytes(data, sizeof(data)); rng = saved_rng;
    for (i = 0; i < 3; ++i) {
        write_part(tx, (unsigned)(2 * i + 1), data[i], sizeof(data[i]));
        write_part(rx, (unsigned)(2 * i + 1), data[i], sizeof(data[i]));
        memcpy(frame + 400 * i, data[i] + 64, 400);
    }
    CHECK(GD_stats(tx)->payload_allocated == 1000000000 && GD_stats(rx)->payload_allocated == 1000000000);
    size = GD_compress(tx, cc, coded, sizeof(coded), frame, sizeof(frame), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == 0x2a);
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(frame));
    CHECK(!memcmp(output, frame, sizeof(frame)));
    printf("{\"local_half_capacities\":[250000000,200000000,50000000],\"tx_index_bytes\":%llu,\"tx_metadata_bytes\":%llu,\"encoded_bytes\":%zu}\n",
        (unsigned long long)GD_stats(tx)->index_allocated, (unsigned long long)GD_stats(tx)->metadata_allocated, size);
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx); GD_free(rx);
}

static void test_store_address_isolation(void)
{
    const uint32_t tx_capacity[3] = {CAP * 2, CAP, CAP};
    const uint32_t rx_capacity[3] = {CAP * 3, CAP, CAP};
    GD_Store* tx = GD_createWithCapacities(tx_capacity, 1, test_epochs);
    GD_Store* rx = GD_createWithCapacities(rx_capacity, 0, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char data[1300], coded[2048], output[383];
    GD_FrameView view;
    size_t size;
    uint64_t const saved_rng = rng;
    CHECK(tx && rx && cc && dc);
    rng = 0x3389adcfd93a50e1ULL; random_bytes(data, sizeof(data)); rng = saved_rng;
    write_part(tx, 3, data, sizeof(data)); write_part(rx, 3, data, sizeof(data));
    size = GD_compress(tx, cc, coded, sizeof(coded), data + 113, sizeof(output), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 3));
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(output));
    CHECK(!memcmp(output, data + 113, sizeof(output)));
    CHECK(GD_rotate(rx, 0, GD_epoch(rx, 0), next_epochs[GD_committed(rx, 0)], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(output));
    CHECK(!memcmp(output, data + 113, sizeof(output)));
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx); GD_free(rx);
}

static void test_independent_lifetimes_reject_old_frames(void)
{
    GD_Store* tx = GD_create(CAP, 1, test_epochs);
    GD_Store* restarted_rx = GD_create(CAP, 0, next_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char data[1000], replacement[1000], coded[2048], output[1000];
    uint64_t const saved_rng = rng;
    GD_FrameView view;
    size_t size;
    CHECK(tx && restarted_rx && cc && dc);
    random_bytes(data, sizeof(data)); random_bytes(replacement, sizeof(replacement)); rng = saved_rng;
    write_part(tx, 1, data, sizeof(data));
    write_part(restarted_rx, 1, replacement, sizeof(replacement));
    size = GD_compress(tx, cc, coded, sizeof(coded), data, sizeof(data), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 1));
    CHECK(ZSTD_isError(GD_decompress(restarted_rx, dc, output, sizeof(output), coded, size, &view)));
    CHECK(GD_lastResult(restarted_rx) == GD_STALE);
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx); GD_free(restarted_rx);
}

static void test_uuid_checks_and_rotation(void)
{
    GD_Epoch epochs[GD_PARTITIONS], replacement = fixture_newEpoch();
    GD_Store *tx, *rx;
    ZSTD_CCtx* cc = ZSTD_createCCtx(); ZSTD_DCtx* dc = ZSTD_createDCtx();
    unsigned char data[1000], coded[2048], output[1000];
    GD_FrameView view, wrong;
    GD_Move keep = {0, 0};
    size_t size;
    unsigned i;
    uint64_t const saved_rng = rng;
    uint64_t written;
    memcpy(epochs, test_epochs, sizeof(epochs));
    epochs[1].bytes[0] = 0xfe; replacement.bytes[0] = 1;
    CHECK(!GD_create(CAP, 1, NULL));
    epochs[3] = GD_NO_EPOCH;
    CHECK(!GD_create(CAP, 1, epochs));
    epochs[3] = test_epochs[3];
    tx = GD_create(CAP, 1, epochs); rx = GD_create(CAP, 0, epochs);
    CHECK(tx && rx && cc && dc);
    random_bytes(data, sizeof(data)); rng = saved_rng;
    write_part(tx, 1, data, sizeof(data)); write_part(rx, 1, data, sizeof(data));
    size = GD_compress(tx, cc, coded, sizeof(coded), data, sizeof(data), &view);
    CHECK(!ZSTD_isError(size) && view.used_mask == (1U << 1));
    for (i = 0; i < sizeof(epochs[1].bytes); ++i) {
        wrong = view; wrong.epoch[1].bytes[i] ^= 1;
        memset(output, 0xa9, sizeof(output));
        CHECK(ZSTD_isError(GD_decompress(rx, dc, output, sizeof(output), coded, size, &wrong)));
        CHECK(GD_lastResult(rx) == GD_STALE && output[0] == 0xa9 && output[sizeof(output)-1] == 0xa9);
        CHECK(GD_read(rx, 1, wrong.epoch[1], 0, output, sizeof(output)) == GD_STALE);
        CHECK(GD_write(rx, 1, wrong.epoch[1], 0, data, sizeof(data)) == GD_STALE);
    }
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(data));
    CHECK(!memcmp(output, data, sizeof(data)));
    CHECK(GD_rotate(tx, 0, epochs[0], next_epochs[0], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 0, epochs[0], next_epochs[0], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    written = GD_stats(tx)->payload_written;
    CHECK(GD_rotate(tx, 0, epochs[1], GD_NO_EPOCH, 0, next_epochs[0], &keep, 1) == GD_INVALID);
    CHECK(GD_rotate(tx, 0, epochs[1], epochs[1], 0, next_epochs[0], &keep, 1) == GD_INVALID);
    CHECK(GD_stats(tx)->payload_written == written && GD_prepare(tx, 0) == 0);
    CHECK(GD_read(tx, 1, epochs[1], 0, output, sizeof(output)) == GD_OK && !memcmp(output, data, sizeof(data)));
    CHECK(GD_read(tx, 0, next_epochs[0], 0, output, 1) == GD_MISSING);
    /* A lexicographically lower replacement is an unrelated UUID, not old state. */
    CHECK(GD_rotate(tx, 0, epochs[1], replacement, 0, next_epochs[0], &keep, 1) == GD_OK);
    CHECK(GD_rotate(rx, 0, epochs[1], replacement, 0, next_epochs[0], &keep, 1) == GD_OK);
    CHECK(GD_rotate(rx, 0, epochs[1], replacement, 0, next_epochs[0], &keep, 1) == GD_STALE);
    CHECK(GD_write(rx, 1, epochs[1], 0, data, sizeof(data)) == GD_STALE);
    CHECK(GD_read(rx, 1, replacement, 0, output, 1) == GD_MISSING);
    CHECK(GD_read(rx, 0, next_epochs[0], 0, output, sizeof(output)) == GD_OK && !memcmp(output, data, sizeof(data)));
    CHECK(GD_write(tx, 1, replacement, 0, data, sizeof(data)) == GD_OK);
    CHECK(GD_write(rx, 1, replacement, 0, data, sizeof(data)) == GD_OK);
    CHECK(ZSTD_isError(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view)) && GD_lastResult(rx) == GD_STALE);
    size = GD_compress(tx, cc, coded, sizeof(coded), data, sizeof(data), &view);
    CHECK(!ZSTD_isError(size));
    CHECK(GD_decompress(rx, dc, output, sizeof(output), coded, size, &view) == sizeof(data) && !memcmp(output, data, sizeof(data)));
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc); GD_free(tx); GD_free(rx);
}

static void test_heat_decay_changes_retention(void)
{
    unsigned char data[CAP], coded[2048];
    GD_Store* tx = GD_create(CAP, 1, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    GD_FrameView view;
    GD_Move selected[2];
    GD_PartitionStats state;
    double first, second;
    uint64_t const saved_rng = rng;
    CHECK(tx && cc);
    random_bytes(data, sizeof(data)); rng = saved_rng;
    write_part(tx, 5, data, sizeof(data));
    CHECK(GD_setHeatPolicy(tx, 10, 0) == GD_OK);
    GD_setTime(tx, 100);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data + GD_BLOCK_SIZE - 1000, 1000, &view)) && view.used_mask == 32);
    first = (double)GD_stats(tx)->matched_bytes[2]; CHECK(first > 900);
    GD_setTime(tx, 110);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data + CAP - 600, 600, &view)) && view.used_mask == 32);
    second = (double)GD_stats(tx)->matched_bytes[2] - first; CHECK(second > 550);
    CHECK(GD_rotate(tx, 2, test_epochs[4], next_epochs[4], 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE, selected, 1) == 1 && selected[0].source_block == 1);
    CHECK(GD_observePartition(tx, 5, &state) == GD_OK && fabs(state.heat - (first / 2 + second)) < 0.001);
    GD_setTime(tx, 105); /* Wall-clock rollback does not reheat old traffic. */
    CHECK(GD_observePartition(tx, 5, &state) == GD_OK && fabs(state.heat - (first / 2 + second)) < 0.001);
    GD_setTime(tx, 120);
    CHECK(GD_setHeatPolicy(tx, 10, 75) == GD_OK);
    CHECK(GD_selectMoves(tx, 2, 0, 2 * GD_BLOCK_SIZE, selected, 2) == 0); /* Both below 0.075*4096. */
    CHECK(GD_setHeatPolicy(tx, 10, 50) == GD_OK);
    CHECK(GD_selectMoves(tx, 2, 0, 2 * GD_BLOCK_SIZE, selected, 2) == 2);
    CHECK(GD_setHeatPolicy(tx, 0, 0) == GD_INVALID);
    CHECK(GD_setHeatPolicy(tx, 20, 0) == GD_OK); /* Rebase under old T first. */
    GD_setTime(tx, 140);
    CHECK(GD_observePartition(tx, 5, &state) == GD_OK && fabs(state.heat - (first / 8 + second / 4)) < 0.001);
    CHECK(GD_blockHits(tx, 5, 0) == 1 && GD_blockHits(tx, 5, 1) == 1);
    CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE, selected, 1) == 1 && selected[0].source_block == 1);
    CHECK(GD_rotate(tx, 2, test_epochs[5], next_epochs[5], 3, test_epochs[3], selected, 1) == GD_OK);
    CHECK(GD_observePartition(tx, 3, &state) == GD_OK && fabs(state.heat - second / 4) < 0.001);
    GD_setTime(tx, 160);
    CHECK(GD_observePartition(tx, 3, &state) == GD_OK && fabs(state.heat - second / 8) < 0.001);
    CHECK(GD_blockHits(tx, 3, 0) == 1 && (double)GD_stats(tx)->matched_bytes[2] == first + second);
    /* A zero threshold still cannot promote never-used payload. */
    CHECK(GD_rotate(tx, 2, next_epochs[4], fixture_newEpoch(), 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_selectMoves(tx, 2, 0, 2 * GD_BLOCK_SIZE, selected, 2) == 0);
    ZSTD_freeCCtx(cc); GD_free(tx);
}

static void test_snapshot_partial_mismatch_and_invalid_sections(void)
{
    uint32_t capacities[3] = {CAP, CAP, CAP};
    unsigned char payloads[GD_PARTITIONS][CAP], expected[CAP], coded[2048];
    void* buffers[GD_PARTITIONS];
    GD_Missing range = {1, test_epochs[1], 0, CAP};
    GD_IndexSnapshot indexes[GD_PARTITIONS] = {{0}};
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]},
                        {0,CAP,0,0,0,0}, {1,3,5}, &range, 1, indexes, 100};
    GD_Store* tx;
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    GD_FrameView view;
    GD_PartitionStats state;
    unsigned char *snapshot, *bad;
    uint64_t const saved_rng = rng;
    uint64_t first, second;
    size_t size, written;
    unsigned i;
    uint32_t cursor;
    random_bytes(payloads, sizeof(payloads)); rng = saved_rng;
    for (i = 0; i < GD_PARTITIONS; ++i) buffers[i] = payloads[i];
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx && cc);
    CHECK(GD_setHeatPolicy(tx, 10, 0) == GD_OK && GD_setUnclaimedWindow(tx, CAP) == GD_OK);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), payloads[1] + 200, 400, &view)) && view.used_mask == 2);
    first = GD_stats(tx)->matched_bytes[0];
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), payloads[1] + GD_BLOCK_SIZE + 200, 400, &view)) && view.used_mask == 2);
    second = GD_stats(tx)->matched_bytes[0] - first;
    CHECK(first == 400 && second == 400);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK); cursor = state.scan_cursor;
    GD_setTime(tx, 110); /* Saving folds elapsed time even without another hit. */
    size = GD_indexSnapshotSize(tx, 1); CHECK(size);
    snapshot = malloc(size); bad = malloc(size); CHECK(snapshot && bad);
    memset(snapshot, 0xa7, size);
    CHECK(GD_saveIndex(tx, 1, snapshot, size - 1, &written) == GD_CAPACITY && !written && snapshot[0] == 0xa7);
    CHECK(GD_saveIndex(tx, 1, snapshot, size, &written) == GD_OK && written == size);
    GD_free(tx);
    indexes[1] = (GD_IndexSnapshot){snapshot, size}; layout.now = 120;
    payloads[1][220] ^= 1; /* External change after snapshot publication. */
    memcpy(expected, payloads[1], CAP);
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.recovery.index_result == GD_OK);
    CHECK(state.recovery.checksum_failures == 1 && state.recovery.checksum_bytes == CAP);
    CHECK(state.unclaimed_bytes == CAP - 400 && state.scan_cursor == cursor);
    CHECK(fabs(state.heat - second / 4.0) < 0.001 && GD_blockHits(tx, 1, 0) == 0 && GD_blockHits(tx, 1, 1) == 1);
    CHECK(GD_setUnclaimedWindow(tx, 0) == GD_OK);
    CHECK(!ZSTD_isError(GD_compressTracked(tx, cc, coded, sizeof(coded), payloads[1] + 200, 400, &view, 0)) && !view.used_mask);
    CHECK(!ZSTD_isError(GD_compressTracked(tx, cc, coded, sizeof(coded), payloads[1] + GD_BLOCK_SIZE + 200, 400, &view, 0)) && view.used_mask == 2);
    CHECK(!GD_stats(tx)->payload_written && !GD_stats(tx)->payload_recognized);
    CHECK(GD_setHeatPolicy(tx, 20, 0) == GD_OK);
    GD_setTime(tx, 140);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && fabs(state.heat - second / 8.0) < 0.001);
    CHECK(GD_setUnclaimedWindow(tx, CAP) == GD_OK);
    CHECK(!ZSTD_isError(GD_compressTracked(tx, cc, coded, sizeof(coded), payloads[1] + 200, 400, &view, 0)) && view.used_mask == 2);
    CHECK(GD_stats(tx)->payload_recognized == 400 && !GD_stats(tx)->payload_written);
    GD_free(tx);
    CHECK(!memcmp(payloads[1], expected, CAP));
    layout.now = 90;
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && fabs(state.heat - second / 2.0) < 0.001);
    CHECK(GD_setHeatPolicy(tx, 10, 0) == GD_OK);
    GD_setTime(tx, 105);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && fabs(state.heat - second / 2.0) < 0.001);
    GD_setTime(tx, 120);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && fabs(state.heat - second / 4.0) < 0.001);
    GD_free(tx); layout.now = 120;
    /* Older authoritative metadata can confirm less than the saved index.
     * Keep the still-valid prefix without inferring readiness for the tail. */
    payloads[1][220] ^= 1;
    range.length = layout.extent[1] = GD_BLOCK_SIZE;
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.recovery.index_result == GD_OK);
    CHECK(state.recovery.checksum_failures == 1 && state.unclaimed_bytes == GD_BLOCK_SIZE - 400);
    CHECK(fabs(state.heat - first / 4.0) < 0.001 && state.scan_cursor <= GD_BLOCK_SIZE);
    CHECK(GD_setUnclaimedWindow(tx, 0) == GD_OK);
    CHECK(!ZSTD_isError(GD_compressTracked(tx, cc, coded, sizeof(coded), payloads[1] + 200, 400, &view, 0)) && view.used_mask == 2);
    CHECK(GD_read(tx, 1, test_epochs[1], GD_BLOCK_SIZE, coded, 1) == GD_MISSING);
    GD_free(tx);
    payloads[1][220] ^= 1; range.length = layout.extent[1] = CAP;
    /* Parse failures adopt no index or heat, even when the payload is valid.
     * These fixed-format mutations test version, count and offset admission. */
    for (i = 0; i < 10; ++i) {
        memcpy(bad, snapshot, size); indexes[1] = (GD_IndexSnapshot){bad, size};
        if (i == 0) --indexes[1].size;
        if (i == 1) bad[7] = 2;
        if (i == 2) bad[size - 1] ^= 1;
        if (i == 3) bad[16] ^= 1;
        if (i == 4) memset(bad + 56, 0xff, 4);
        if (i == 5) memset(bad + 32, 0xff, 4);
        if (i == 6) memset(bad + 60, 0xff, 4);
        if (i >= 7) {
            size_t const entries = ((size_t)1 << MEM_readLE32(snapshot + 40)) * MEM_readLE32(snapshot + 44);
            unsigned char* const block = bad + 68 + entries * 6;
            if (i == 7) MEM_writeLE64(block + 12, UINT64_C(0x7ff8000000000000)); /* NaN heat. */
            if (i == 8) MEM_writeLE32(block + 8, GD_BLOCK_SIZE + 1);
            if (i == 9) MEM_writeLE32(bad + 68, CAP + 1); /* Index outside saved extent. */
            MEM_writeLE64(bad + size - 8, XXH64(bad, size - 8, 0));
        }
        tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
        CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.recovery.index_result == (i == 3 ? GD_STALE : GD_INVALID));
        CHECK(!state.recovery.restored_positions && state.unclaimed_bytes == CAP && !state.heat);
        CHECK(!GD_blockHits(tx, 1, 0) && !GD_blockHits(tx, 1, 1) && !GD_stats(tx)->payload_written);
        CHECK(GD_setUnclaimedWindow(tx, 0) == GD_OK);
        CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), payloads[1] + GD_BLOCK_SIZE + 200, 400, &view)) && !view.used_mask);
        GD_free(tx);
        CHECK(!memcmp(payloads[1], expected, CAP));
    }
    free(snapshot); free(bad); ZSTD_freeCCtx(cc);
}

static void test_snapshot_preserves_old_index_after_append(void)
{
    uint32_t capacities[3] = {CAP, CAP, CAP};
    unsigned char payloads[GD_PARTITIONS][CAP], original[4000], added[1500], coded[2048];
    void* buffers[GD_PARTITIONS];
    GD_Missing range = {1, test_epochs[1], 0, sizeof(original) + sizeof(added)};
    GD_IndexSnapshot indexes[GD_PARTITIONS] = {{0}};
    GD_Layout layout = {{test_epochs[0],test_epochs[1],test_epochs[2],test_epochs[3],test_epochs[4],test_epochs[5]},
                        {0}, {1,3,5}, NULL, 0, NULL, 0};
    GD_Store* tx;
    GD_PartitionStats state;
    GD_FrameView view;
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    unsigned char* snapshot;
    uint64_t const saved_rng = rng;
    uint32_t offset;
    size_t size, written;
    unsigned i;
    random_bytes(original, sizeof(original)); random_bytes(added, sizeof(added)); rng = saved_rng;
    for (i = 0; i < GD_PARTITIONS; ++i) buffers[i] = payloads[i];
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx && cc);
    CHECK(GD_append(tx, 0, original, sizeof(original), &offset) == GD_OK);
    size = GD_indexSnapshotSize(tx, 1); snapshot = malloc(size); CHECK(snapshot && size);
    CHECK(GD_saveIndex(tx, 1, snapshot, size, &written) == GD_OK && written == size);
    CHECK(GD_append(tx, 0, added, sizeof(added), &offset) == GD_OK && offset == sizeof(original));
    GD_free(tx);
    indexes[1] = (GD_IndexSnapshot){snapshot, size};
    layout.indexes = indexes; layout.extent[1] = range.length; layout.ranges = &range; layout.range_count = 1;
    tx = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(tx);
    CHECK(GD_observePartition(tx, 1, &state) == GD_OK && state.recovery.index_result == GD_OK);
    CHECK(!state.recovery.checksum_failures && state.unclaimed_bytes == sizeof(added));
    CHECK(state.recovery.checksum_bytes == sizeof(original));
    CHECK(GD_setUnclaimedWindow(tx, 0) == GD_OK);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), original + 3600, 300, &view)) && view.used_mask == 2);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), added, sizeof(added), &view)) && !view.used_mask);
    CHECK(!GD_stats(tx)->payload_written && !memcmp(payloads[1], original, sizeof(original)));
    CHECK(!memcmp(payloads[1] + sizeof(original), added, sizeof(added)));
    free(snapshot); GD_free(tx); ZSTD_freeCCtx(cc);
}

static void test_export_validity_ranges(void)
{
    uint32_t capacities[3] = {CAP + 13, CAP + 13, CAP + 13};
    unsigned char payloads[GD_PARTITIONS][CAP + 13];
    void* buffers[GD_PARTITIONS];
    GD_Missing input[] = {
        {1, {{0}}, 17, GD_BLOCK_SIZE + 33},
        {1, {{0}}, GD_BLOCK_SIZE + 80, 220},
        {3, {{0}}, 0, 400}, {3, {{0}}, 400, 400},
        {5, {{0}}, CAP + 12, 1}
    };
    GD_Missing output[5];
    GD_Layout layout = {{{{0}}}, {0,CAP,0,1000,0,CAP+13}, {1,3,5}, input, 5, NULL, 0};
    GD_Store* store;
    size_t count;
    unsigned i;
    memset(payloads, 0xa7, sizeof(payloads));
    for (i = 0; i < GD_PARTITIONS; ++i) { buffers[i] = payloads[i]; layout.epoch[i] = test_epochs[i]; }
    for (i = 0; i < 5; ++i) input[i].epoch = test_epochs[input[i].partition];
    store = GD_createWithBuffers(capacities, buffers, 1, &layout); CHECK(store);
    CHECK(GD_exportRanges(store, NULL, 0, &count) == GD_CAPACITY && count == 4);
    CHECK(GD_exportRanges(store, output, 3, &count) == GD_CAPACITY && count == 4);
    CHECK(GD_exportRanges(store, output, 5, &count) == GD_OK && count == 4);
    CHECK(output[0].partition == 1 && output[0].offset == 17 && output[0].length == GD_BLOCK_SIZE + 33);
    CHECK(output[1].partition == 1 && output[1].offset == GD_BLOCK_SIZE + 80 && output[1].length == 220);
    CHECK(output[2].partition == 3 && output[2].offset == 0 && output[2].length == 800);
    CHECK(output[3].partition == 5 && output[3].offset == CAP + 12 && output[3].length == 1);
    for (i = 0; i < count; ++i) CHECK(GD_epochEqual(output[i].epoch, test_epochs[output[i].partition]));
    CHECK(!GD_stats(store)->payload_written && !GD_stats(store)->payload_recognized);
    GD_free(store);
    store = GD_create(CAP, 1, test_epochs); CHECK(store);
    CHECK(GD_exportRanges(store, NULL, 0, &count) == GD_OK && !count);
    GD_free(store);
}

static void test_receiver_layout_without_payload(void)
{
    uint32_t capacities[3] = {CAP, CAP, CAP};
    GD_Layout layout = {{{{0}}}, {0,700,0,0,0,0}, {1,3,5}, NULL, 0, NULL, 0};
    GD_Store* rx;
    GD_PartitionStats state;
    unsigned char input[32], output[32];
    GD_Missing claimed;
    size_t count;
    unsigned p;
    for (p = 0; p < GD_PARTITIONS; ++p) layout.epoch[p] = test_epochs[p];
    rx = GD_createWithBuffers(capacities, NULL, 0, &layout); CHECK(rx);
    CHECK(GD_extent(rx, 1) == 700);
    CHECK(GD_read(rx, 1, test_epochs[1], 17, output, sizeof(output)) == GD_MISSING);
    CHECK(GD_exportRanges(rx, NULL, 0, &count) == GD_OK && !count);
    memset(input, 0x6d, sizeof(input));
    CHECK(GD_write(rx, 1, test_epochs[1], 17, input, sizeof(input)) == GD_OK);
    CHECK(GD_read(rx, 1, test_epochs[1], 17, output, sizeof(output)) == GD_OK && !memcmp(input, output, sizeof(input)));
    CHECK(GD_exportRanges(rx, &claimed, 1, &count) == GD_OK && count == 1 && claimed.offset == 17 && claimed.length == sizeof(input));
    CHECK(GD_observePartition(rx, 1, &state) == GD_OK && state.extent == 700 && state.present_bytes == sizeof(input));
    CHECK(!state.unclaimed_bytes && !state.heat && !GD_stats(rx)->index_allocated && !GD_stats(rx)->indexed_positions);
    GD_free(rx);
    /* A sender cannot invent learned payload from an extent announcement. */
    CHECK(!GD_createWithBuffers(capacities, NULL, 1, &layout));
    layout.ranges = &claimed; layout.range_count = 1;
    CHECK(!GD_createWithBuffers(capacities, NULL, 0, &layout));
}

static int move_by_source(const void* left, const void* right)
{
    const GD_Move *a = (const GD_Move*)left, *b = (const GD_Move*)right;
    return (a->source_block > b->source_block) - (a->source_block < b->source_block);
}

static void test_retention_physical_tail(void)
{
    uint32_t capacities[3] = {32 * GD_BLOCK_SIZE, 18 * GD_BLOCK_SIZE, 4 * GD_BLOCK_SIZE + GD_BLOCK_SIZE / 2};
    unsigned char data[4 * GD_BLOCK_SIZE + GD_BLOCK_SIZE / 2], coded[2 * GD_BLOCK_SIZE], decoded[sizeof(data)];
    uint64_t const saved_rng = rng;
    unsigned trial;
    random_bytes(data, sizeof(data));
    rng = saved_rng;
    for (trial = 0; trial < 2; ++trial) {
        GD_Store *tx = GD_createWithCapacities(capacities, 1, test_epochs);
        GD_Store *rx = GD_createWithCapacities(capacities, 0, test_epochs);
        ZSTD_CCtx* cc = ZSTD_createCCtx();
        GD_Move moves[9];
        GD_FrameView view;
        GD_Missing appended;
        GD_PartitionStats state;
        GD_Epoch replacement = fixture_newEpoch();
        uint32_t offset, required;
        size_t at, count;
        CHECK(tx && rx && cc);
        CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK && !offset);
        CHECK(GD_write(rx, 5, test_epochs[5], 0, data, 4 * GD_BLOCK_SIZE) == GD_OK);
        CHECK(GD_rotate(tx, 2, test_epochs[4], replacement, 3, test_epochs[3], NULL, 0) == GD_OK);
        CHECK(GD_rotate(rx, 2, test_epochs[4], replacement, 3, test_epochs[3], NULL, 0) == GD_OK);
        for (at = 0; at < sizeof(data); at += GD_BLOCK_SIZE) {
            size_t const length = sizeof(data) - at < GD_BLOCK_SIZE ? sizeof(data) - at : GD_BLOCK_SIZE;
            CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data + at, length, &view)) && view.used_mask == 32);
        }
        CHECK(GD_setHeatPolicy(tx, 86400, 1000) == GD_OK);
        CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE / 2, moves, 9) == 1 && moves[0].source_block == 4);
        CHECK(GD_setHeatPolicy(tx, 86400, 0) == GD_OK);
        CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE / 2 - 1, moves, 9) == 0);
        CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE / 2, moves, 9) == 1 && moves[0].source_block == 4);
        CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data + 4 * GD_BLOCK_SIZE, GD_BLOCK_SIZE / 2, &view)) && view.used_mask == 32);
        CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE, moves, 1) == 1 && moves[0].source_block == 4);
        CHECK(GD_selectMoves(tx, 2, 0, GD_BLOCK_SIZE, moves, 9) == 1 && moves[0].source_block == 4);
        count = GD_selectMoves(tx, 2, 0, capacities[1] / (trial ? 2 : 4), moves, 9);
        CHECK(count == 5);
        qsort(moves, count, sizeof(*moves), move_by_source);
        CHECK(GD_compactMoves(tx, 2, test_epochs[5], 3, test_epochs[3], capacities[1], moves, &count, &appended, &required) == GD_OK);
        CHECK(count == 5 && !appended.length && required == sizeof(data));
        replacement = fixture_newEpoch();
        CHECK(GD_rotate(tx, 2, test_epochs[5], replacement, 3, test_epochs[3], moves, count) == GD_OK);
        CHECK(GD_rotate(rx, 2, test_epochs[5], replacement, 3, test_epochs[3], moves, count) == GD_OK);
        CHECK(GD_extent(tx, 3) == sizeof(data) && GD_extent(rx, 3) == sizeof(data));
        CHECK(GD_read(tx, 3, test_epochs[3], 0, decoded, sizeof(decoded)) == GD_OK && !memcmp(data, decoded, sizeof(data)));
        CHECK(GD_read(rx, 3, test_epochs[3], 0, decoded, 4 * GD_BLOCK_SIZE) == GD_OK && !memcmp(data, decoded, 4 * GD_BLOCK_SIZE));
        CHECK(GD_read(rx, 3, test_epochs[3], 4 * GD_BLOCK_SIZE, decoded, GD_BLOCK_SIZE / 2) == GD_MISSING);
        CHECK(GD_read(rx, 5, test_epochs[5], 0, decoded, 1) == GD_STALE);
        CHECK(GD_write(rx, 3, test_epochs[3], 4 * GD_BLOCK_SIZE, data + 4 * GD_BLOCK_SIZE, GD_BLOCK_SIZE / 2) == GD_OK);
        CHECK(GD_read(rx, 3, test_epochs[3], 0, decoded, sizeof(decoded)) == GD_OK && !memcmp(data, decoded, sizeof(data)));
        CHECK(GD_observePartition(tx, 3, &state) == GD_OK && state.present_bytes == sizeof(data));
        CHECK(!GD_stats(rx)->index_allocated && !GD_stats(rx)->indexed_positions);
        CHECK(GD_stats(tx)->payload_transferred == sizeof(data) && !GD_stats(tx)->transferred_padding);
        GD_free(tx); GD_free(rx); ZSTD_freeCCtx(cc);
    }
}

static void test_tail_merge_does_not_expand_budget(void)
{
    uint32_t capacities[3] = {8 * GD_BLOCK_SIZE, 4 * GD_BLOCK_SIZE, 2 * GD_BLOCK_SIZE + 128};
    unsigned char data[2 * GD_BLOCK_SIZE + 128], united[GD_BLOCK_SIZE + 64], coded[2 * GD_BLOCK_SIZE];
    GD_Store* tx = GD_createWithCapacities(capacities, 1, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    GD_Move moves[3];
    GD_FrameView view;
    GD_Missing appended;
    uint64_t const saved_rng = rng;
    uint64_t written;
    uint32_t offset, required;
    size_t at, count;
    CHECK(tx && cc);
    random_bytes(data, sizeof(data)); random_bytes(united, sizeof(united)); rng = saved_rng;
    memcpy(data + GD_BLOCK_SIZE, united, GD_BLOCK_SIZE);
    memcpy(data + 2 * GD_BLOCK_SIZE, united + GD_BLOCK_SIZE - 64, 128);
    CHECK(GD_append(tx, 2, data, sizeof(data), &offset) == GD_OK);
    CHECK(GD_rotate(tx, 2, test_epochs[4], fixture_newEpoch(), 3, test_epochs[3], NULL, 0) == GD_OK);
    for (at = 0; at < sizeof(data); at += GD_BLOCK_SIZE) {
        size_t const length = sizeof(data) - at < GD_BLOCK_SIZE ? sizeof(data) - at : GD_BLOCK_SIZE;
        CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data + at, length, &view)) && view.used_mask == 32);
    }
    count = GD_selectMoves(tx, 2, 0, sizeof(data), moves, 3);
    CHECK(count == 3);
    qsort(moves, count, sizeof(*moves), move_by_source);
    written = GD_stats(tx)->payload_written;
    CHECK(GD_compactMoves(tx, 2, test_epochs[5], 3, test_epochs[3], capacities[1], moves, &count, &appended, &required) == GD_OK);
    CHECK(count == 3 && !appended.length && required == sizeof(data));
    CHECK(GD_stats(tx)->payload_written == written);
    CHECK(GD_rotate(tx, 2, test_epochs[5], fixture_newEpoch(), 3, test_epochs[3], moves, count) == GD_OK);
    CHECK(GD_extent(tx, 3) == sizeof(data));
    GD_free(tx); ZSTD_freeCCtx(cc);
}

static void test_copy_batches_keep_source_until_retire(void)
{
    uint32_t capacities[3] = {4 * GD_BLOCK_SIZE + 128, CAP, CAP};
    unsigned char data[4 * GD_BLOCK_SIZE + 128], prefix[2 * GD_BLOCK_SIZE], coded[2 * GD_BLOCK_SIZE], output[GD_BLOCK_SIZE];
    GD_Store *tx = GD_createWithCapacities(capacities, 1, test_epochs);
    GD_Store *rx = GD_createWithCapacities(capacities, 0, test_epochs);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    GD_Move first = {0, 2 * GD_BLOCK_SIZE}, tail = {4, 3 * GD_BLOCK_SIZE};
    GD_Move invalid[] = {{2, 3 * GD_BLOCK_SIZE}, {2, 4 * GD_BLOCK_SIZE}};
    GD_Epoch const target = fixture_newEpoch(), replacement = fixture_newEpoch();
    GD_FrameView view;
    GD_PartitionStats source_state, target_state;
    uint64_t const saved_rng = rng;
    uint64_t written;
    uint32_t offset;
    CHECK(tx && rx && cc);
    random_bytes(data, sizeof(data)); random_bytes(prefix, sizeof(prefix)); rng = saved_rng;
    CHECK(GD_setHeatPolicy(tx, 10, 0) == GD_OK); GD_setTime(tx, 100);
    CHECK(GD_append(tx, 0, data, sizeof(data), &offset) == GD_OK);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view)) && view.used_mask == 2);
    CHECK(!ZSTD_isError(GD_compress(tx, cc, coded, sizeof(coded), data + 4 * GD_BLOCK_SIZE, 128, &view)) && view.used_mask == 2);
    CHECK(GD_write(rx, 1, test_epochs[1], 0, data, 512) == GD_OK);
    CHECK(GD_rotate(tx, 0, test_epochs[0], target, 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 0, test_epochs[0], target, 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
    CHECK(GD_append(tx, 0, prefix, sizeof(prefix), &offset) == GD_OK);
    CHECK(GD_write(rx, 0, target, 0, prefix, sizeof(prefix)) == GD_OK);
    GD_setTime(tx, 110);
    CHECK(GD_observePartition(tx, 1, &source_state) == GD_OK && source_state.heat > 0);
    written = GD_stats(tx)->payload_written;
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 0, target, &first, 1) == GD_OK);
    CHECK(GD_copyMoves(rx, 0, test_epochs[1], 0, target, &first, 1) == GD_OK);
    CHECK(GD_epochEqual(GD_epoch(tx, 1), test_epochs[1]) && GD_prepare(tx, 0) == 0);
    CHECK(GD_read(tx, 1, test_epochs[1], 0, output, GD_BLOCK_SIZE) == GD_OK && !memcmp(output, data, GD_BLOCK_SIZE));
    CHECK(GD_stats(tx)->payload_written == written + GD_BLOCK_SIZE);
    CHECK(GD_extent(tx, 0) == 3 * GD_BLOCK_SIZE && GD_extent(rx, 0) == 3 * GD_BLOCK_SIZE);
    CHECK(GD_read(rx, 0, target, first.destination_offset, output, 512) == GD_OK && !memcmp(output, data, 512));
    CHECK(GD_read(rx, 0, target, first.destination_offset + 512, output, 1) == GD_MISSING);
    CHECK(!ZSTD_isError(GD_compressTracked(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 0)) && view.used_mask == 2);
    /* Invalid later batches cannot disturb the already copied prefix or live source. */
    CHECK(GD_copyMoves(tx, 0, replacement, 0, target, &tail, 1) == GD_STALE);
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 0, replacement, &tail, 1) == GD_STALE);
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 1, test_epochs[1], &tail, 1) == GD_INVALID);
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 0, target, &first, 1) == GD_INVALID);
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 0, target, invalid, 2) == GD_INVALID);
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 0, target, invalid + 1, 1) == GD_CAPACITY);
    CHECK(GD_stats(tx)->payload_written == written + GD_BLOCK_SIZE && GD_extent(tx, 0) == 3 * GD_BLOCK_SIZE);
    CHECK(GD_epochEqual(GD_epoch(tx, 1), test_epochs[1]));
    /* The source receives late bytes between batches. Already copied target
     * holes remain holes until their own repair arrives. */
    CHECK(GD_write(rx, 1, test_epochs[1], 512, data + 512, GD_BLOCK_SIZE - 512) == GD_OK);
    CHECK(GD_write(rx, 1, test_epochs[1], 4 * GD_BLOCK_SIZE, data + 4 * GD_BLOCK_SIZE, 128) == GD_OK);
    CHECK(GD_read(rx, 0, target, first.destination_offset + 512, output, 1) == GD_MISSING);
    CHECK(GD_copyMoves(tx, 0, test_epochs[1], 0, target, &tail, 1) == GD_OK);
    CHECK(GD_copyMoves(rx, 0, test_epochs[1], 0, target, &tail, 1) == GD_OK);
    CHECK(GD_read(rx, 0, target, tail.destination_offset, output, 128) == GD_OK && !memcmp(output, data + 4 * GD_BLOCK_SIZE, 128));
    CHECK(GD_extent(tx, 0) == 3 * GD_BLOCK_SIZE + 128 && GD_extent(rx, 0) == 3 * GD_BLOCK_SIZE + 128);
    CHECK(GD_observePartition(tx, 0, &target_state) == GD_OK && target_state.heat == source_state.heat);
    CHECK(GD_stats(tx)->payload_written == written + GD_BLOCK_SIZE + 128);
    CHECK(GD_rotate(tx, 0, test_epochs[1], replacement, 0, target, NULL, 0) == GD_OK);
    CHECK(GD_rotate(rx, 0, test_epochs[1], replacement, 0, target, NULL, 0) == GD_OK);
    CHECK(GD_stats(tx)->payload_written == written + GD_BLOCK_SIZE + 128);
    CHECK(GD_prepare(tx, 0) == 1 && GD_prepare(rx, 0) == 1);
    CHECK(GD_read(tx, 1, test_epochs[1], 0, output, 1) == GD_STALE);
    CHECK(GD_read(rx, 1, test_epochs[1], 0, output, 1) == GD_STALE);
    CHECK(GD_write(rx, 0, target, first.destination_offset + 512, data + 512, GD_BLOCK_SIZE - 512) == GD_OK);
    CHECK(GD_read(rx, 0, target, first.destination_offset, output, GD_BLOCK_SIZE) == GD_OK && !memcmp(output, data, GD_BLOCK_SIZE));
    CHECK(!ZSTD_isError(GD_compressTracked(tx, cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 0)) && view.used_mask == 1);
    CHECK(GD_observePartition(tx, 0, &target_state) == GD_OK && target_state.heat == source_state.heat);
    CHECK(!GD_stats(rx)->index_allocated && !GD_stats(rx)->indexed_positions);
    ZSTD_freeCCtx(cc); GD_free(tx); GD_free(rx);
}

static void test_promotion_resumes_after_perpetual_retention(void)
{
    uint32_t capacities[3] = {4 * GD_BLOCK_SIZE, CAP, CAP};
    GD_Store* stores[] = {GD_createWithCapacities(capacities, 1, test_epochs), GD_createWithCapacities(capacities, 0, test_epochs)};
    unsigned char old[GD_BLOCK_SIZE], prefix[GD_BLOCK_SIZE], data[2 * GD_BLOCK_SIZE], output[GD_BLOCK_SIZE], coded[2 * GD_BLOCK_SIZE];
    GD_Epoch const p0 = fixture_newEpoch(), p1 = fixture_newEpoch(), m2 = fixture_newEpoch(), m3 = fixture_newEpoch();
    GD_Move first = {0, GD_BLOCK_SIZE}, retained = {0, 2 * GD_BLOCK_SIZE}, second = {1, 0};
    GD_FrameView view;
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    uint64_t const saved_rng = rng;
    unsigned i;
    CHECK(stores[0] && stores[1] && cc);
    random_bytes(old, sizeof(old)); random_bytes(prefix, sizeof(prefix)); random_bytes(data, sizeof(data)); rng = saved_rng;
    for (i = 0; i < 2; ++i) {
        GD_Store* s = stores[i];
        write_part(s, 1, old, sizeof(old));
        write_part(s, 3, data, sizeof(data));
        CHECK(GD_rotate(s, 0, test_epochs[0], p0, 0, GD_NO_EPOCH, NULL, 0) == GD_OK);
        CHECK(GD_write(s, 0, p0, 0, prefix, sizeof(prefix)) == GD_OK);
        CHECK(GD_rotate(s, 1, test_epochs[2], m2, 0, p0, NULL, 0) == GD_OK);
    }
    CHECK(!ZSTD_isError(GD_compress(stores[0], cc, coded, sizeof(coded), old, sizeof(old), &view)) && view.used_mask == 2);
    CHECK(!ZSTD_isError(GD_compress(stores[0], cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view)) && view.used_mask == 8);
    CHECK(!ZSTD_isError(GD_compress(stores[0], cc, coded, sizeof(coded), data + GD_BLOCK_SIZE, GD_BLOCK_SIZE, &view)) && view.used_mask == 8);
    for (i = 0; i < 2; ++i) {
        GD_Store* s = stores[i];
        CHECK(GD_copyMoves(s, 1, test_epochs[3], 0, p0, &first, 1) == GD_OK);
        CHECK(GD_extent(s, 0) == capacities[0] / 2);
        /* Pause M-to-P at half-full, retain old P in its reserved half, and
         * retire P. The still-live M source then resumes into the new prepare. */
        CHECK(GD_copyMoves(s, 0, test_epochs[1], 0, p0, &retained, 1) == GD_OK);
        CHECK(GD_rotate(s, 0, test_epochs[1], p1, 0, p0, NULL, 0) == GD_OK);
        CHECK(GD_epochEqual(GD_epoch(s, 3), test_epochs[3]) && GD_committed(s, 1) == 3);
        CHECK(GD_copyMoves(s, 1, test_epochs[3], 1, p1, &second, 1) == GD_OK);
        CHECK(GD_rotate(s, 1, test_epochs[3], m3, 1, p1, NULL, 0) == GD_OK);
        CHECK(GD_read(s, 0, p0, first.destination_offset, output, GD_BLOCK_SIZE) == GD_OK && !memcmp(output, data, GD_BLOCK_SIZE));
        CHECK(GD_read(s, 0, p0, retained.destination_offset, output, GD_BLOCK_SIZE) == GD_OK && !memcmp(output, old, GD_BLOCK_SIZE));
        CHECK(GD_read(s, 1, p1, 0, output, GD_BLOCK_SIZE) == GD_OK && !memcmp(output, data + GD_BLOCK_SIZE, GD_BLOCK_SIZE));
        CHECK(GD_read(s, 3, test_epochs[3], 0, output, 1) == GD_STALE);
    }
    CHECK(!ZSTD_isError(GD_compressTracked(stores[0], cc, coded, sizeof(coded), data, GD_BLOCK_SIZE, &view, 0)) && view.used_mask == 1);
    CHECK(!ZSTD_isError(GD_compressTracked(stores[0], cc, coded, sizeof(coded), data + GD_BLOCK_SIZE, GD_BLOCK_SIZE, &view, 0)) && view.used_mask == 2);
    CHECK(!GD_stats(stores[1])->index_allocated && !GD_stats(stores[1])->indexed_positions);
    ZSTD_freeCCtx(cc); GD_free(stores[0]); GD_free(stores[1]);
}

int main(int argc, char** argv)
{
    fixture_initialEpochs(test_epochs); fixture_initialEpochs(next_epochs);
    test_copy_batches_keep_source_until_retire();
    test_promotion_resumes_after_perpetual_retention();
    test_retention_physical_tail();
    test_tail_merge_does_not_expand_budget();
    test_receiver_layout_without_payload();
    test_export_validity_ranges();
    test_heat_decay_changes_retention();
    test_snapshot_partial_mismatch_and_invalid_sections();
    test_snapshot_preserves_old_index_after_append();
    test_independent_lifetimes_reject_old_frames();
    test_uuid_checks_and_rotation();
    test_readonly_recovery();
    test_store_address_isolation();
    if (argc == 2 && !strcmp(argv[1], "isolation")) return 0;
    test_file_mappings_are_not_modified_by_recovery();
    test_unclaimed_order_and_cursor();
    test_sender_ignores_recovered_invalid_ranges();
    test_dynamic_regions_and_selection();
    test_large_local_capacities();
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
