/* Experimental SRFEC dictionary research, under the repository's BSD license. */
#ifndef ZSTD_GEN_DICTIONARY_H
#define ZSTD_GEN_DICTIONARY_H
#include <stddef.h>
#include <stdint.h>
#define ZSTD_STATIC_LINKING_ONLY
#include "../../lib/zstd.h"

#define GD_PARTITIONS 6
#ifndef GD_BLOCK_SIZE
#define GD_BLOCK_SIZE 4096
#endif
#define GD_MAX_FRAME 65535
#define GD_DICTIONARY_ID_BASE 32768U

typedef struct GD_Store GD_Store;
typedef enum {
    GD_OK, GD_CAPACITY, GD_STALE, GD_MISSING, GD_CONFLICT,
    GD_INVALID, GD_NOMEM, GD_CODEC
} GD_Result;
typedef struct {
    uint64_t epoch[GD_PARTITIONS];
    uint32_t used_mask;
} GD_FrameView;
/* Observation of an actually encoded match, in its own half's address space.
 * These transient descriptors neither own payload nor create dictionary objects. */
typedef struct {
    uint32_t source_offset, dictionary_offset, length;
    unsigned partition;
} GD_Match;
typedef struct {
    unsigned partition;
    uint64_t epoch;
    uint32_t offset;
    uint32_t length;
} GD_Missing;
/* Trusted recovery metadata, consumed only during construction. Ranges are
 * sorted by partition/offset, non-overlapping and bounded by that half's extent.
 * RX callers supply only ranges validated against the authoritative sender. */
typedef struct {
    uint64_t epoch[GD_PARTITIONS];
    uint32_t extent[GD_PARTITIONS];
    unsigned prepare[3];
    const GD_Missing* ranges;
    size_t range_count;
} GD_Layout;
typedef struct {
    uint32_t source_block;
    uint32_t destination_offset;
} GD_Move;
typedef struct {
    /* Fixed backing reservation, including borrowed buffers; not resident RAM. */
    uint64_t payload_allocated;
    uint64_t payload_written;
    uint64_t payload_relocated;
    uint64_t payload_freed;
    uint64_t payload_transferred;
    uint64_t transferred_referenced_upper;
    uint64_t transferred_padding;
    uint64_t metadata_allocated;
    uint64_t index_allocated;
    uint64_t indexed_positions;
    uint64_t matches[3];
    uint64_t matched_bytes[3];
    uint64_t payload_peak_allocated;
    /* Candidate starting positions inspected, and payload bytes newly indexed. */
    uint64_t unclaimed_scanned, payload_recognized;
} GD_Stats;

typedef struct {
    uint64_t epoch;
    uint64_t payload_allocated;
    uint64_t present_bytes;
    uint64_t referenced_upper;
    uint32_t capacity;
    uint32_t extent;
    uint64_t unclaimed_bytes;
    uint32_t unclaimed_ranges, scan_cursor;
} GD_PartitionStats;

GD_Store* GD_create(uint32_t partition_capacity, int sender);
/* Each entry is the capacity of EACH of the two independent halves of a tier. */
GD_Store* GD_createWithCapacities(const uint32_t tier_capacities[3], int sender);
/* Borrow six non-overlapping continuous ranges, each of its tier's capacity.
 * They may be the halves of three mmap files. The caller keeps them mapped
 * until GD_free, which never frees or modifies borrowed payload. NULL buffers
 * allocates the same layout internally for standalone codec use. All ranges
 * initially have zero valid bytes unless recovered supplies explicit metadata.
 * Recovery requires borrowed buffers and never writes their bytes. Recovered
 * sender ranges start unclaimed without scanning or building payload indexes. */
GD_Store* GD_createWithBuffers(const uint32_t tier_capacities[3],
                              void* const buffers[GD_PARTITIONS], int sender,
                              const GD_Layout* recovered);
void GD_free(GD_Store* store);
/* Largest local capacity, for caller workspace sizing only. */
uint32_t GD_capacity(const GD_Store* store);
uint32_t GD_partitionCapacity(const GD_Store* store, unsigned partition);
GD_Result GD_observePartition(const GD_Store* store, unsigned partition,
                              GD_PartitionStats* stats);
unsigned GD_prepare(const GD_Store* store, unsigned tier);
unsigned GD_committed(const GD_Store* store, unsigned tier);
uint64_t GD_epoch(const GD_Store* store, unsigned partition);
uint32_t GD_extent(const GD_Store* store, unsigned partition);
const GD_Stats* GD_stats(const GD_Store* store);
const GD_Missing* GD_missing(const GD_Store* store);
GD_Result GD_lastResult(const GD_Store* store);
const void* GD_blockAddress(const GD_Store* store, unsigned partition, unsigned block);
uint64_t GD_blockHits(const GD_Store* store, unsigned partition, unsigned block);

/* Rank committed blocks by tracked reused bytes / actual retained capacity.
 * Retention still reserves GD_BLOCK_SIZE destination bytes per region, including
 * padding. Equal scores prefer referenced ranges meeting at adjacent block
 * edges, then lower offsets. The caller bounds retention and applies epoch-
 * checked GD_rotate; this scan neither appends nor changes payload. */
size_t GD_selectMoves(const GD_Store* store, unsigned tier,
                      uint32_t destination_offset, GD_Move* moves, size_t capacity);

/* During a cross-tier rotation, merge directly overlapping hot ranges from
 * disjoint adjacent selected blocks into destination prepare. Sorted unique
 * source moves are reduced to the ordinary copies, with final offsets.
 * The source is unchanged until GD_rotate. Capacity failure changes neither
 * payload nor moves; required reports space needed in an empty destination.
 * All new bytes form one append range. Allocation failure is session-terminal. */
GD_Result GD_compactMoves(GD_Store* store, unsigned tier, uint64_t source_epoch,
                         unsigned destination, uint64_t destination_epoch,
                         GD_Move* moves, size_t* count, GD_Missing* appended,
                         uint32_t* required);

/* Receiver writes may arrive out of order. Conflicts never overwrite bytes.
 * Sender append is restricted to prepare; writes are also exposed for replay
 * and benchmark fixtures. The caller performs authentication before either. */
GD_Result GD_write(GD_Store* store, unsigned partition, uint64_t epoch,
                   uint32_t offset, const void* source, size_t length);
GD_Result GD_append(GD_Store* store, unsigned tier, const void* source,
                    size_t length, uint32_t* offset);
/* Learn unmatched spans of at least eight bytes into adhoc prepare. Existing
 * dictionary matches are never reinserted and this pass does not add retention
 * heat. The result is one contiguous new range; length zero means no learning.
 * Capacity failure commits no bytes. Allocation failure is terminal for the
 * owning session and must not be treated as successful maintenance. */
GD_Result GD_learn(GD_Store* store, const void* source, size_t length,
                   GD_Missing* learned);
GD_Result GD_read(GD_Store* store, unsigned partition, uint64_t epoch,
                  uint32_t offset, void* destination, size_t length);

/* Copy selected regions into destination prepare, preserving their usage,
 * then invalidate the old committed half and advance its epoch. Payload bytes
 * in the retired half remain untouched; its backing is available for later
 * new-epoch append. Perpetual retains into its current prepare half.
 * Explicit destination offsets make receiver replay independent of holes.
 * Only present receiver source bytes are copied; the rest remain repairable
 * destination holes. Invalid plans change neither payload nor epochs.
 * The caller supplies the expected retiring epoch for duplicate/stale replay. */
GD_Result GD_rotate(GD_Store* store, unsigned tier, uint64_t retiring_epoch,
                    unsigned destination, uint64_t destination_epoch,
                    const GD_Move* moves, size_t count);

/* Research acceptance threshold, 1..100 percent including native frame headers.
 * Default 50 is a prototype candidate, not a deployed SRFEC configuration. */
GD_Result GD_setSegmentRatio(GD_Store* store, unsigned percent);
/* One common per-half discovery window, shared by all regions of a frame.
 * 0 disables discovery; maximum GD_MAX_FRAME. Research default is 4096 starting
 * positions per half, not a payload allocation or compression segment size. */
GD_Result GD_setUnclaimedWindow(GD_Store* store, uint32_t positions);
/* Valid until the next encode/learn call; failed or discarded encodings expose
 * no selected matches. Used by workload attribution and behavioral tests. */
const GD_Match* GD_matches(const GD_Store* store, size_t* count);
/* Concatenated independent native frames, in original byte order. Each frame
 * names one half using Dictionary_ID BASE+partition, or ID 0 for ordinary zstd.
 * Local distances use only that half's fixed capacity, never its growing extent.
 * Epochs remain in the enclosing view. This experimental format replaces the
 * former shared-history encoding; product adoption requires a protocol update. */
size_t GD_compress(GD_Store* store, ZSTD_CCtx* context,
                   void* destination, size_t capacity, const void* source,
                   size_t length, GD_FrameView* view);
/* Re-encoding an existing repetition or an immediately learned frame must not
 * count as another independent reuse when selecting blocks for promotion.
 * Match/byte totals still describe codec work; track_usage controls only the
 * block hits and referenced-region observations used by retention policy. */
size_t GD_compressTracked(GD_Store* store, ZSTD_CCtx* context,
                         void* destination, size_t capacity, const void* source,
                         size_t length, GD_FrameView* view, int track_usage);
size_t GD_decompress(GD_Store* store, ZSTD_DCtx* context,
                     void* destination, size_t capacity, const void* source,
                     size_t length, const GD_FrameView* view);
#endif
