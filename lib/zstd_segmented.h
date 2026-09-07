/* Experimental fork API. BSD license, as provided in the repository LICENSE.
 * Independent frames with a caller-owned segmented raw-content dictionary.
 * Dictionary identity, availability and lifetime belong to the caller.
 * Statically link this header and the matching fork revision. */
#ifndef ZSTD_SEGMENTED_H
#define ZSTD_SEGMENTED_H
#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include "zstd.h"
/* Leave room for a complete independent frame and the three repeat-offset
 * codes in zstd's 32-bit sequence offset representation. This is an address
 * limit; no dictionary-sized allocation is made by either external API. */
#define ZSTD_EXTERNAL_FRAME_SIZE_MAX 65535U
#define ZSTD_EXTERNAL_DICT_SIZE_MAX ((size_t)0xFFFFFFFFU - ZSTD_EXTERNAL_FRAME_SIZE_MAX - 3U)
#ifdef __cplusplus
extern "C" {
#endif
/* Copy exactly length bytes, returning 0 or a ZSTD error. Missing bytes must
 * produce an error. The callback is borrowed only for this synchronous call. */
typedef size_t (*ZSTD_DictRead)(void* opaque, size_t offset,
                              void* destination, size_t length);
ZSTDLIB_STATIC_API size_t ZSTD_compressSequencesWithExternalDictSize(
    ZSTD_CCtx* cctx, void* dst, size_t dstCapacity,
    const ZSTD_Sequence* sequences, size_t sequenceCount,
    const void* src, size_t srcSize, size_t dictionarySize);
/* One zstd frame, at most ZSTD_EXTERNAL_FRAME_SIZE_MAX decoded bytes, with raw dictionary
 * history. No trained entropy tables or payload are loaded. Resets parameters.
 * Errors may leave partial bytes in dst: only successful output is deliverable. */
ZSTDLIB_STATIC_API size_t ZSTD_decompressWithExternalDict(
    ZSTD_DCtx* dctx, void* dst, size_t dstCapacity,
    const void* src, size_t srcSize, size_t dictionarySize,
    ZSTD_DictRead read, void* opaque);
#ifdef __cplusplus
}
#endif
#endif
