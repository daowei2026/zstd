/* Experimental SRFEC dictionary research, under the repository's BSD license. */
#ifndef ZSTD_GEN_DICTIONARY_H
#define ZSTD_GEN_DICTIONARY_H
#include <stddef.h>
#include <stdint.h>
#define ZSTD_STATIC_LINKING_ONLY
#include "../../lib/zstd.h"

#define GD_PARTITIONS 6
#define GD_BLOCK_SIZE 4096
#define GD_MAX_FRAME 65535

typedef struct GD_Store GD_Store;
typedef enum {
    GD_OK, GD_CAPACITY, GD_STALE, GD_MISSING, GD_CONFLICT,
    GD_INVALID, GD_NOMEM, GD_CODEC
} GD_Result;
typedef struct {
    uint64_t epoch[GD_PARTITIONS];
    uint32_t used_mask;
} GD_FrameView;
typedef struct {
    unsigned partition;
    uint64_t epoch;
    uint32_t offset;
    uint32_t length;
} GD_Missing;
typedef struct {
    uint32_t source_block;
    uint32_t destination_offset;
} GD_Move;
typedef struct {
    uint64_t payload_allocated;
    uint64_t payload_written;
    uint64_t payload_relocated;
    uint64_t payload_freed;
    uint64_t payload_transferred;
    uint64_t metadata_allocated;
    uint64_t index_allocated;
    uint64_t indexed_positions;
    uint64_t matches[3];
} GD_Stats;

GD_Store* GD_create(uint32_t partition_capacity, int sender);
void GD_free(GD_Store* store);
uint32_t GD_capacity(const GD_Store* store);
unsigned GD_prepare(const GD_Store* store, unsigned tier);
unsigned GD_committed(const GD_Store* store, unsigned tier);
uint64_t GD_epoch(const GD_Store* store, unsigned partition);
uint32_t GD_extent(const GD_Store* store, unsigned partition);
const GD_Stats* GD_stats(const GD_Store* store);
const GD_Missing* GD_missing(const GD_Store* store);
GD_Result GD_lastResult(const GD_Store* store);
const void* GD_blockAddress(const GD_Store* store, unsigned partition, unsigned block);
uint64_t GD_blockHits(const GD_Store* store, unsigned partition, unsigned block);

/* Receiver writes may arrive out of order. Conflicts never overwrite bytes.
 * Sender append is restricted to prepare; writes are also exposed for replay
 * and benchmark fixtures. The caller performs authentication before either. */
GD_Result GD_write(GD_Store* store, unsigned partition, uint64_t epoch,
                   uint32_t offset, const void* source, size_t length);
GD_Result GD_append(GD_Store* store, unsigned tier, const void* source,
                    size_t length, uint32_t* offset);
GD_Result GD_read(GD_Store* store, unsigned partition, uint64_t epoch,
                  uint32_t offset, void* destination, size_t length);

/* One atomic rotation: selected blocks transfer sole ownership, remaining
 * source blocks are freed, its epoch advances, and prepare becomes committed.
 * For perpetual retention, destination may equal the retiring source slot.
 * Explicit destination offsets make receiver replay independent of holes.
 * An absent receiver block remains a hole at its new destination.
 * The caller supplies the expected retiring epoch for duplicate/stale replay. */
GD_Result GD_rotate(GD_Store* store, unsigned tier, uint64_t retiring_epoch,
                    unsigned destination, uint64_t destination_epoch,
                    const GD_Move* moves, size_t count);

/* Produces standard zstd sequences using the fixed virtual address space of
 * all six partitions. No dictionary payload is flattened for the codec. */
GD_Result GD_sequences(GD_Store* store, const void* source, size_t length,
                       ZSTD_Sequence* sequences, size_t capacity, size_t* count,
                       GD_FrameView* view);
size_t GD_compress(GD_Store* store, ZSTD_CCtx* context,
                   void* destination, size_t capacity, const void* source,
                   size_t length, GD_FrameView* view);
size_t GD_decompress(GD_Store* store, ZSTD_DCtx* context,
                     void* destination, size_t capacity, const void* source,
                     size_t length, const GD_FrameView* view);
#endif
