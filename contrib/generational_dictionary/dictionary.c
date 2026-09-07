/* Experimental SRFEC dictionary research, repository BSD license. */
#include "dictionary.h"
#include "../../lib/zstd_segmented.h"
#include "../../lib/compress/zstd_compress_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char* data;
    unsigned char* present;
    uint32_t used;
    uint32_t present_count;
    uint64_t hits;
    uint64_t hit_regions;
    uint64_t reused_bytes;
    unsigned char reused_edges;  /* exact start/end coverage, unlike 64-byte bins */
} GD_Block;
typedef struct {
    GD_Block** blocks;
    uint32_t* index;
    unsigned hash_log, ways, stride, offset_bits;
    uint32_t extent, capacity, block_count;
    uint64_t epoch;
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

static GD_Block* GD_newBlock(GD_Store* store)
{
    GD_Block* block = (GD_Block*)calloc(1, sizeof(*block));
    if (!block) return NULL;
    block->data = (unsigned char*)malloc(GD_BLOCK_SIZE);
    if (!store->sender) block->present = (unsigned char*)calloc(GD_BLOCK_SIZE / 8, 1);
    if (!block->data || (!store->sender && !block->present)) {
        free(block->data); free(block->present); free(block); return NULL;
    }
    store->stats.payload_allocated += GD_BLOCK_SIZE;
    store->stats.metadata_allocated += sizeof(*block) + (block->present ? GD_BLOCK_SIZE / 8 : 0);
    return block;
}

static void GD_freeBlock(GD_Store* store, GD_Block* block)
{
    if (!block) return;
    store->stats.payload_allocated -= GD_BLOCK_SIZE;
    store->stats.payload_freed += GD_BLOCK_SIZE;
    store->stats.metadata_allocated -= sizeof(*block) + (block->present ? GD_BLOCK_SIZE / 8 : 0);
    free(block->data); free(block->present); free(block);
}

GD_Store* GD_create(uint32_t capacity, int sender)
{
    uint32_t capacities[3] = {capacity, capacity, capacity};
    return GD_createWithCapacities(capacities, sender);
}

GD_Store* GD_createWithCapacities(const uint32_t capacities[3], int sender)
{
    GD_Store* store;
    unsigned slot, tier;
    static const unsigned logs[3] = {20, 19, 18};
    static const unsigned ways[3] = {8, 4, 2};
    if (!capacities) return NULL;
    for (tier = 0; tier < 3; ++tier)
        if (!capacities[tier] || capacities[tier] > (1U << 30) / GD_PARTITIONS) return NULL;
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
        part->blocks = (GD_Block**)calloc(part->block_count, sizeof(*part->blocks));
        if (!part->blocks) { GD_free(store); return NULL; }
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
        if (part->blocks) for (i = 0; i < part->block_count; ++i) GD_freeBlock(store, part->blocks[i]);
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
    for (b = 0; b < part->block_count; ++b) {
        GD_Block* block = part->blocks[b];
        uint64_t regions;
        unsigned covered = 0;
        if (!block) continue;
        stats->payload_allocated += GD_BLOCK_SIZE;
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
    if (p >= GD_PARTITIONS || b >= s->parts[p].block_count || !s->parts[p].blocks[b]) return NULL;
    return s->parts[p].blocks[b]->data;
}
uint64_t GD_blockHits(const GD_Store* s, unsigned p, unsigned b)
{
    if (p >= GD_PARTITIONS || b >= s->parts[p].block_count || !s->parts[p].blocks[b]) return 0;
    return s->parts[p].blocks[b]->hits;
}

static unsigned GD_continuity(const GD_Part* part, uint32_t b)
{
    const GD_Block* block = part->blocks[b];
    unsigned score = 0;
    if ((block->reused_edges & 1) && b && part->blocks[b-1] &&
        (part->blocks[b-1]->reused_edges & 2)) ++score;
    if ((block->reused_edges & 2) && b+1 < part->block_count && part->blocks[b+1] &&
        (part->blocks[b+1]->reused_edges & 1)) ++score;
    return score;
}

static int GD_lessRetained(const GD_Part* part, uint32_t a, uint32_t b)
{
    uint64_t const ah = part->blocks[a]->reused_bytes, bh = part->blocks[b]->reused_bytes;
    unsigned ac, bc;
    /* Every move retains a full allocation, so the denominator is constant. */
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
        if (!part->blocks[b] || !part->blocks[b]->reused_bytes) continue;
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
static const void* GD_key(const GD_Part* part,
                          uint32_t offset, unsigned char scratch[8])
{
    GD_Block* block;
    unsigned in, i;
    if (offset > part->capacity || part->capacity - offset < 8) return NULL;
    block = part->blocks[offset / GD_BLOCK_SIZE];
    in = offset % GD_BLOCK_SIZE;
    if (!block) return NULL;
    if (!block->present && in + 8 <= block->used) return block->data + in;
    for (i = 0; i < 8; ++i) {
        uint32_t const at = offset + i;
        GD_Block* b = part->blocks[at / GD_BLOCK_SIZE];
        if (!GD_present(b, at % GD_BLOCK_SIZE)) return NULL;
        scratch[i] = b->data[at % GD_BLOCK_SIZE];
    }
    return scratch;
}

static void GD_indexRange(GD_Store* store, GD_Part* part, uint32_t start, uint32_t end)
{
    uint32_t offset;
    if (!store->sender || end < 8) return;
    start = start > 7 ? start - 7 : 0;
    for (offset = start; offset <= end - 8; ++offset) {
        unsigned char scratch[8];
        const void* key;
        uint32_t hash, tag;
        uint32_t* row;
        if (offset % part->stride) continue;
        key = GD_key(part, offset, scratch);
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
        GD_Block* block = part->blocks[at / GD_BLOCK_SIZE];
        unsigned const in = at % GD_BLOCK_SIZE;
        if (GD_present(block, in)) {
            if (block->data[in] != src[i]) return GD_CONFLICT;
        } else if (store->sender && at < old_extent) {
            return GD_INVALID; /* A promotion gap is never rewritten. */
        }
    }
    if (length) {
        uint32_t first = offset / GD_BLOCK_SIZE;
        uint32_t const last = (offset + (uint32_t)length - 1) / GD_BLOCK_SIZE;
        for (; first <= last; ++first) {
            if (!part->blocks[first]) {
                part->blocks[first] = GD_newBlock(store);
                if (!part->blocks[first]) return GD_NOMEM;
            }
        }
    }
    for (i = 0; i < length; ++i) {
        uint32_t const at = offset + (uint32_t)i;
        GD_Block* block = part->blocks[at / GD_BLOCK_SIZE];
        unsigned const in = at % GD_BLOCK_SIZE;
        if (!GD_present(block, in)) {
            block->data[in] = src[i];
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
        GD_Block* block = part->blocks[at / GD_BLOCK_SIZE];
        if (!GD_present(block, at % GD_BLOCK_SIZE)) {
            size_t missing = 1;
            while (i + missing < length) {
                uint32_t const next = offset + (uint32_t)(i + missing);
                if (GD_present(part->blocks[next / GD_BLOCK_SIZE], next % GD_BLOCK_SIZE)) break;
                ++missing;
            }
            store->missing.partition = slot; store->missing.epoch = epoch;
            store->missing.offset = at; store->missing.length = (uint32_t)missing;
            return store->result = GD_MISSING;
        }
    }
    i = 0;
    while (i < length) {
        uint32_t const at = offset + (uint32_t)i;
        size_t const n = MIN(length - i, GD_BLOCK_SIZE - at % GD_BLOCK_SIZE);
        memcpy(dst + i, part->blocks[at / GD_BLOCK_SIZE]->data + at % GD_BLOCK_SIZE, n);
        i += n;
    }
    return store->result = GD_OK;
}

GD_Result GD_rotate(GD_Store* store, unsigned tier, uint64_t epoch,
                    unsigned destination, uint64_t destination_epoch,
                    const GD_Move* moves, size_t count)
{
    unsigned source;
    GD_Part *src, *dst;
    GD_Block** retained;
    unsigned char* selected;
    uint32_t extent;
    size_t i;
    if (tier >= 3 || destination >= GD_PARTITIONS || (count && !moves)) return GD_INVALID;
    source = GD_committed(store, tier);
    src = &store->parts[source]; dst = &store->parts[destination];
    if (src->epoch != epoch) return GD_STALE;
    if (count && dst->epoch != destination_epoch) return GD_STALE;
    if (epoch > UINT64_MAX - GD_PARTITIONS) return GD_INVALID;
    if (count && !(destination == source && tier == 0) &&
        !(destination / 2 < tier && destination == GD_prepare(store, destination / 2))) return GD_INVALID;
    if (count > src->block_count) return GD_INVALID;
    retained = count ? (GD_Block**)calloc(count, sizeof(*retained)) : NULL;
    selected = (unsigned char*)calloc(src->block_count, 1);
    if ((count && !retained) || !selected) { free(retained); free(selected); return GD_NOMEM; }
    extent = destination == source ? 0 : dst->extent;
    /* Validate the complete ownership transaction before detaching any block. */
    for (i = 0; i < count; ++i) {
        uint32_t const b = moves[i].source_block;
        uint32_t const at = moves[i].destination_offset;
        uint32_t const reserved = at <= dst->capacity ? MIN(GD_BLOCK_SIZE, dst->capacity - at) : 0;
        if (b >= src->block_count || selected[b] || at % GD_BLOCK_SIZE ||
            at < extent || !reserved ||
            (destination != source && dst->blocks[at / GD_BLOCK_SIZE])) {
            free(retained); free(selected); return GD_INVALID;
        }
        if (src->blocks[b] && src->blocks[b]->used > reserved) {
            free(retained); free(selected); return GD_CAPACITY;
        }
        selected[b] = 1;
        retained[i] = src->blocks[b];
        extent = at + reserved;
    }
    for (i = 0; i < count; ++i) src->blocks[moves[i].source_block] = NULL;
    for (i = 0; i < src->block_count; ++i) {
        GD_freeBlock(store, src->blocks[i]); src->blocks[i] = NULL;
    }
    if (src->index) memset(src->index, 0, ((size_t)1 << src->hash_log) * src->ways * sizeof(uint32_t));
    src->extent = 0;
    src->epoch += GD_PARTITIONS;
    store->prepare[tier] = source;
    for (i = 0; i < count; ++i) {
        uint32_t const at = moves[i].destination_offset;
        dst->blocks[at / GD_BLOCK_SIZE] = retained[i];
        if (retained[i]) {
            uint64_t regions = retained[i]->hit_regions;
            unsigned covered = 0;
            while (regions) { regions &= regions - 1; covered += 64; }
            store->stats.payload_transferred += GD_BLOCK_SIZE;
            store->stats.transferred_referenced_upper += MIN(covered, retained[i]->used);
            store->stats.transferred_padding += GD_BLOCK_SIZE - retained[i]->used;
            retained[i]->hits = 0;
            retained[i]->hit_regions = 0;
            retained[i]->reused_bytes = 0;
            retained[i]->reused_edges = 0;
            GD_indexRange(store, dst, at, at + retained[i]->used);
        }
    }
    if (count) dst->extent = extent;
    free(retained); free(selected);
    return GD_OK;
}

static size_t GD_matchLength(const GD_Part* part,
                             uint32_t offset, const unsigned char* src, size_t length)
{
    size_t matched = 0;
    while (matched < length && offset < part->capacity) {
        GD_Block* block = part->blocks[offset / GD_BLOCK_SIZE];
        unsigned const in = offset % GD_BLOCK_SIZE;
        size_t available, same;
        if (!block || in >= block->used) break;
        available = MIN(length - matched, block->used - in);
        available = MIN(available, part->capacity - offset);
        same = ZSTD_count(src + matched, block->data + in, src + matched + available);
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
                GD_Block* block = store->parts[best_slot].blocks[b];
                uint32_t const base = b * GD_BLOCK_SIZE;
                uint32_t const begin = MAX(best_offset, base), end = MIN(best_offset + (uint32_t)best, base + GD_BLOCK_SIZE);
                unsigned const first_region = (begin - base) / 64;
                unsigned const last_region = (end - base - 1) / 64;
                ++block->hits;
                block->reused_bytes += MIN((uint64_t)(end - begin), UINT64_MAX - block->reused_bytes);
                if (begin == base) block->reused_edges |= 1;
                if (end == base + GD_BLOCK_SIZE) block->reused_edges |= 2;
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
    uint32_t b;
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
    /* Allocate the exact destination blocks before writing any literal. */
    for (b = part->extent / GD_BLOCK_SIZE; b <= (part->extent + (uint32_t)total - 1) / GD_BLOCK_SIZE; ++b) {
        if (!part->blocks[b]) {
            part->blocks[b] = GD_newBlock(store);
            if (!part->blocks[b]) return GD_NOMEM;
        }
    }
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
        sequences, count, src, length, GD_PARTITIONS * (size_t)store->capacity);
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
        GD_PARTITIONS * (size_t)store->capacity, GD_readExternal, &reader);
    if (ZSTD_isError(result) && store->result == GD_OK) store->result = GD_CODEC;
    return result;
}
