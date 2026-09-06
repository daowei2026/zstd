/* Causal, content-only workload observation. BSD license.
 * Reads Ethernet classic pcap from stdin; never writes packet contents. */
#include "dictionary.h"
#include "../../lib/zstd_segmented.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "workload check failed at line %d\n", __LINE__); exit(1); } } while (0)
#define MINIMUM(a,b) ((a) < (b) ? (a) : (b))
typedef struct {
    GD_Store *tx, *rx;
    ZSTD_CCtx *cc, *plain;
    ZSTD_DCtx *dc;
    uint64_t frames, raw, coded, nodict, poor, appends, maintenance;
    uint64_t matched[3], headers[3], payload[3], rotations[3];
    uint64_t first_ns, last_ns;
} Direction;
typedef struct { uint32_t block; uint64_t hits; } Candidate;
static uint32_t capacity, admit_every;
static unsigned char endpoint[4];
static Direction directions[2];

static uint32_t number(const char* text)
{
    char* end; unsigned long value = strtoul(text, &end, 10);
    REQUIRE(*text && !*end && value && value <= UINT32_MAX && *text != '-');
    return (uint32_t)value;
}
static uint32_t u32(const unsigned char* p, int little)
{
    if (little) return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static unsigned u16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
static int read_exact(void* out, size_t n, int eof_allowed)
{
    size_t got = fread(out, 1, n, stdin);
    if (!got && eof_allowed && feof(stdin)) return 0;
    REQUIRE(got == n);
    return 1;
}
static void initialize(Direction* d)
{
    d->tx = GD_create(capacity, 1); d->rx = GD_create(capacity, 0);
    d->cc = ZSTD_createCCtx(); d->plain = ZSTD_createCCtx(); d->dc = ZSTD_createDCtx();
    REQUIRE(d->tx && d->rx && d->cc && d->plain && d->dc);
}
static int compare(const void* a, const void* b)
{
    const Candidate *x = a, *y = b;
    if (x->hits != y->hits) return x->hits > y->hits ? -1 : 1;
    return (x->block > y->block) - (x->block < y->block);
}
static void rotate(Direction* d, unsigned tier)
{
    unsigned source = GD_committed(d->tx, tier), destination, block;
    size_t count = 0, i, blocks = (capacity + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE;
    size_t limit = capacity / GD_BLOCK_SIZE / 4;
    uint32_t offset;
    Candidate* candidates = calloc(blocks, sizeof(*candidates));
    GD_Move* moves = calloc(limit ? limit : 1, sizeof(*moves));
    REQUIRE(candidates && moves);
    for (block = 0; block < blocks; ++block) {
        uint64_t hits = GD_blockHits(d->tx, source, block);
        if (hits >= 2) { candidates[count].block = block; candidates[count++].hits = hits; }
    }
    qsort(candidates, count, sizeof(*candidates), compare);
    count = MINIMUM(count, limit);
    destination = tier ? GD_prepare(d->tx, tier - 1) : source;
    offset = tier ? (GD_extent(d->tx, destination) + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE : 0;
    if (tier && count && offset + count * GD_BLOCK_SIZE > capacity) {
        rotate(d, tier - 1); destination = GD_prepare(d->tx, tier - 1);
        offset = (GD_extent(d->tx, destination) + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE;
    }
    for (i = 0; i < count; ++i) {
        moves[i].source_block = candidates[i].block;
        moves[i].destination_offset = offset + (uint32_t)i * GD_BLOCK_SIZE;
    }
    {
        uint64_t epoch = GD_epoch(d->tx, source), target = GD_epoch(d->tx, destination);
        REQUIRE(GD_rotate(d->tx, tier, epoch, destination, target, moves, count) == GD_OK);
        REQUIRE(GD_rotate(d->rx, tier, epoch, destination, target, moves, count) == GD_OK);
    }
    d->maintenance += (32 + count * 8) * 3;
    ++d->rotations[tier];
    free(candidates); free(moves);
}
static void append(Direction* d, const void* data, size_t length)
{
    unsigned part = GD_prepare(d->tx, 2);
    uint32_t offset;
    if (GD_extent(d->tx, part) + length > capacity) { rotate(d, 2); part = GD_prepare(d->tx, 2); }
    REQUIRE(GD_append(d->tx, 2, data, length, &offset) == GD_OK);
    REQUIRE(GD_write(d->rx, part, GD_epoch(d->tx, part), offset, data, length) == GD_OK);
    d->maintenance += (length + 32 * ((length + 999) / 1000)) * 3;
    ++d->appends;
}
static size_t overlap(size_t a, size_t b, size_t low, size_t high)
{
    if (a < low) a = low;
    if (b > high) b = high;
    return b > a ? b - a : 0;
}
static void observe(Direction* d, const unsigned char* frame, size_t length,
                    size_t header_end, size_t ip_end, uint64_t ns)
{
    ZSTD_Sequence seq[GD_MAX_FRAME / 8 + 1];
    GD_FrameView view;
    unsigned char encoded[GD_MAX_FRAME + 1024], decoded[GD_MAX_FRAME];
    size_t count, i, at = 0, matched = 0, coded, plain, restored;
    if (!d->tx) initialize(d);
    if (!d->frames) d->first_ns = ns;
    d->last_ns = ns;
    /* Same matcher and codec calls as GD_compress, with sequence observation.
     * Learn only after scoring and verifying this original frame. */
    REQUIRE(GD_sequences(d->tx, frame, length, seq, sizeof(seq)/sizeof(*seq), &count, &view) == GD_OK);
    for (i = 0; i < count; ++i) {
        at += seq[i].litLength;
        if (seq[i].matchLength) {
            size_t address = (size_t)GD_PARTITIONS * capacity + at - seq[i].offset;
            unsigned tier = (unsigned)(address / capacity) / 2;
            REQUIRE(tier < 3 && at + seq[i].matchLength <= length);
            d->matched[tier] += seq[i].matchLength;
            d->headers[tier] += overlap(at, at + seq[i].matchLength, 0, header_end);
            d->payload[tier] += overlap(at, at + seq[i].matchLength, header_end, ip_end);
            matched += seq[i].matchLength;
            at += seq[i].matchLength;
        }
    }
    REQUIRE(at == length);
    REQUIRE(!ZSTD_isError(ZSTD_CCtx_reset(d->cc, ZSTD_reset_session_and_parameters)));
    if (view.used_mask) {
        REQUIRE(!ZSTD_isError(ZSTD_CCtx_setParameter(d->cc, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters)));
        coded = ZSTD_compressSequencesWithExternalDictSize(d->cc, encoded, sizeof(encoded), seq, count,
                                                         frame, length, (size_t)GD_PARTITIONS * capacity);
    } else coded = ZSTD_compressCCtx(d->cc, encoded, sizeof(encoded), frame, length, 3);
    REQUIRE(!ZSTD_isError(coded));
    restored = GD_decompress(d->rx, d->dc, decoded, sizeof(decoded), encoded, coded, &view);
    REQUIRE(restored == length && memcmp(decoded, frame, length) == 0);
    plain = ZSTD_compressCCtx(d->plain, encoded, sizeof(encoded), frame, length, 3);
    REQUIRE(!ZSTD_isError(plain));
    ++d->frames; d->raw += length; d->coded += coded; d->nodict += plain;
    /* Byte-only experimental policy: admit every Nth frame with less than
     * 50% external dictionary coverage. No template IDs, random-data oracle,
     * application labels, future observations, or repetition copies. */
    if (matched * 2 < length && d->poor++ % admit_every == 0) append(d, frame, length);
}
static void triple(const char* name, const uint64_t values[3])
{
    printf(",\"%s\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", name, values[0], values[1], values[2]);
}
static void report(unsigned dir, const char* kind)
{
    Direction* d = &directions[dir];
    const GD_Stats* s;
    uint64_t allocated = 0;
    unsigned tier, role;
    if (!d->tx) return;
    s = GD_stats(d->tx);
    printf("{\"kind\":\"%s\",\"direction\":\"%s\",\"frames\":%" PRIu64
           ",\"first_ns\":%" PRIu64 ",\"last_ns\":%" PRIu64 ",\"raw_bytes\":%" PRIu64
           ",\"coded_bytes\":%" PRIu64 ",\"no_dictionary_bytes\":%" PRIu64
           ",\"maintenance_model_bytes\":%" PRIu64 ",\"model_ratio_R2\":%.8f"
           ",\"appends\":%" PRIu64 ",\"payload_written\":%" PRIu64
           ",\"payload_allocated\":%" PRIu64 ",\"payload_freed\":%" PRIu64
           ",\"payload_transferred\":%" PRIu64 ",\"retained_cold_lower\":%" PRIu64
           ",\"index_bytes\":%" PRIu64 ",\"metadata_bytes\":%" PRIu64,
           kind, dir ? "toward_client" : "from_client", d->frames,
           d->first_ns, d->last_ns, d->raw, d->coded, d->nodict, d->maintenance,
           (double)((d->coded + 32*d->frames)*2 + d->maintenance) / ((d->raw + 32*d->frames)*2),
           d->appends, s->payload_written, s->payload_allocated, s->payload_freed,
           s->payload_transferred, s->payload_transferred - s->transferred_referenced_upper,
           s->index_allocated, s->metadata_allocated);
    triple("matched_bytes", d->matched); triple("header_matched_bytes", d->headers);
    triple("transport_payload_matched_bytes", d->payload); triple("rotations", d->rotations);
    printf(",\"partitions\":[");
    for (tier = 0; tier < 3; ++tier) for (role = 0; role < 2; ++role) {
        unsigned p = role ? GD_committed(d->tx, tier) : GD_prepare(d->tx, tier);
        unsigned b; uint64_t bytes = 0;
        for (b = 0; b < (capacity + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE; ++b)
            if (GD_blockAddress(d->tx, p, b)) bytes += GD_BLOCK_SIZE;
        allocated += bytes;
        printf("%s{\"tier\":%u,\"role\":\"%s\",\"epoch\":%" PRIu64
               ",\"extent\":%u,\"allocated\":%" PRIu64 ",\"capacity\":%u}",
               tier || role ? "," : "", tier, role ? "committed" : "prepare",
               GD_epoch(d->tx, p), GD_extent(d->tx, p), bytes, capacity);
    }
    REQUIRE(allocated == s->payload_allocated);
    printf("]}\n"); fflush(stdout);
}
int main(int argc, char** argv)
{
    unsigned char global[24], record[16], frame[GD_MAX_FRAME];
    uint64_t selected = 0, ignored = 0, next_report = 0, last_ns = 0;
    unsigned dir;
    int little, nanos;
    REQUIRE(argc == 4);
    capacity = number(argv[1]); admit_every = number(argv[2]);
    REQUIRE(capacity >= 16384 && capacity <= (1U << 30) / GD_PARTITIONS);
    REQUIRE(inet_pton(AF_INET, argv[3], endpoint) == 1);
    read_exact(global, sizeof(global), 0);
    little = !memcmp(global, "\xd4\xc3\xb2\xa1", 4) || !memcmp(global, "\x4d\x3c\xb2\xa1", 4);
    nanos = !memcmp(global, "\x4d\x3c\xb2\xa1", 4) || !memcmp(global, "\xa1\xb2\x3c\x4d", 4);
    REQUIRE(little || !memcmp(global, "\xa1\xb2\xc3\xd4", 4) || !memcmp(global, "\xa1\xb2\x3c\x4d", 4));
    REQUIRE(!memcmp(global + 4, little ? "\x02\x00\x04\x00" : "\x00\x02\x00\x04", 4));
    REQUIRE(u32(global + 20, little) == 1); /* Ethernet, no implicit cooked-header conversion. */
    printf("{\"kind\":\"configuration\",\"partition_bytes\":%u,\"admit_every\":%u,"
           "\"business_R\":2,\"maintenance_R\":3,\"model_record_header\":32,"
           "\"maintenance_payload_limit\":1000,\"report_interval_ns\":1000000000}\n", capacity, admit_every);
    while (read_exact(record, sizeof(record), 1)) {
        uint32_t length = u32(record + 8, little), original = u32(record + 12, little);
        uint32_t fraction = u32(record + 4, little);
        uint64_t ns = (uint64_t)u32(record, little) * 1000000000 + fraction * (nanos ? 1ULL : 1000ULL);
        size_t ip = 14, header_end, ip_end;
        unsigned type, ihl, proto;
        REQUIRE(length == original && length >= 14 && length <= sizeof(frame));
        REQUIRE(fraction < (nanos ? 1000000000U : 1000000U));
        read_exact(frame, length, 0);
        type = u16(frame + 12);
        while (type == 0x8100 || type == 0x88a8) {
            REQUIRE(ip + 4 <= length && ip <= 18); type = u16(frame + ip + 2); ip += 4;
        }
        if (type != 0x0800) { ++ignored; continue; }
        REQUIRE(ip + 20 <= length && frame[ip] >> 4 == 4);
        if (!memcmp(frame + ip + 12, endpoint, 4)) dir = 0;
        else if (!memcmp(frame + ip + 16, endpoint, 4)) dir = 1;
        else { ++ignored; continue; }
        ihl = (frame[ip] & 15) * 4; ip_end = ip + u16(frame + ip + 2);
        /* Offload super-packets and IP fragments need a separate measurement
         * path; rejecting them avoids claiming reconstructed frames were seen. */
        REQUIRE(ihl >= 20 && ip + ihl <= ip_end && ip_end <= length);
        REQUIRE(ip_end - ip <= 1500 && (u16(frame + ip + 6) & 0x3fff) == 0);
        header_end = ip + ihl; proto = frame[ip + 9];
        if (proto == 6) {
            unsigned tcp;
            REQUIRE(header_end + 20 <= ip_end);
            tcp = (frame[header_end + 12] >> 4) * 4;
            REQUIRE(tcp >= 20 && header_end + tcp <= ip_end); header_end += tcp;
        } else if (proto == 17) { REQUIRE(header_end + 8 <= ip_end); header_end += 8; }
        REQUIRE(!selected || ns >= last_ns); last_ns = ns;
        observe(&directions[dir], frame, length, header_end, ip_end, ns); ++selected;
        if (!next_report) next_report = ns + 1000000000;
        if (ns >= next_report) {
            report(0, "sample"); report(1, "sample"); next_report = ns + 1000000000;
        }
    }
    REQUIRE(selected);
    report(0, "final"); report(1, "final");
    {
        struct rusage usage; REQUIRE(getrusage(RUSAGE_SELF, &usage) == 0);
        printf("{\"kind\":\"capture_summary\",\"selected\":%" PRIu64 ",\"ignored\":%" PRIu64
               ",\"analyzer_peak_rss_kib\":%ld,\"complete\":true}\n", selected, ignored, usage.ru_maxrss);
    }
    for (dir = 0; dir < 2; ++dir) {
        Direction* d = &directions[dir];
        GD_free(d->tx); GD_free(d->rx); ZSTD_freeCCtx(d->cc); ZSTD_freeCCtx(d->plain); ZSTD_freeDCtx(d->dc);
    }
    return 0;
}
