/* Experimental SRFEC dictionary research, repository BSD license. */
#include "dictionary.h"
#include "../../lib/zstd_segmented.h"
#include "../../lib/compress/zstd_compress_internal.h"
#include <stdlib.h>
#include <string.h>
#include "../../lib/zstd_errors.h"

#define GD_MAX_MATCHES (GD_MAX_FRAME / 8 + 1)
typedef struct { uint32_t begin, end, priority; } GD_Pending;
typedef struct { uint32_t begin, end, offset, size; } GD_Piece;
typedef struct { uint32_t begin, end; } GD_Unclaimed;
typedef struct { uint32_t remaining[GD_PARTITIONS]; size_t comparisons; } GD_Discovery;
#define GD_INPUT_HASH_LOG 13

typedef struct {
    unsigned char* present;
    uint32_t used;
    uint32_t present_count;
    uint64_t hits;
    uint64_t hit_regions;
    uint64_t reused_bytes;
    uint32_t reused_begin, reused_end; /* exact bounding range, unlike 64-byte bins */
} GD_Block;
typedef struct {
    unsigned char* data;
    GD_Block* blocks;
    uint32_t* index;
    uint16_t* tags;
    unsigned hash_log, ways, stride;
    uint32_t extent, capacity, block_count;
    uint64_t epoch;
    int owns_data;
    GD_Unclaimed* unclaimed;
    size_t unclaimed_count, unclaimed_capacity;
    uint64_t unclaimed_bytes;
    uint32_t scan_cursor;
} GD_Part;
struct GD_Store {
    GD_Part parts[GD_PARTITIONS];
    unsigned prepare[3];
    uint32_t capacity;
    int sender;
    ZSTD_Sequence* sequences;
    GD_Match *trial, *matches;
    GD_Pending* pending;
    GD_Piece* pieces;
    int64_t *starts, *ends;
    uint32_t* minima;
    unsigned char* encoded;
    size_t match_count;
    unsigned segment_ratio;
    uint32_t unclaimed_window;
    uint32_t *input_heads, *input_next;
    GD_Stats stats;
    GD_Missing missing;
    GD_Result result;
};

static int GD_present(const GD_Block* block, unsigned offset)
{
    if (!block) return 0;
    if (block->present) return (block->present[offset >> 3] >> (offset & 7)) & 1;
    return offset < block->used;
}

static int GD_prepareBlock(GD_Store* store, GD_Block* block, int sparse)
{
    if (sparse && block->used < GD_BLOCK_SIZE && !block->present) {
        uint32_t i;
        block->present = (unsigned char*)calloc(GD_BLOCK_SIZE / 8, 1);
        if (!block->present) return 0;
        for (i = 0; i < block->used; ++i) block->present[i >> 3] |= (unsigned char)(1U << (i & 7));
        block->present_count = block->used;
        store->stats.metadata_allocated += GD_BLOCK_SIZE / 8;
    }
    return 1;
}

static int GD_reserveUnclaimed(GD_Store* store, GD_Part* part, size_t count)
{
    GD_Unclaimed* entries;
    size_t capacity;
    if (count <= part->unclaimed_capacity) return 1;
    capacity = MAX(count, part->unclaimed_capacity * 2);
    if (capacity > SIZE_MAX / sizeof(*entries)) return 0;
    entries = (GD_Unclaimed*)realloc(part->unclaimed, capacity * sizeof(*entries));
    if (!entries) return 0;
    store->stats.metadata_allocated += (capacity - part->unclaimed_capacity) * sizeof(*entries);
    part->unclaimed = entries; part->unclaimed_capacity = capacity;
    return 1;
}

/* Constructor-only readiness adoption. No payload byte is read or written. */
static int GD_restoreRange(GD_Store* store, GD_Part* part, uint32_t offset, uint32_t length)
{
    uint32_t at = offset, end = offset + length;
    while (at < end) {
        GD_Block* block = &part->blocks[at / GD_BLOCK_SIZE];
        uint32_t const in = at % GD_BLOCK_SIZE;
        uint32_t const n = MIN(end - at, GD_BLOCK_SIZE - in);
        if (!GD_prepareBlock(store, block, in > block->used)) return 0;
        if (block->present) {
            uint32_t i;
            for (i = in; i < in + n; ++i) {
                if (!GD_present(block, i)) ++block->present_count;
                block->present[i >> 3] |= (unsigned char)(1U << (i & 7));
            }
        }
        block->used = MAX(block->used, in + n);
        at += n;
    }
    if (store->sender) {
        if (part->unclaimed_count && part->unclaimed[part->unclaimed_count - 1].end == offset)
            part->unclaimed[part->unclaimed_count - 1].end = end;
        else {
            if (!GD_reserveUnclaimed(store, part, part->unclaimed_count + 1)) return 0;
            part->unclaimed[part->unclaimed_count++] = (GD_Unclaimed){offset, end};
        }
        part->unclaimed_bytes += length;
    }
    return 1;
}

static void GD_clearBlock(GD_Store* store, GD_Block* block)
{
    if (block->present) store->stats.metadata_allocated -= GD_BLOCK_SIZE / 8;
    free(block->present);
    memset(block, 0, sizeof(*block));
}

GD_Store* GD_create(uint32_t capacity, int sender)
{
    uint32_t capacities[3] = {capacity, capacity, capacity};
    return GD_createWithCapacities(capacities, sender);
}

GD_Store* GD_createWithCapacities(const uint32_t capacities[3], int sender)
{
    return GD_createWithBuffers(capacities, NULL, sender, NULL);
}

GD_Store* GD_createWithBuffers(const uint32_t capacities[3],
                              void* const buffers[GD_PARTITIONS], int sender,
                              const GD_Layout* recovered)
{
    GD_Store* store;
    unsigned slot, tier;
    static const unsigned logs[3] = {20, 19, 18};
    static const unsigned ways[3] = {8, 4, 2};
    if (!capacities) return NULL;
    for (tier = 0; tier < 3; ++tier)
        if (!capacities[tier] || capacities[tier] > ZSTD_EXTERNAL_DICT_SIZE_MAX) return NULL;
    if (recovered) {
        size_t i;
        if (!buffers || (recovered->range_count && !recovered->ranges)) return NULL;
        for (slot = 0; slot < GD_PARTITIONS; ++slot)
            if (!recovered->epoch[slot] || recovered->extent[slot] > capacities[slot / 2]) return NULL;
        for (tier = 0; tier < 3; ++tier)
            if (recovered->prepare[tier] / 2 != tier) return NULL;
        for (i = 0; i < recovered->range_count; ++i) {
            GD_Missing const r = recovered->ranges[i];
            if (r.partition >= GD_PARTITIONS || r.epoch != recovered->epoch[r.partition] || !r.length ||
                r.offset > recovered->extent[r.partition] || r.length > recovered->extent[r.partition] - r.offset)
                return NULL;
            if (i) {
                GD_Missing const previous = recovered->ranges[i - 1];
                if (previous.partition > r.partition || (previous.partition == r.partition &&
                    previous.offset + previous.length > r.offset)) return NULL;
            }
        }
    }
    if (buffers) for (slot = 0; slot < GD_PARTITIONS; ++slot) {
        uintptr_t const base = (uintptr_t)buffers[slot];
        unsigned other;
        if (!base || base > UINTPTR_MAX - capacities[slot / 2]) return NULL;
        for (other = 0; other < slot; ++other) {
            uintptr_t const previous = (uintptr_t)buffers[other];
            if (base < previous + capacities[other / 2] &&
                previous < base + capacities[slot / 2]) return NULL;
        }
    }
    store = (GD_Store*)calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->capacity = MAX(capacities[0], MAX(capacities[1], capacities[2]));
    store->sender = sender != 0;
    store->segment_ratio = 50;
    store->unclaimed_window = 4096;
    store->stats.metadata_allocated = sizeof(*store);
    if (store->sender) {
        size_t const count = GD_MAX_MATCHES;
#define GD_SCRATCH(member, n) do { \
        size_t const bytes = (n) * sizeof(*store->member); \
        store->member = malloc(bytes); \
        if (!store->member) { GD_free(store); return NULL; } \
        store->stats.metadata_allocated += bytes; \
    } while (0)
        GD_SCRATCH(sequences, count);
        GD_SCRATCH(trial, count); GD_SCRATCH(matches, count);
        GD_SCRATCH(pending, count + 1); GD_SCRATCH(pieces, count);
        GD_SCRATCH(starts, count); GD_SCRATCH(ends, count); GD_SCRATCH(minima, count);
        GD_SCRATCH(encoded, 2 * ZSTD_compressBound(GD_MAX_FRAME));
        GD_SCRATCH(input_heads, 1U << GD_INPUT_HASH_LOG); GD_SCRATCH(input_next, GD_MAX_FRAME);
#undef GD_SCRATCH
    }
    for (slot = 0; slot < GD_PARTITIONS; ++slot) {
        GD_Part* part = &store->parts[slot];
        uint32_t const capacity = capacities[slot / 2];
        unsigned log = logs[slot / 2];
        part->capacity = capacity;
        part->block_count = (uint32_t)(((uint64_t)capacity + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE);
        while (log > 4 && (1U << (log - 1)) >= ((uint64_t)capacity + 7) / 8) --log;
        part->hash_log = log;
        part->ways = ways[slot / 2];
        part->stride = capacity < 1048576 ? 1 : (8U << (slot / 2));
        part->epoch = slot + 1;
        part->owns_data = buffers == NULL;
        part->data = buffers ? (unsigned char*)buffers[slot] : (unsigned char*)malloc(capacity);
        part->blocks = (GD_Block*)calloc(part->block_count, sizeof(*part->blocks));
        if (!part->data || !part->blocks) { GD_free(store); return NULL; }
        store->stats.payload_allocated += capacity;
        store->stats.payload_peak_allocated = store->stats.payload_allocated;
        store->stats.metadata_allocated += part->block_count * sizeof(*part->blocks);
        if (store->sender) {
            size_t const bytes = ((size_t)1 << log) * part->ways * sizeof(uint32_t);
            part->index = (uint32_t*)calloc(1, bytes);
            part->tags = (uint16_t*)calloc(1, bytes / 2);
            if (!part->index || !part->tags) { GD_free(store); return NULL; }
            store->stats.index_allocated += bytes + bytes / 2;
        }
    }
    for (slot = 0; slot < 3; ++slot) store->prepare[slot] = 2 * slot + 1;
    if (recovered) {
        size_t i;
        for (slot = 0; slot < GD_PARTITIONS; ++slot) {
            store->parts[slot].epoch = recovered->epoch[slot];
            store->parts[slot].extent = recovered->extent[slot];
        }
        for (tier = 0; tier < 3; ++tier) store->prepare[tier] = recovered->prepare[tier];
        for (i = 0; i < recovered->range_count; ++i) {
            GD_Missing const r = recovered->ranges[i];
            if (!GD_restoreRange(store, &store->parts[r.partition], r.offset, r.length)) {
                GD_free(store); return NULL;
            }
        }
    }
    return store;
}

void GD_free(GD_Store* store)
{
    unsigned slot;
    if (!store) return;
    for (slot = 0; slot < GD_PARTITIONS; ++slot) {
        GD_Part* part = &store->parts[slot];
        uint32_t i;
        if (part->blocks) for (i = 0; i < part->block_count; ++i) GD_clearBlock(store, &part->blocks[i]);
        if (part->owns_data) free(part->data);
        free(part->blocks); free(part->index); free(part->tags); free(part->unclaimed);
    }
    free(store->sequences); free(store->trial); free(store->matches);
    free(store->pending); free(store->pieces); free(store->starts); free(store->ends);
    free(store->minima); free(store->encoded); free(store->input_heads); free(store->input_next); free(store);
}
uint32_t GD_capacity(const GD_Store* s) { return s->capacity; }
uint32_t GD_partitionCapacity(const GD_Store* s, unsigned p) { return p < GD_PARTITIONS ? s->parts[p].capacity : 0; }
GD_Result GD_observePartition(const GD_Store* s, unsigned p, GD_PartitionStats* stats)
{
    const GD_Part* part;
    unsigned b;
    if (!s || p >= GD_PARTITIONS || !stats) return GD_INVALID;
    part = &s->parts[p];
    memset(stats, 0, sizeof(*stats));
    stats->epoch = part->epoch; stats->capacity = part->capacity; stats->extent = part->extent;
    stats->payload_allocated = part->capacity;
    stats->unclaimed_bytes = part->unclaimed_bytes;
    stats->unclaimed_ranges = (uint32_t)part->unclaimed_count; stats->scan_cursor = part->scan_cursor;
    for (b = 0; b < part->block_count; ++b) {
        GD_Block* block = &part->blocks[b];
        uint64_t regions;
        unsigned covered = 0;
        stats->present_bytes += block->present ? block->present_count : block->used;
        regions = block->hit_regions;
        while (regions) { regions &= regions - 1; covered += 64; }
        stats->referenced_upper += MIN(covered, block->used);
    }
    return GD_OK;
}
unsigned GD_prepare(const GD_Store* s, unsigned tier) { return tier < 3 ? s->prepare[tier] : GD_PARTITIONS; }
unsigned GD_committed(const GD_Store* s, unsigned tier) { return GD_prepare(s, tier) ^ 1U; }
uint64_t GD_epoch(const GD_Store* s, unsigned p) { return p < GD_PARTITIONS ? s->parts[p].epoch : 0; }
uint32_t GD_extent(const GD_Store* s, unsigned p) { return p < GD_PARTITIONS ? s->parts[p].extent : 0; }
const GD_Stats* GD_stats(const GD_Store* s) { return &s->stats; }
const GD_Missing* GD_missing(const GD_Store* s) { return &s->missing; }
GD_Result GD_lastResult(const GD_Store* s) { return s->result; }
const void* GD_blockAddress(const GD_Store* s, unsigned p, unsigned b)
{
    if (p >= GD_PARTITIONS || b >= s->parts[p].block_count || !s->parts[p].blocks[b].used) return NULL;
    return s->parts[p].data + b * GD_BLOCK_SIZE;
}
uint64_t GD_blockHits(const GD_Store* s, unsigned p, unsigned b)
{
    if (p >= GD_PARTITIONS || b >= s->parts[p].block_count) return 0;
    return s->parts[p].blocks[b].hits;
}

static unsigned GD_continuity(const GD_Part* part, uint32_t b)
{
    const GD_Block* block = &part->blocks[b];
    unsigned score = 0;
    if (!block->reused_begin && b &&
        part->blocks[b-1].reused_end == GD_BLOCK_SIZE) ++score;
    if (block->reused_end == GD_BLOCK_SIZE && b+1 < part->block_count &&
        part->blocks[b+1].reused_bytes && !part->blocks[b+1].reused_begin) ++score;
    return score;
}

static int GD_lessRetained(const GD_Part* part, uint32_t a, uint32_t b)
{
    uint64_t const ah = part->blocks[a].reused_bytes, bh = part->blocks[b].reused_bytes;
    unsigned ac, bc;
    /* Every move reserves a full metadata region, so the denominator is constant. */
    if (ah != bh) return ah < bh;
    ac = GD_continuity(part, a); bc = GD_continuity(part, b);
    return ac < bc || (ac == bc && a > b);
}

size_t GD_selectMoves(const GD_Store* store, unsigned tier, uint32_t offset,
                      GD_Move* moves, size_t capacity)
{
    const GD_Part* part;
    uint32_t blocks, b;
    size_t count = 0;
    if (!store || !store->sender || tier >= 3 || !moves || !capacity ||
        offset % GD_BLOCK_SIZE || capacity > (UINT32_MAX - offset) / GD_BLOCK_SIZE) return 0;
    part = &store->parts[GD_committed(store, tier)];
    blocks = (part->extent + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE;
    /* A bounded min heap keeps the scan O(blocks * log(retained)). */
    for (b = 0; b < blocks; ++b) {
        size_t at;
        if (!part->blocks[b].reused_bytes) continue;
        if (count < capacity) {
            at = count++;
            while (at && GD_lessRetained(part, b, moves[(at-1)/2].source_block)) {
                moves[at].source_block = moves[(at-1)/2].source_block;
                at = (at-1)/2;
            }
            moves[at].source_block = b;
        } else if (GD_lessRetained(part, moves[0].source_block, b)) {
            at = 0;
            while (at * 2 + 1 < count) {
                size_t child = at * 2 + 1;
                if (child + 1 < count && GD_lessRetained(part, moves[child+1].source_block, moves[child].source_block)) ++child;
                if (!GD_lessRetained(part, moves[child].source_block, b)) break;
                moves[at].source_block = moves[child].source_block;
                at = child;
            }
            moves[at].source_block = b;
        }
    }
    for (b = 0; b < count; ++b) moves[b].destination_offset = offset + b * GD_BLOCK_SIZE;
    return count;
}

/* A complete eight-byte key may cross an append boundary or storage block. */
static const void* GD_key(const GD_Part* part, uint32_t offset)
{
    GD_Block* block;
    unsigned in, i;
    if (offset > part->capacity || part->capacity - offset < 8) return NULL;
    block = &part->blocks[offset / GD_BLOCK_SIZE];
    in = offset % GD_BLOCK_SIZE;
    if (!block->present && in + 8 <= block->used) return part->data + offset;
    for (i = 0; i < 8; ++i) {
        uint32_t const at = offset + i;
        GD_Block* b = &part->blocks[at / GD_BLOCK_SIZE];
        if (!GD_present(b, at % GD_BLOCK_SIZE)) return NULL;
    }
    return part->data + offset;
}

static void GD_indexKey(GD_Store* store, GD_Part* part, uint32_t offset, const void* key)
{
    uint32_t const hash = (uint32_t)ZSTD_hashPtr(key, 32, 8);
    uint32_t* row = part->index + (hash >> (32 - part->hash_log)) * part->ways;
    uint16_t* tags = part->tags + (hash >> (32 - part->hash_log)) * part->ways;
    memmove(row + 1, row, (part->ways - 1) * sizeof(*row));
    memmove(tags + 1, tags, (part->ways - 1) * sizeof(*tags));
    row[0] = offset + 1; tags[0] = (uint16_t)hash;
    ++store->stats.indexed_positions;
}

static void GD_indexRange(GD_Store* store, GD_Part* part, uint32_t start, uint32_t end)
{
    uint32_t offset;
    if (!store->sender || end < 8) return;
    start = start > 7 ? start - 7 : 0;
    for (offset = start; offset <= end - 8; ++offset) {
        const void* key;
        if (offset % part->stride) continue;
        key = GD_key(part, offset);
        if (!key) continue;
        GD_indexKey(store, part, offset, key);
    }
}

/* A verified contiguous match becomes ordinary index coverage. Reserve the
 * possible interval split first; allocation failure leaves it unclaimed. */
static GD_Result GD_recognize(GD_Store* store, GD_Part* part, uint32_t begin, uint32_t end)
{
    size_t i;
    uint64_t removed = 0;
    for (i = 0; i < part->unclaimed_count; ++i) {
        GD_Unclaimed const r = part->unclaimed[i];
        if (r.end <= begin || r.begin >= end) continue;
        removed += MIN(end, r.end) - MAX(begin, r.begin);
        if (begin > r.begin && end < r.end &&
            !GD_reserveUnclaimed(store, part, part->unclaimed_count + 1)) return GD_NOMEM;
    }
    if (!removed) return GD_OK;
    GD_indexRange(store, part, begin, end);
    /* Large-half sampling must not lose an explicitly discovered unaligned key. */
    if (begin % part->stride) GD_indexKey(store, part, begin, part->data + begin);
    for (i = 0; i < part->unclaimed_count;) {
        GD_Unclaimed r = part->unclaimed[i];
        if (r.end <= begin || r.begin >= end) { ++i; continue; }
        if (begin <= r.begin && end >= r.end) {
            memmove(part->unclaimed + i, part->unclaimed + i + 1,
                (--part->unclaimed_count - i) * sizeof(*part->unclaimed));
        } else if (begin <= r.begin) {
            part->unclaimed[i++].begin = end;
        } else if (end >= r.end) {
            part->unclaimed[i++].end = begin;
        } else {
            memmove(part->unclaimed + i + 2, part->unclaimed + i + 1,
                (part->unclaimed_count - i - 1) * sizeof(*part->unclaimed));
            part->unclaimed[i].end = begin;
            part->unclaimed[i + 1] = (GD_Unclaimed){end, r.end};
            ++part->unclaimed_count; i += 2;
        }
    }
    part->unclaimed_bytes -= removed;
    store->stats.payload_recognized += removed;
    return GD_OK;
}

GD_Result GD_write(GD_Store* store, unsigned slot, uint64_t epoch,
                   uint32_t offset, const void* source, size_t length)
{
    GD_Part* part;
    const unsigned char* src = (const unsigned char*)source;
    size_t i;
    uint32_t old_extent;
    if (slot >= GD_PARTITIONS || (!source && length)) return GD_INVALID;
    part = &store->parts[slot];
    if (epoch != part->epoch) return GD_STALE;
    if (offset > part->capacity || length > part->capacity - offset) return GD_CAPACITY;
    if (store->sender && offset > part->extent) return GD_INVALID;
    old_extent = part->extent;
    /* Validate all overlapping bytes before committing any new byte. */
    for (i = 0; i < length; ++i) {
        uint32_t const at = offset + (uint32_t)i;
        GD_Block* block = &part->blocks[at / GD_BLOCK_SIZE];
        unsigned const in = at % GD_BLOCK_SIZE;
        if (GD_present(block, in)) {
            if (part->data[at] != src[i]) return GD_CONFLICT;
        } else if (store->sender && at < old_extent) {
            return GD_INVALID; /* A promotion gap is never rewritten. */
        }
    }
    if (length) {
        uint32_t first = offset / GD_BLOCK_SIZE;
        uint32_t const last = (offset + (uint32_t)length - 1) / GD_BLOCK_SIZE;
        for (; first <= last; ++first) {
            GD_Block* block = &part->blocks[first];
            uint32_t const in = first == offset / GD_BLOCK_SIZE ? offset % GD_BLOCK_SIZE : 0;
            if (!GD_prepareBlock(store, block, !store->sender || in > block->used)) return GD_NOMEM;
        }
    }
    for (i = 0; i < length; ++i) {
        uint32_t const at = offset + (uint32_t)i;
        GD_Block* block = &part->blocks[at / GD_BLOCK_SIZE];
        unsigned const in = at % GD_BLOCK_SIZE;
        if (!GD_present(block, in)) {
            part->data[at] = src[i];
            if (block->present) {
                block->present[in >> 3] |= (unsigned char)(1U << (in & 7));
                if (++block->present_count == GD_BLOCK_SIZE) {
                    free(block->present); block->present = NULL;
                    store->stats.metadata_allocated -= GD_BLOCK_SIZE / 8;
                }
            }
            if (block->used < in + 1) block->used = in + 1;
            ++store->stats.payload_written;
        }
    }
    if (part->extent < offset + length) part->extent = offset + (uint32_t)length;
    if (store->sender && part->extent > old_extent) GD_indexRange(store, part, old_extent, part->extent);
    return GD_OK;
}

GD_Result GD_append(GD_Store* s, unsigned tier, const void* source, size_t length, uint32_t* offset)
{
    unsigned slot;
    if (!s->sender || tier >= 3 || !offset) return GD_INVALID;
    slot = GD_prepare(s, tier);
    *offset = s->parts[slot].extent;
    return GD_write(s, slot, s->parts[slot].epoch, *offset, source, length);
}

/* Linear prefix matching finds containment or a suffix/prefix overlap. */
static uint32_t GD_overlap(const unsigned char* left, uint32_t ln,
                           const unsigned char* right, uint32_t rn)
{
    uint32_t prefix[GD_BLOCK_SIZE];
    uint32_t i, n = 0;
    prefix[0] = 0;
    for (i = 1; i < rn; ++i) {
        while (n && right[n] != right[i]) n = prefix[n-1];
        if (right[n] == right[i]) ++n;
        prefix[i] = n;
    }
    n = 0;
    for (i = 0; i < ln; ++i) {
        while (n && right[n] != left[i]) n = prefix[n-1];
        if (right[n] == left[i]) ++n;
        if (n == rn) return rn;
    }
    return n;
}

static int GD_readyRange(const GD_Part* part, uint32_t offset, uint32_t length)
{
    uint32_t at;
    if (offset > part->extent || length > part->extent - offset) return 0;
    for (at = offset; at < offset + length; ++at)
        if (!GD_present(&part->blocks[at / GD_BLOCK_SIZE], at % GD_BLOCK_SIZE)) return 0;
    return 1;
}

GD_Result GD_compactMoves(GD_Store* store, unsigned tier, uint64_t epoch,
                         unsigned destination, uint64_t destination_epoch,
                         GD_Move* moves, size_t* count, GD_Missing* appended,
                         uint32_t* required)
{
    typedef struct { uint32_t overlap; int reverse; } Pair;
    GD_Part *src, *dst;
    Pair* pairs;
    size_t i, kept = 0, original;
    uint32_t bytes = 0, start, offset;
    uint64_t end;
    unsigned char joined[2 * GD_BLOCK_SIZE];
    GD_Result result = GD_OK;
    if (!store || !store->sender || !count || !appended || !required ||
        !tier || tier >= 3 || destination != GD_prepare(store, tier-1) ||
        (*count && !moves)) return GD_INVALID;
    src = &store->parts[GD_committed(store, tier)]; dst = &store->parts[destination];
    *appended = (GD_Missing){0}; *required = 0;
    if (src->epoch != epoch || dst->epoch != destination_epoch) return GD_STALE;
    original = *count;
    if (original > src->block_count) return GD_INVALID;
    if (!original) return GD_OK;
    for (i = 0; i < original; ++i) {
        uint32_t const b = moves[i].source_block;
        if (b >= src->block_count || (i && b <= moves[i-1].source_block) ||
            !src->blocks[b].reused_bytes) return GD_INVALID;
    }
    pairs = (Pair*)calloc(original, sizeof(*pairs));
    if (!pairs) return GD_NOMEM;
    /* Determine the actual append and move footprint before any mutation.
     * No all-pairs search: each selected block participates in at most one pair. */
    for (i = 0; i < original;) {
        if (i + 1 < original) {
            GD_Block* a = &src->blocks[moves[i].source_block];
            GD_Block* b = &src->blocks[moves[i+1].source_block];
            const unsigned char* ap = src->data + moves[i].source_block * GD_BLOCK_SIZE + a->reused_begin;
            const unsigned char* bp = src->data + moves[i+1].source_block * GD_BLOCK_SIZE + b->reused_begin;
            uint32_t const an = a->reused_end - a->reused_begin;
            uint32_t const bn = b->reused_end - b->reused_begin;
            if (!GD_readyRange(src, moves[i].source_block * GD_BLOCK_SIZE + a->reused_begin, an) ||
                !GD_readyRange(src, moves[i+1].source_block * GD_BLOCK_SIZE + b->reused_begin, bn)) {
                ++kept; ++i; continue;
            }
            uint32_t const ab = GD_overlap(ap, an, bp, bn);
            uint32_t const ba = GD_overlap(bp, bn, ap, an);
            uint32_t const overlap = MAX(ab, ba);
            /* Initial conservative policy: at least half of the shorter hot
             * range overlaps, and at least one codec match key is shared. */
            if (overlap >= 8 && overlap * 2 >= MIN(an, bn)) {
                pairs[i].overlap = overlap; pairs[i].reverse = ba > ab;
                bytes += an + bn - overlap;
                i += 2;
                continue;
            }
        }
        ++kept; ++i;
    }
    *required = kept ? (bytes + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE + (uint32_t)kept * GD_BLOCK_SIZE : bytes;
    start = dst->extent;
    end = (uint64_t)start + bytes;
    if (kept) end = (end + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE + kept * GD_BLOCK_SIZE;
    if (end > dst->capacity) { free(pairs); return GD_CAPACITY; }
    kept = 0;
    for (i = 0; i < original;) {
        if (pairs[i].overlap) {
            uint32_t const ai = moves[i + pairs[i].reverse].source_block;
            uint32_t const bi = moves[i + !pairs[i].reverse].source_block;
            GD_Block* a = &src->blocks[ai];
            GD_Block* b = &src->blocks[bi];
            uint32_t const an = a->reused_end - a->reused_begin;
            uint32_t const bn = b->reused_end - b->reused_begin;
            uint32_t const overlap = pairs[i].overlap;
            memcpy(joined, src->data + ai * GD_BLOCK_SIZE + a->reused_begin, an);
            memcpy(joined + an, src->data + bi * GD_BLOCK_SIZE + b->reused_begin + overlap, bn - overlap);
            result = GD_append(store, tier-1, joined, an + bn - overlap, &offset);
            if (result != GD_OK) break;
            store->stats.payload_relocated += an + bn - overlap;
            i += 2;
        } else {
            moves[kept++].source_block = moves[i++].source_block;
        }
    }
    free(pairs);
    if (result != GD_OK) return result;
    offset = (dst->extent + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE * GD_BLOCK_SIZE;
    for (i = 0; i < kept; ++i) moves[i].destination_offset = offset + (uint32_t)i * GD_BLOCK_SIZE;
    *count = kept;
    if (bytes) *appended = (GD_Missing){destination, dst->epoch, start, bytes};
    return GD_OK;
}

GD_Result GD_read(GD_Store* store, unsigned slot, uint64_t epoch,
                  uint32_t offset, void* destination, size_t length)
{
    GD_Part* part;
    size_t i;
    unsigned char* dst = (unsigned char*)destination;
    if (slot >= GD_PARTITIONS || (!destination && length)) return store->result = GD_INVALID;
    part = &store->parts[slot];
    if (epoch != part->epoch) return store->result = GD_STALE;
    if (offset > part->capacity || length > part->capacity - offset) return store->result = GD_INVALID;
    for (i = 0; i < length; ++i) {
        uint32_t const at = offset + (uint32_t)i;
        GD_Block* block = &part->blocks[at / GD_BLOCK_SIZE];
        if (!GD_present(block, at % GD_BLOCK_SIZE)) {
            size_t missing = 1;
            while (i + missing < length) {
                uint32_t const next = offset + (uint32_t)(i + missing);
                if (GD_present(&part->blocks[next / GD_BLOCK_SIZE], next % GD_BLOCK_SIZE)) break;
                ++missing;
            }
            store->missing.partition = slot; store->missing.epoch = epoch;
            store->missing.offset = at; store->missing.length = (uint32_t)missing;
            return store->result = GD_MISSING;
        }
    }
    if (length) memcpy(dst, part->data + offset, length);
    return store->result = GD_OK;
}

GD_Result GD_rotate(GD_Store* store, unsigned tier, uint64_t epoch,
                    unsigned destination, uint64_t destination_epoch,
                    const GD_Move* moves, size_t count)
{
    unsigned source;
    GD_Part *src, *dst;
    unsigned char* selected;
    uint32_t extent;
    size_t i;
    if (tier >= 3 || destination >= GD_PARTITIONS || (count && !moves)) return GD_INVALID;
    source = GD_committed(store, tier);
    src = &store->parts[source]; dst = &store->parts[destination];
    if (src->epoch != epoch) return GD_STALE;
    if (count && dst->epoch != destination_epoch) return GD_STALE;
    if (epoch > UINT64_MAX - GD_PARTITIONS) return GD_INVALID;
    if (count && !(tier == 0 && destination == GD_prepare(store, 0)) &&
        !(destination / 2 < tier && destination == GD_prepare(store, destination / 2))) return GD_INVALID;
    if (count > src->block_count) return GD_INVALID;
    selected = count ? (unsigned char*)calloc(src->block_count, 1) : NULL;
    if (count && !selected) return GD_NOMEM;
    extent = dst->extent;
    /* Reject the entire plan before copying bytes or invalidating the source. */
    for (i = 0; i < count; ++i) {
        uint32_t const b = moves[i].source_block;
        uint32_t const at = moves[i].destination_offset;
        uint32_t const reserved = at <= dst->capacity ? MIN(GD_BLOCK_SIZE, dst->capacity - at) : 0;
        if (b >= src->block_count || selected[b] || at % GD_BLOCK_SIZE ||
            at < extent || !reserved ||
            dst->blocks[at / GD_BLOCK_SIZE].used) {
            free(selected); return GD_INVALID;
        }
        if (src->blocks[b].used > reserved) {
            free(selected); return GD_CAPACITY;
        }
        selected[b] = 1;
        extent = at + reserved;
    }
    /* Allocate sparse RX readiness before any payload mutation. Missing
     * source bytes remain holes and can be repaired at the destination epoch. */
    for (i = 0; i < count; ++i) {
        GD_Block* block = &dst->blocks[moves[i].destination_offset / GD_BLOCK_SIZE];
        GD_Block* original = &src->blocks[moves[i].source_block];
        if (!GD_prepareBlock(store, block, !store->sender || original->present != NULL)) { free(selected); return GD_NOMEM; }
    }
    for (i = 0; i < count; ++i) {
        uint32_t const from = moves[i].source_block * GD_BLOCK_SIZE;
        uint32_t const at = moves[i].destination_offset;
        GD_Block* a = &src->blocks[moves[i].source_block];
        GD_Block* b = &dst->blocks[at / GD_BLOCK_SIZE];
        unsigned char* const bitmap = b->present;
        uint64_t regions = a->hit_regions;
        unsigned copied = 0, covered = 0;
        if (!a->present) {
            if (a->used) memcpy(dst->data + at, src->data + from, a->used);
            copied = a->used;
        } else {
            unsigned j = 0;
            while (j < a->used) {
                unsigned start;
                while (j < a->used && !GD_present(a, j)) ++j;
                start = j;
                while (j < a->used && GD_present(a, j)) ++j;
                if (j > start) memcpy(dst->data + at + start, src->data + from + start, j - start);
                copied += j - start;
            }
        }
        store->stats.payload_written += copied;
        store->stats.payload_relocated += copied;
        *b = *a;
        b->present = bitmap;
        if (a->present) memcpy(bitmap, a->present, GD_BLOCK_SIZE / 8);
        else if (bitmap) {
            free(bitmap); b->present = NULL;
            store->stats.metadata_allocated -= GD_BLOCK_SIZE / 8;
        }
        while (regions) { regions &= regions - 1; covered += 64; }
        store->stats.payload_transferred += MIN(GD_BLOCK_SIZE, dst->capacity - at);
        store->stats.transferred_referenced_upper += MIN(covered, a->used);
        store->stats.transferred_padding += MIN(GD_BLOCK_SIZE, dst->capacity - at) - a->used;
        GD_indexRange(store, dst, at, at + a->used);
    }
    if (count) dst->extent = extent;
    /* Retire logical validity only. Backing bytes stay owned by this half. */
    for (i = 0; i < src->block_count; ++i) GD_clearBlock(store, &src->blocks[i]);
    if (src->index) memset(src->index, 0, ((size_t)1 << src->hash_log) * src->ways * sizeof(uint32_t));
    src->extent = 0;
    src->unclaimed_count = 0; src->unclaimed_bytes = 0; src->scan_cursor = 0;
    src->epoch += GD_PARTITIONS;
    store->prepare[tier] = source;
    free(selected);
    return GD_OK;
}

static size_t GD_matchLength(const GD_Part* part,
                             uint32_t offset, const unsigned char* src, size_t length)
{
    size_t matched = 0;
    while (matched < length && offset < part->capacity) {
        GD_Block* block = &part->blocks[offset / GD_BLOCK_SIZE];
        unsigned const in = offset % GD_BLOCK_SIZE;
        size_t available, same;
        if (in >= block->used) break;
        available = MIN(length - matched, block->used - in);
        available = MIN(available, part->capacity - offset);
        if (block->present) {
            size_t ready = 0;
            while (ready < available && GD_present(block, in + (unsigned)ready)) ++ready;
            available = ready;
            if (!available) break;
        }
        same = ZSTD_count(src + matched, part->data + offset, src + matched + available);
        matched += same; offset += (uint32_t)same;
        if (same < available) break;
    }
    return matched;
}

static size_t GD_lookup(const GD_Part* part, const unsigned char* src,
                        size_t length, uint32_t* offset)
{
    size_t best = 0;
    uint32_t hash;
    size_t row;
    unsigned way;
    if (length < 8 || !part->extent) return 0;
    hash = (uint32_t)ZSTD_hashPtr(src, 32, 8);
    row = (size_t)(hash >> (32 - part->hash_log)) * part->ways;
    for (way = 0; way < part->ways; ++way) {
        size_t same;
        uint32_t position;
        if (!part->index[row + way] || part->tags[row + way] != (uint16_t)hash) continue;
        position = part->index[row + way] - 1;
        same = GD_matchLength(part, position, src, length);
        if (same >= 8 && same > best) { best = same; *offset = position; }
        if (best == length) break;
    }
    return best;
}

static unsigned GD_slot(const GD_Store* store, unsigned priority)
{
    return priority & 1 ? GD_prepare(store, priority / 2) : GD_committed(store, priority / 2);
}

GD_Result GD_setSegmentRatio(GD_Store* store, unsigned percent)
{
    if (!store || !store->sender || !percent || percent > 100) return GD_INVALID;
    store->segment_ratio = percent;
    return GD_OK;
}

GD_Result GD_setUnclaimedWindow(GD_Store* store, uint32_t positions)
{
    if (!store || !store->sender || positions > GD_MAX_FRAME) return GD_INVALID;
    store->unclaimed_window = positions;
    return GD_OK;
}

static GD_Discovery GD_discoveryBudget(const GD_Store* store, size_t length)
{
    GD_Discovery budget;
    unsigned slot;
    for (slot = 0; slot < GD_PARTITIONS; ++slot)
        budget.remaining[slot] = (uint32_t)MIN(store->unclaimed_window, store->parts[slot].unclaimed_bytes);
    budget.comparisons = 24 * length;
    return budget;
}

/* The temporary hash is of THIS business range, never another payload index.
 * All byte positions sharing a bucket remain candidates; exact bytes decide.
 * Per-half cursors and a shared comparison budget survive retries within a frame. */
static GD_Result GD_discover(GD_Store* store, unsigned slot, const unsigned char* src,
                             uint32_t begin, uint32_t end, GD_Discovery* budget, int* found)
{
    GD_Part* part = &store->parts[slot];
    uint32_t at;
    *found = 0;
    if (end - begin < 8 || !budget->remaining[slot] || !part->unclaimed_count ||
        budget->comparisons < end - begin) return GD_OK;
    budget->comparisons -= end - begin;
    memset(store->input_heads, 0, (1U << GD_INPUT_HASH_LOG) * sizeof(*store->input_heads));
    for (at = end - 7; at-- > begin;) {
        size_t const hash = ZSTD_hashPtr(src + at, GD_INPUT_HASH_LOG, 8);
        store->input_next[at] = store->input_heads[hash];
        store->input_heads[hash] = at + 1;
    }
    while (budget->remaining[slot] && budget->comparisons >= 8 && part->unclaimed_count) {
        size_t low = 0, high = part->unclaimed_count;
        const void* key;
        uint32_t candidate, best_at = 0, best = 0;
        /* Find the first unclaimed interval not behind the persistent cursor. */
        while (low < high) {
            size_t const mid = low + (high - low) / 2;
            if (part->unclaimed[mid].end <= part->scan_cursor) low = mid + 1;
            else high = mid;
        }
        if (low == part->unclaimed_count) { low = 0; part->scan_cursor = part->unclaimed[0].begin; }
        at = MAX(part->scan_cursor, part->unclaimed[low].begin);
        part->scan_cursor = at + 1;
        --budget->remaining[slot]; ++store->stats.unclaimed_scanned;
        key = GD_key(part, at);
        if (!key) continue;
        candidate = store->input_heads[ZSTD_hashPtr(key, GD_INPUT_HASH_LOG, 8)];
        while (candidate && budget->comparisons >= 8) {
            uint32_t const position = candidate - 1;
            size_t same = 0;
            budget->comparisons -= 8;
            if (MEM_read64(key) == MEM_read64(src + position)) {
                same = GD_matchLength(part, at, src + position,
                    MIN(end - position, budget->comparisons));
                budget->comparisons -= same;
                if (same >= 8 && same > best) { best = (uint32_t)same; best_at = position; }
            }
            if (best == end - begin) break;
            candidate = store->input_next[position];
        }
        if (best) {
            uint32_t offset = at;
            /* A cursor may first meet a pattern in its middle. Extend to its
             * actual beginning inside this valid unclaimed range, without a grid. */
            while (best_at > begin && offset > part->unclaimed[low].begin &&
                   budget->comparisons && part->data[offset - 1] == src[best_at - 1]) {
                --offset; --best_at; ++best; --budget->comparisons;
            }
            {
                GD_Result const result = GD_recognize(store, part, offset, offset + best);
                if (result != GD_OK) return result;
            }
            part->scan_cursor = offset + best;
            budget->remaining[slot] -= MIN(budget->remaining[slot], best - 1);
            *found = 1;
            return GD_OK;
        }
    }
    return GD_OK;
}

const GD_Match* GD_matches(const GD_Store* store, size_t* count)
{
    *count = store->match_count;
    return store->matches;
}

/* Enumerate variable-length, byte-verified matches in one local dictionary.
 * Skipping a match is greedy; unmatched prefixes never suppress later patterns. */
static size_t GD_scan(GD_Store* store, unsigned slot, const unsigned char* src,
                      uint32_t begin, uint32_t end)
{
    size_t count = 0;
    uint32_t at = begin;
    while (end - at >= 8) {
        uint32_t offset = 0;
        size_t const length = GD_lookup(&store->parts[slot], src + at, end - at, &offset);
        if (length) {
            GD_Match* match = &store->trial[count++];
            match->source_offset = at; match->dictionary_offset = offset;
            match->length = (uint32_t)length; match->partition = slot;
            at += (uint32_t)length;
        } else ++at;
    }
    return count;
}

/* Longest interval between match boundaries under a linear byte-cost estimate.
 * Literal gaps cost one byte; a sequence costs eight and a frame sixteen.
 * A decreasing prefix stack finds the longest qualifying interval in O(matches),
 * without trying every pair of cut points. Actual encoded size is checked later.
 * This is a bounded heuristic, not an optimal zstd parse or an exact cost oracle. */
static int GD_longest(GD_Store* store, size_t count, size_t* first, size_t* last)
{
    int64_t saved = 0;
    size_t i, top = 0;
    uint32_t longest = 0;
    for (i = 0; i < count; ++i) {
        GD_Match const m = store->trial[i];
        store->starts[i] = 100 * saved - (int64_t)(100 - store->segment_ratio) * m.source_offset;
        saved += m.length - 8;
        store->ends[i] = 100 * saved -
            (int64_t)(100 - store->segment_ratio) * (m.source_offset + m.length);
        if (!top || store->starts[i] < store->starts[store->minima[top - 1]])
            store->minima[top++] = (uint32_t)i;
    }
    for (i = count; i-- > 0 && top;) {
        while (top && store->ends[i] - store->starts[store->minima[top - 1]] >= 1600) {
            size_t const start = store->minima[--top];
            if (start <= i) {
                uint32_t const length = store->trial[i].source_offset + store->trial[i].length -
                    store->trial[start].source_offset;
                if (length > longest || (length == longest && start < *first)) {
                    longest = length; *first = start; *last = i;
                }
            }
        }
    }
    return longest != 0;
}

static size_t GD_encodeRegion(GD_Store* store, ZSTD_CCtx* context, void* dst,
                              size_t capacity, const unsigned char* src,
                              size_t first, size_t last)
{
    GD_Match const initial = store->trial[first];
    unsigned const slot = initial.partition;
    uint32_t const begin = initial.source_offset;
    uint32_t at = begin;
    size_t i, count = 0;
    for (i = first; i <= last; ++i) {
        GD_Match const m = store->trial[i];
        ZSTD_Sequence* seq = &store->sequences[count++];
        seq->litLength = m.source_offset - at;
        seq->matchLength = m.length;
        seq->offset = store->parts[slot].capacity + (m.source_offset - begin) - m.dictionary_offset;
        seq->rep = 0;
        at = m.source_offset + m.length;
    }
    memset(&store->sequences[count++], 0, sizeof(*store->sequences));
    FORWARD_IF_ERROR(ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters), "");
    FORWARD_IF_ERROR(ZSTD_CCtx_setParameter(context, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters), "");
    return ZSTD_compressSequencesWithExternalDictSize(context, dst, capacity,
        store->sequences, count, src + begin, at - begin,
        store->parts[slot].capacity, GD_DICTIONARY_ID_BASE + slot);
}

static void GD_recordMatch(GD_Store* store, const GD_Match* match, int track_usage)
{
    unsigned const slot = match->partition;
    uint32_t b, last = (match->dictionary_offset + match->length - 1) / GD_BLOCK_SIZE;
    ++store->stats.matches[slot / 2];
    store->stats.matched_bytes[slot / 2] += match->length;
    for (b = match->dictionary_offset / GD_BLOCK_SIZE; track_usage && b <= last; ++b) {
        GD_Block* block = &store->parts[slot].blocks[b];
        uint32_t const base = b * GD_BLOCK_SIZE;
        uint32_t const begin = MAX(match->dictionary_offset, base);
        uint32_t const end = MIN(match->dictionary_offset + match->length, base + GD_BLOCK_SIZE);
        unsigned const first_region = (begin - base) / 64, last_region = (end - base - 1) / 64;
        block->hits += block->hits != UINT64_MAX;
        if (!block->reused_bytes) {
            block->reused_begin = begin - base; block->reused_end = end - base;
        } else {
            block->reused_begin = MIN(block->reused_begin, begin - base);
            block->reused_end = MAX(block->reused_end, end - base);
        }
        block->reused_bytes += MIN((uint64_t)(end - begin), UINT64_MAX - block->reused_bytes);
        block->hit_regions |= (UINT64_MAX << first_region) & (UINT64_MAX >> (63 - last_region));
    }
}

static size_t GD_novelRanges(GD_Store* store, const unsigned char* src, size_t length, size_t* total)
{
    size_t at = 0, anchor = 0, count = 0;
    *total = 0;
    while (at + 8 <= length) {
        size_t matched = 0;
        unsigned priority;
        for (priority = 0; priority < GD_PARTITIONS && !matched; ++priority) {
            uint32_t offset;
            matched = GD_lookup(&store->parts[GD_slot(store, priority)], src + at, length - at, &offset);
        }
        if (!matched) { ++at; continue; }
        if (at - anchor >= 8) {
            store->trial[count].source_offset = (uint32_t)anchor;
            store->trial[count++].length = (uint32_t)(at - anchor);
            *total += at - anchor;
        }
        at += matched; anchor = at;
    }
    if (length - anchor >= 8) {
        store->trial[count].source_offset = (uint32_t)anchor;
        store->trial[count++].length = (uint32_t)(length - anchor);
        *total += length - anchor;
    }
    return count;
}

GD_Result GD_learn(GD_Store* store, const void* source, size_t length,
                   GD_Missing* learned)
{
    const unsigned char* src = (const unsigned char*)source;
    GD_Part* part;
    GD_Discovery budget;
    size_t count, i, total;
    uint64_t recognized;
    if (!store || !store->sender || !source || !length || length > GD_MAX_FRAME || !learned) return GD_INVALID;
    store->match_count = 0;
    memset(learned, 0, sizeof(*learned));
    learned->partition = GD_prepare(store, 2);
    part = &store->parts[learned->partition];
    learned->epoch = part->epoch; learned->offset = part->extent;
    budget = GD_discoveryBudget(store, length);
    recognized = store->stats.payload_recognized;
    count = GD_novelRanges(store, src, length, &total);
    for (i = 0; i < count;) {
        unsigned priority;
        int found = 0;
        uint32_t const begin = store->trial[i].source_offset, end = begin + store->trial[i].length;
        for (priority = 0; priority < GD_PARTITIONS && !found; ++priority) {
            GD_Result const r = GD_discover(store, GD_slot(store, priority), src, begin, end, &budget, &found);
            if (r != GD_OK) return r;
        }
        if (found) {
            if (budget.comparisons < length) break;
            budget.comparisons -= length;
            count = GD_novelRanges(store, src, length, &total); i = 0;
        } else {
            ++i;
        }
    }
    if (recognized != store->stats.payload_recognized) count = GD_novelRanges(store, src, length, &total);
    if (total > part->capacity - part->extent) return GD_CAPACITY;
    for (i = 0; i < count; ++i) {
        uint32_t offset;
        GD_Result const result = GD_append(store, 2, src + store->trial[i].source_offset,
                                            store->trial[i].length, &offset);
        if (result != GD_OK) return result;
        learned->length += store->trial[i].length;
    }
    return GD_OK;
}

static int GD_pieceOrder(const void* a, const void* b)
{
    uint32_t const x = ((const GD_Piece*)a)->begin, y = ((const GD_Piece*)b)->begin;
    return (x > y) - (x < y);
}
static int GD_matchOrder(const void* a, const void* b)
{
    uint32_t const x = ((const GD_Match*)a)->source_offset, y = ((const GD_Match*)b)->source_offset;
    return (x > y) - (x < y);
}

static size_t GD_plain(ZSTD_CCtx* context, void* dst, size_t capacity,
                       const void* src, size_t length)
{
    FORWARD_IF_ERROR(ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters), "");
    return ZSTD_compressCCtx(context, dst, capacity, src, length, 3);
}

size_t GD_compress(GD_Store* store, ZSTD_CCtx* context, void* dst, size_t capacity,
                   const void* src, size_t length, GD_FrameView* view)
{
    return GD_compressTracked(store, context, dst, capacity, src, length, view, 1);
}

size_t GD_compressTracked(GD_Store* store, ZSTD_CCtx* context, void* dst, size_t capacity,
                         const void* source, size_t length, GD_FrameView* view, int track_usage)
{
    const unsigned char* src = (const unsigned char*)source;
    unsigned char* out = (unsigned char*)dst;
    size_t const scratch_capacity = ZSTD_compressBound(GD_MAX_FRAME);
    size_t pending = 1, pieces = 0, encoded = 0, selected = 0, i, at = 0, written = 0;
    size_t budget = 24 * length; /* Shared across every region and all six indexes. */
    GD_Discovery discovery;
    size_t result;
    if (store) { store->match_count = 0; store->result = GD_INVALID; }
    if (view) memset(view, 0, sizeof(*view));
    if (!store || !store->sender || !context || !dst || !view || (!src && length) || length > GD_MAX_FRAME)
        return ERROR(GENERIC);
    store->result = GD_OK;
    discovery = GD_discoveryBudget(store, length);
    for (i = 0; i < GD_PARTITIONS; ++i) view->epoch[i] = store->parts[i].epoch;
    store->pending[0].begin = 0; store->pending[0].end = (uint32_t)length;
    store->pending[0].priority = 0;
    while (pending) {
        GD_Pending range = store->pending[--pending];
        unsigned slot;
        size_t count, first = 0, last = 0, n;
        uint32_t begin, end;
        while (range.priority < 2 * GD_PARTITIONS) {
            GD_Part* part = &store->parts[GD_slot(store, range.priority % GD_PARTITIONS)];
            if (range.priority < GD_PARTITIONS ? part->extent != 0 : part->unclaimed_count != 0) break;
            ++range.priority;
        }
        if (range.priority == 2 * GD_PARTITIONS || range.end - range.begin < 8 ||
            range.end - range.begin > budget) continue;
        slot = GD_slot(store, range.priority % GD_PARTITIONS);
        if (range.priority >= GD_PARTITIONS) {
            int found;
            GD_Result const r = GD_discover(store, slot, src, range.begin, range.end, &discovery, &found);
            if (r != GD_OK) { store->result = r; return ERROR(memory_allocation); }
            range.priority = found ? 0 : range.priority + 1;
            store->pending[pending++] = range;
            continue;
        }
        budget -= range.end - range.begin;
        count = GD_scan(store, slot, src, range.begin, range.end);
        if (!GD_longest(store, count, &first, &last)) {
            ++range.priority; store->pending[pending++] = range; continue;
        }
        begin = store->trial[first].source_offset;
        end = store->trial[last].source_offset + store->trial[last].length;
        n = GD_encodeRegion(store, context, store->encoded + scratch_capacity,
                            scratch_capacity, src, first, last);
        if (ZSTD_isError(n)) { store->result = GD_CODEC; return n; }
        if (n * 100 > (size_t)(end - begin) * store->segment_ratio) {
            ++range.priority; store->pending[pending++] = range; continue;
        }
        assert(pieces < GD_MAX_MATCHES && encoded + n <= scratch_capacity &&
               selected + last - first + 1 < GD_MAX_MATCHES);
        store->pieces[pieces].begin = begin; store->pieces[pieces].end = end;
        store->pieces[pieces].offset = (uint32_t)encoded; store->pieces[pieces++].size = (uint32_t)n;
        memcpy(store->encoded + encoded, store->encoded + scratch_capacity, n); encoded += n;
        memcpy(store->matches + selected, store->trial + first, (last - first + 1) * sizeof(*store->matches));
        selected += last - first + 1;
        if (end < range.end) {
            store->pending[pending] = range; store->pending[pending++].begin = end;
        }
        if (begin > range.begin) {
            store->pending[pending] = range; store->pending[pending++].end = begin;
        }
        assert(pending <= GD_MAX_MATCHES);
    }
    if (!pieces) goto plain;
    qsort(store->pieces, pieces, sizeof(*store->pieces), GD_pieceOrder);
    for (i = 0; i <= pieces; ++i) {
        size_t const end = i < pieces ? store->pieces[i].begin : length;
        if (end > at) {
            result = GD_plain(context, out + written, capacity - written, src + at, end - at);
            if (ZSTD_isError(result)) {
                if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) goto plain;
                store->result = GD_CODEC; return result;
            }
            written += result;
        }
        if (i < pieces) {
            GD_Piece const piece = store->pieces[i];
            if (piece.size > capacity - written) goto plain;
            memcpy(out + written, store->encoded + piece.offset, piece.size);
            written += piece.size; at = piece.end;
        }
    }
    qsort(store->matches, selected, sizeof(*store->matches), GD_matchOrder);
    for (i = 0; i < selected; ++i) {
        GD_Match const m = store->matches[i];
        GD_Result const r = GD_recognize(store, &store->parts[m.partition],
            m.dictionary_offset, m.dictionary_offset + m.length);
        if (r != GD_OK) { store->result = r; return ERROR(memory_allocation); }
    }
    store->match_count = selected;
    for (i = 0; i < selected; ++i) {
        view->used_mask |= 1U << store->matches[i].partition;
        GD_recordMatch(store, &store->matches[i], track_usage);
    }
    return written;
plain:
    result = GD_plain(context, dst, capacity, src, length);
    if (ZSTD_isError(result)) store->result = GD_CODEC;
    return result;
}

typedef struct { GD_Store* store; unsigned slot; uint64_t epoch; } GD_Reader;
static size_t GD_readExternal(void* opaque, size_t offset, void* dst, size_t length)
{
    GD_Reader* reader = (GD_Reader*)opaque;
    if (offset > UINT32_MAX ||
        GD_read(reader->store, reader->slot, reader->epoch, (uint32_t)offset, dst, length) != GD_OK)
        return ERROR(dictionary_wrong);
    return 0;
}

size_t GD_decompress(GD_Store* store, ZSTD_DCtx* context, void* dst, size_t capacity,
                     const void* source, size_t length, const GD_FrameView* view)
{
    const unsigned char* src = (const unsigned char*)source;
    unsigned char* out = (unsigned char*)dst;
    size_t at = 0, written = 0;
    unsigned used_mask = 0;
    if (!store || !context || !dst || !src || !length || !view ||
        view->used_mask >> GD_PARTITIONS) return ERROR(GENERIC);
    store->result = GD_OK;
    memset(&store->missing, 0, sizeof(store->missing));
    while (at < length) {
        ZSTD_frameHeader header;
        GD_Reader reader = {store, GD_PARTITIONS, 0};
        size_t frame_size, decoded, dictionary_size = 0;
        size_t const header_result = ZSTD_getFrameHeader(&header, src + at, length - at);
        if (header_result || header.frameType != ZSTD_frame ||
            header.frameContentSize > GD_MAX_FRAME - written ||
            header.frameContentSize > capacity - written ||
            (!header.frameContentSize && (at || ZSTD_findFrameCompressedSize(src, length) != length)))
            goto invalid;
        frame_size = ZSTD_findFrameCompressedSize(src + at, length - at);
        if (ZSTD_isError(frame_size)) goto invalid;
        if (header.dictID) {
            unsigned const slot = header.dictID - GD_DICTIONARY_ID_BASE;
            if (slot >= GD_PARTITIONS || !(view->used_mask & (1U << slot))) goto invalid;
            if (view->epoch[slot] != store->parts[slot].epoch) {
                store->result = GD_STALE; return ERROR(dictionary_wrong);
            }
            reader.slot = slot; reader.epoch = view->epoch[slot];
            dictionary_size = store->parts[slot].capacity;
            used_mask |= 1U << slot;
        }
        decoded = ZSTD_decompressWithExternalDict(context, out + written, capacity - written,
            src + at, frame_size, dictionary_size, header.dictID, GD_readExternal, &reader);
        if (ZSTD_isError(decoded)) {
            if (store->result == GD_OK) store->result = GD_CODEC;
            return decoded;
        }
        if (decoded != header.frameContentSize) goto invalid;
        written += decoded; at += frame_size;
    }
    if (used_mask != view->used_mask) goto invalid;
    return written;
invalid:
    store->result = GD_CODEC;
    return ERROR(corruption_detected);
}
