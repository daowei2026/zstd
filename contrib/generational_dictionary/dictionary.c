/* Experimental SRFEC dictionary research, repository BSD license. */
#include "dictionary.h"
#include "../../lib/zstd_segmented.h"
#include "../../lib/compress/zstd_compress_internal.h"
#include <stdlib.h>
#include <string.h>

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
    unsigned hash_log, ways, stride, offset_bits;
    uint32_t extent, capacity, block_count;
    uint64_t epoch;
    int owns_data;
} GD_Part;
struct GD_Store {
    GD_Part parts[GD_PARTITIONS];
    unsigned prepare[3];
    uint32_t capacity;
    int sender;
    ZSTD_Sequence* sequences;
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

static int GD_prepareBlock(GD_Store* store, GD_Block* block)
{
    if (!store->sender && !block->used && !block->present) {
        block->present = (unsigned char*)calloc(GD_BLOCK_SIZE / 8, 1);
        if (!block->present) return 0;
        store->stats.metadata_allocated += GD_BLOCK_SIZE / 8;
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
    return GD_createWithBuffers(capacities, NULL, sender);
}

GD_Store* GD_createWithBuffers(const uint32_t capacities[3],
                              void* const buffers[GD_PARTITIONS], int sender)
{
    GD_Store* store;
    unsigned slot, tier;
    static const unsigned logs[3] = {20, 19, 18};
    static const unsigned ways[3] = {8, 4, 2};
    if (!capacities) return NULL;
    for (tier = 0; tier < 3; ++tier)
        if (!capacities[tier] || capacities[tier] > (1U << 30) / GD_PARTITIONS) return NULL;
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
    store->stats.metadata_allocated = sizeof(*store);
    if (store->sender) {
        size_t const bytes = (GD_MAX_FRAME / 8 + 1) * sizeof(ZSTD_Sequence);
        store->sequences = (ZSTD_Sequence*)malloc(bytes);
        if (!store->sequences) { GD_free(store); return NULL; }
        store->stats.metadata_allocated += bytes;
    }
    for (slot = 0; slot < GD_PARTITIONS; ++slot) {
        GD_Part* part = &store->parts[slot];
        uint32_t const capacity = capacities[slot / 2];
        unsigned log = logs[slot / 2];
        part->capacity = capacity;
        part->block_count = (capacity + GD_BLOCK_SIZE - 1) / GD_BLOCK_SIZE;
        while (log > 4 && (1U << (log - 1)) >= (capacity + 7) / 8) --log;
        part->hash_log = log;
        part->ways = ways[slot / 2];
        part->stride = capacity < 1048576 ? 1 : (8U << (slot / 2));
        part->offset_bits = 1;
        while (((uint64_t)1 << part->offset_bits) <= capacity) ++part->offset_bits;
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
            if (!part->index) { GD_free(store); return NULL; }
            store->stats.index_allocated += bytes;
        }
    }
    for (slot = 0; slot < 3; ++slot) store->prepare[slot] = 2 * slot + 1;
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
        free(part->blocks); free(part->index);
    }
    free(store->sequences); free(store);
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

static void GD_indexRange(GD_Store* store, GD_Part* part, uint32_t start, uint32_t end)
{
    uint32_t offset;
    if (!store->sender || end < 8) return;
    start = start > 7 ? start - 7 : 0;
    for (offset = start; offset <= end - 8; ++offset) {
        const void* key;
        uint32_t hash, tag;
        uint32_t* row;
        if (offset % part->stride) continue;
        key = GD_key(part, offset);
        if (!key) continue;
        hash = (uint32_t)ZSTD_hashPtr(key, 32, 8);
        tag = hash << part->offset_bits;
        row = part->index + (hash >> (32 - part->hash_log)) * part->ways;
        memmove(row + 1, row, (part->ways - 1) * sizeof(*row));
        row[0] = (offset + 1) | tag;
        ++store->stats.indexed_positions;
    }
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
            if (!GD_prepareBlock(store, &part->blocks[first])) return GD_NOMEM;
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
        if (!GD_prepareBlock(store, block)) { free(selected); return GD_NOMEM; }
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
        same = ZSTD_count(src + matched, part->data + offset, src + matched + available);
        matched += same; offset += (uint32_t)same;
        if (same < available) break;
    }
    return matched;
}

static GD_Result GD_sequencesTracked(GD_Store* store, const void* source, size_t length,
                       ZSTD_Sequence* sequences, size_t capacity, size_t* count,
                       GD_FrameView* view, int track_usage)
{
    const unsigned char* src = (const unsigned char*)source;
    size_t at = 0, anchor = 0, n = 0;
    unsigned slot;
    if (!store->sender || length > GD_MAX_FRAME || (!source && length) || !sequences || !count || !view) return GD_INVALID;
    memset(view, 0, sizeof(*view));
    for (slot = 0; slot < GD_PARTITIONS; ++slot) view->epoch[slot] = store->parts[slot].epoch;
    while (at + 8 <= length) {
        size_t best = 0;
        uint32_t best_offset = 0;
        unsigned best_slot = 0, tier;
        for (tier = 0; tier < 3; ++tier) {
            unsigned which;
            for (which = 0; which < 2; ++which) {
                GD_Part* part;
                uint32_t* row;
                uint32_t hash, mask, tag;
                unsigned way;
                slot = which ? GD_prepare(store, tier) : GD_committed(store, tier);
                part = &store->parts[slot];
                if (!part->extent) continue;
                hash = (uint32_t)ZSTD_hashPtr(src + at, 32, 8);
                mask = ((uint32_t)1 << part->offset_bits) - 1;
                tag = hash << part->offset_bits;
                row = part->index + (hash >> (32 - part->hash_log)) * part->ways;
                for (way = 0; way < part->ways; ++way) {
                    uint32_t offset;
                    size_t same;
                    if (!row[way]) continue;
                    if ((row[way] & ~mask) != tag) continue;
                    offset = (row[way] & mask) - 1;
                    same = GD_matchLength(part, offset, src + at, length - at);
                    if (same >= 8 && same > best) { best = same; best_offset = offset; best_slot = slot; }
                    if (best == length - at) break;
                }
                /* A usable older partition wins at this position even if the
                 * prepare partition could supply a longer match. */
                if (best >= 8) break;
            }
            if (best >= 8) break;
        }
        if (best >= 8) {
            uint32_t b, last;
            if (n == capacity) return GD_CAPACITY;
            sequences[n].litLength = (unsigned)(at - anchor);
            sequences[n].matchLength = (unsigned)best;
            sequences[n].offset = GD_PARTITIONS * store->capacity + (unsigned)at -
                                  (best_slot * store->capacity + best_offset);
            sequences[n].rep = 0;
            ++n;
            view->used_mask |= 1U << best_slot;
            ++store->stats.matches[best_slot / 2];
            store->stats.matched_bytes[best_slot / 2] += best;
            last = (best_offset + (uint32_t)best - 1) / GD_BLOCK_SIZE;
            for (b = best_offset / GD_BLOCK_SIZE; track_usage && b <= last; ++b) {
                GD_Block* block = &store->parts[best_slot].blocks[b];
                uint32_t const base = b * GD_BLOCK_SIZE;
                uint32_t const begin = MAX(best_offset, base), end = MIN(best_offset + (uint32_t)best, base + GD_BLOCK_SIZE);
                unsigned const first_region = (begin - base) / 64;
                unsigned const last_region = (end - base - 1) / 64;
                ++block->hits;
                if (!block->reused_bytes) {
                    block->reused_begin = begin - base; block->reused_end = end - base;
                } else {
                    block->reused_begin = MIN(block->reused_begin, begin - base);
                    block->reused_end = MAX(block->reused_end, end - base);
                }
                block->reused_bytes += MIN((uint64_t)(end - begin), UINT64_MAX - block->reused_bytes);
                block->hit_regions |= (UINT64_MAX << first_region) & (UINT64_MAX >> (63 - last_region));
            }
            at += best; anchor = at;
        } else {
            ++at;
            /* Bound the cost of encrypted/unseen payload. This research limit
             * trades late matches for predictable work; correctness is intact. */
            if (at - anchor >= 128) break;
        }
    }
    if (n == capacity) return GD_CAPACITY;
    memset(&sequences[n], 0, sizeof(sequences[n]));
    sequences[n++].litLength = (unsigned)(length - anchor);
    *count = n;
    return GD_OK;
}

GD_Result GD_sequences(GD_Store* store, const void* source, size_t length,
                       ZSTD_Sequence* sequences, size_t capacity, size_t* count,
                       GD_FrameView* view)
{
    return GD_sequencesTracked(store, source, length, sequences, capacity, count, view, 1);
}

GD_Result GD_learn(GD_Store* store, const void* source, size_t length,
                   GD_Missing* learned)
{
    GD_FrameView view;
    GD_Part* part;
    size_t count, i, total = 0, at = 0;
    GD_Result result;
    if (!store || !store->sender || !source || !length || length > GD_MAX_FRAME || !learned) return GD_INVALID;
    memset(learned, 0, sizeof(*learned));
    learned->partition = GD_prepare(store, 2);
    part = &store->parts[learned->partition];
    learned->epoch = part->epoch;
    learned->offset = part->extent;
    result = GD_sequencesTracked(store, source, length, store->sequences,
        GD_MAX_FRAME / 8 + 1, &count, &view, 0);
    if (result != GD_OK) return result;
    for (i = 0; i < count; ++i)
        if (store->sequences[i].litLength >= 8) total += store->sequences[i].litLength;
    if (total > part->capacity - part->extent) return GD_CAPACITY;
    if (!total) return GD_OK;
    for (i = 0; i < count; ++i) {
        ZSTD_Sequence const seq = store->sequences[i];
        if (seq.litLength >= 8) {
            uint32_t offset;
            result = GD_append(store, 2, (const unsigned char*)source + at, seq.litLength, &offset);
            if (result != GD_OK) return result;
            learned->length += seq.litLength;
        }
        at += seq.litLength + seq.matchLength;
    }
    return GD_OK;
}

size_t GD_compress(GD_Store* store, ZSTD_CCtx* context, void* dst, size_t capacity,
                   const void* src, size_t length, GD_FrameView* view)
{
    return GD_compressTracked(store, context, dst, capacity, src, length, view, 1);
}

size_t GD_compressTracked(GD_Store* store, ZSTD_CCtx* context, void* dst, size_t capacity,
                         const void* src, size_t length, GD_FrameView* view, int track_usage)
{
    /* Stores are serialized owners. Reuse their scratch instead of placing
     * more than 128 KiB on every C thread's stack, including short frames. */
    ZSTD_Sequence* const sequences = store->sequences;
    size_t count;
    GD_Result r = GD_sequencesTracked(store, src, length, sequences,
                               GD_MAX_FRAME / 8 + 1, &count, view, track_usage);
    store->result = r;
    if (r != GD_OK) return ERROR(GENERIC);
    FORWARD_IF_ERROR(ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters), "");
    if (!view->used_mask) return ZSTD_compressCCtx(context, dst, capacity, src, length, 3);
    FORWARD_IF_ERROR(ZSTD_CCtx_setParameter(context, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters), "");
    return ZSTD_compressSequencesWithExternalDictSize(context, dst, capacity,
        sequences, count, src, length, GD_PARTITIONS * (size_t)store->capacity, 0);
}

typedef struct { GD_Store* store; const GD_FrameView* view; } GD_Reader;
static size_t GD_readExternal(void* opaque, size_t offset, void* dst, size_t length)
{
    GD_Reader* reader = (GD_Reader*)opaque;
    GD_Store* store = reader->store;
    unsigned char* out = (unsigned char*)dst;
    while (length) {
        unsigned const slot = (unsigned)(offset / store->capacity);
        uint32_t const at = (uint32_t)(offset % store->capacity);
        size_t const n = MIN(length, store->capacity - at);
        if (slot >= GD_PARTITIONS || !(reader->view->used_mask & (1U << slot))) {
            store->result = GD_INVALID; return ERROR(dictionary_wrong);
        }
        if (GD_read(store, slot, reader->view->epoch[slot], at, out, n) != GD_OK) return ERROR(dictionary_wrong);
        offset += n; out += n; length -= n;
    }
    return 0;
}

size_t GD_decompress(GD_Store* store, ZSTD_DCtx* context, void* dst, size_t capacity,
                     const void* src, size_t length, const GD_FrameView* view)
{
    GD_Reader reader = {store, view};
    size_t result;
    if (!view) return ERROR(GENERIC);
    store->result = GD_OK;
    memset(&store->missing, 0, sizeof(store->missing));
    result = ZSTD_decompressWithExternalDict(context, dst, capacity, src, length,
        GD_PARTITIONS * (size_t)store->capacity, 0, GD_readExternal, &reader);
    if (ZSTD_isError(result) && store->result == GD_OK) store->result = GD_CODEC;
    return result;
}
