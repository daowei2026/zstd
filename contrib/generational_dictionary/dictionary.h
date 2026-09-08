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
/* Opaque UUID bytes. The sender supplies fresh random UUIDs; the receiver uses
 * those same IDs after authenticated maintenance. The codec never generates,
 * orders or derives a partition from them. All-zero is reserved for no epoch. */
typedef struct { unsigned char bytes[16]; } GD_Epoch;
extern const GD_Epoch GD_NO_EPOCH;
int GD_epochEqual(GD_Epoch a, GD_Epoch b);
typedef struct {
    GD_Epoch epoch[GD_PARTITIONS];
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
    GD_Epoch epoch;
    uint32_t offset;
    uint32_t length;
} GD_Missing;
typedef struct { const void* data; size_t size; } GD_IndexSnapshot;
typedef struct {
    GD_Result index_result;
    uint32_t checksum_failures;
    uint64_t restored_positions, checksum_bytes;
} GD_Recovery;
/* Trusted recovery metadata, consumed only during construction. Ranges are
 * sorted by partition/offset, non-overlapping and bounded by that half's extent.
 * RX callers supply only ranges validated against the authoritative sender. */
typedef struct {
    GD_Epoch epoch[GD_PARTITIONS];
    uint32_t extent[GD_PARTITIONS];
    unsigned prepare[3];
    const GD_Missing* ranges;
    size_t range_count;
    /* Optional six per-half index sections, consumed only by construction.
     * now is Unix seconds; 0 leaves unknown downtime conservatively unaged. */
    const GD_IndexSnapshot* indexes;
    uint64_t now;
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
    GD_Epoch epoch;
    uint64_t payload_allocated;
    uint64_t present_bytes;
    uint64_t referenced_upper;
    uint32_t capacity;
    uint32_t extent;
    uint64_t unclaimed_bytes;
    uint32_t unclaimed_ranges, scan_cursor;
    double heat;
    GD_Recovery recovery;
} GD_PartitionStats;

GD_Store* GD_create(uint32_t partition_capacity, int sender,
                    const GD_Epoch epochs[GD_PARTITIONS]);
/* Each entry is the capacity of EACH of the two independent halves of a tier. */
GD_Store* GD_createWithCapacities(const uint32_t tier_capacities[3], int sender,
                                  const GD_Epoch epochs[GD_PARTITIONS]);
/* Borrow six non-overlapping continuous ranges, each of its tier's capacity.
 * They may be the halves of three mmap files. The caller keeps them mapped
 * until GD_free, which never frees or modifies borrowed payload. NULL buffers
 * allocates the same layout internally for standalone codec use. Layout is
 * required, including explicit nonzero epochs and prepare roles. Nonempty
 * sender extents and all ready ranges require borrowed buffers. A receiver
 * may reserve announced extents in fresh storage, with no ready bytes. Recovery
 * never writes payload; valid sender ranges start unclaimed without a startup
 * index scan. */
GD_Store* GD_createWithBuffers(const uint32_t tier_capacities[3],
                              void* const buffers[GD_PARTITIONS], int sender,
                              const GD_Layout* layout);
void GD_free(GD_Store* store);
/* Largest local capacity, for caller workspace sizing only. */
uint32_t GD_capacity(const GD_Store* store);
uint32_t GD_partitionCapacity(const GD_Store* store, unsigned partition);
GD_Result GD_observePartition(const GD_Store* store, unsigned partition,
                              GD_PartitionStats* stats);
unsigned GD_prepare(const GD_Store* store, unsigned tier);
unsigned GD_committed(const GD_Store* store, unsigned tier);
GD_Epoch GD_epoch(const GD_Store* store, unsigned partition);
uint32_t GD_extent(const GD_Store* store, unsigned partition);
const GD_Stats* GD_stats(const GD_Store* store);
const GD_Missing* GD_missing(const GD_Store* store);
GD_Result GD_lastResult(const GD_Store* store);
const void* GD_blockAddress(const GD_Store* store, unsigned partition, unsigned block);
uint64_t GD_blockHits(const GD_Store* store, unsigned partition, unsigned block);
/* Sender time is supplied once by the owner at a batch boundary (Unix seconds).
 * Backwards observations cannot move the effective clock backwards. Changing
 * half-life rebases existing heat under the old half-life at the current clock. */
void GD_setTime(GD_Store* store, uint64_t now);
GD_Result GD_setHeatPolicy(GD_Store* store, uint32_t half_life_seconds);
/* Per-half portable index sections, not payload or authoritative validity
 * metadata. Caller freezes the owner while sizing/writing, then publishes the
 * ordinary file. Each section includes UUID, checksums, coverage and heat.
 * Construction rejects malformed sections without adopting their index/heat;
 * valid payload remains unclaimed. Payload mismatch discards only affected
 * metadata regions. Allocation failure still fails the whole constructor. */
size_t GD_indexSnapshotSize(const GD_Store* store, unsigned partition);
GD_Result GD_saveIndex(const GD_Store* store, unsigned partition,
                       void* destination, size_t capacity, size_t* written);
/* Export exact ready ranges for the separately persisted validity metadata.
 * Freeze the owner across count/fill. count always reports the required entries;
 * GD_CAPACITY may fill a prefix, which the caller must not publish. No payload
 * bytes are read or changed. Adjacent ready positions form one local range. */
GD_Result GD_exportRanges(const GD_Store* store, GD_Missing* ranges,
                          size_t capacity, size_t* count);

/* RX-only adoption of ranges already verified against authenticated sender
 * checksums by the serialized owner. This changes readiness only: no payload
 * read/write/copy, index construction or heat observation. Ranges must lie in
 * current announced extents; overlap/repetition is idempotent. Validate the
 * entire batch before changing readiness. Allocation failure is terminal for
 * the owner and may leave some verified ranges adopted. The caller bounds the
 * batch's work and owns checksum verification and old-session invalidation. */
GD_Result GD_claimRanges(GD_Store* store, const GD_Missing* ranges, size_t count);
/* Borrow a half's continuous backing for read-only checksum computation. This
 * does not assert byte validity. The owner bounds reads by its fixed epoch and
 * valid-range snapshot, and keeps the store/mapping alive throughout each read. */
const void* GD_payloadBuffer(const GD_Store* store, unsigned partition);
/* RX takeover of an authenticated layout. Clears readiness and adopts UUIDs,
 * extents and roles without changing any payload or allocating another copy.
 * No ranges/indexes may be supplied; separately verified ranges use claimRanges.
 * Invalid layouts leave the old state intact. The owner ends old-session use
 * before this operation and preserves recovery candidate metadata outside C. */
GD_Result GD_adoptReceiver(GD_Store* store, const GD_Layout* layout);

/* Rank committed blocks by current decayed reused bytes / valid payload bytes.
 * Cross-tier eligibility uses the target committed's byte-weighted density,
 * falling back to prepare only if committed has no valid payload. Recompute
 * every call. excluded optionally names already copied source regions, one bit
 * per region, with enough bytes for the source extent. Results are hottest first.
 * Retention reserves GD_BLOCK_SIZE bytes per region, bounded by the physical
 * source half's tail; padding inside a region counts. byte_budget bounds the
 * sum, independently of output capacity. Equal scores prefer adjacent block
 * edges, then lower offsets. The caller bounds retention and applies epoch-
 * checked GD_rotate; this scan neither appends nor changes payload. */
size_t GD_selectMoves(const GD_Store* store, unsigned tier,
                      uint32_t destination_offset, uint32_t byte_budget,
                      GD_Move* moves, size_t capacity,
                      const unsigned char* excluded, size_t excluded_size);

/* During a cross-tier rotation, merge directly overlapping hot ranges from
 * disjoint adjacent selected blocks into destination prepare. Sorted unique
 * source moves are reduced to the ordinary copies, with final offsets.
 * The source is unchanged until GD_rotate. Capacity failure changes neither
 * payload nor moves; required reports space needed in an empty destination.
 * destination_limit is an exclusive local end, at most the half capacity;
 * it lets the owner reserve the second half for perpetual retention.
 * All new bytes form one append range. Allocation failure is session-terminal. */
GD_Result GD_compactMoves(GD_Store* store, unsigned tier, GD_Epoch source_epoch,
                         unsigned destination, GD_Epoch destination_epoch, uint32_t destination_limit,
                         GD_Move* moves, size_t* count, GD_Missing* appended,
                         uint32_t* required);

/* Receiver writes may arrive out of order. Conflicts never overwrite bytes.
 * Sender append is restricted to prepare; writes are also exposed for replay
 * and benchmark fixtures. The caller performs authentication before either. */
GD_Result GD_write(GD_Store* store, unsigned partition, GD_Epoch epoch,
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
GD_Result GD_read(GD_Store* store, unsigned partition, GD_Epoch epoch,
                  uint32_t offset, void* destination, size_t length);

/* Copy one caller-bounded batch from committed into destination prepare,
 * preserving heat and leaving the source UUID, role, index and bytes intact.
 * Perpetual retains into its own prepare; other copies go to an older tier.
 * Explicit destination offsets make receiver replay independent of holes.
 * A copied region reserves at most the remaining physical source half capacity,
 * even when the receiver has not received any bytes of that region yet.
 * Only present receiver source bytes are copied; the rest remain repairable
 * destination holes. Invalid plans change neither payload nor epochs.
 * This operation does not retire or switch either half. The owner serializes
 * batches with its maintenance descriptions and finishes with GD_rotate.
 * The maintenance stream deduplicates replay; repeating a used target offset
 * is invalid, not another observation or copy of the same batch. */
GD_Result GD_copyMoves(GD_Store* store, unsigned tier, GD_Epoch source_epoch,
                       unsigned destination, GD_Epoch destination_epoch,
                       const GD_Move* moves, size_t count);

/* Optionally copy final moves using GD_copyMoves, then invalidate committed
 * and adopt replacement_epoch. With count zero, earlier copied batches stay
 * intact without repeating their writes or heat. Retired backing bytes remain
 * untouched and available for later new-epoch append.
 * The caller supplies expected retiring/destination epochs and a fresh sender
 * UUID. A zero or unchanged replacement is invalid; stale replay cannot retire
 * another lifecycle. The enclosing maintenance stream owns replay sequencing. */
GD_Result GD_rotate(GD_Store* store, unsigned tier, GD_Epoch retiring_epoch,
                    GD_Epoch replacement_epoch, unsigned destination, GD_Epoch destination_epoch,
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
