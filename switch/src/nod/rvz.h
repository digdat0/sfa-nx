/* SPDX-License-Identifier: CC0-1.0 */
/* RVZ disc images (Dolphin's compressed format) for the Switch disc reader.
 * Why: on desktop Aurora reads RVZ through the Rust nod library, which cannot be built for the Switch.
 * gc_disc.c wraps the raw file stream with rvz_wrap() when the file starts with "RVZ\x01", so everything
 * above it (FST, main.dol, per-file reads) sees an ordinary uncompressed GameCube image.
 * Supported: GameCube RVZ with zstd or no compression (Dolphin's default is zstd). Wii, WIA, bzip2 and
 * LZMA images are rejected with an error message telling the player to convert them. */
#pragma once

#include "gc_disc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero when the first 4 bytes of a file are the RVZ magic. */
int rvz_is_magic(const unsigned char magic[4]);

/* Builds a stream `out` that reads the decompressed disc from the RVZ file behind `in`.
 * On success the new stream's close callback frees the reader and, when owns_inner is non-zero, also
 * closes `in`. Returns 0 (and sets the gc_disc error) on failure; `in` is left open in that case. */
int rvz_wrap(const GcStream* in, int owns_inner, GcStream* out);

/* Frees a reader made by rvz_wrap without closing the inner stream (used when opening fails later). */
void rvz_free_keep_inner(GcStream* wrapped);

#ifdef __cplusplus
}
#endif
