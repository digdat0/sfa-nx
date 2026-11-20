/* SPDX-License-Identifier: GPL-3.0-or-later */
/* RVZ reader: turns Dolphin's compressed disc format back into plain GameCube disc bytes on demand.
 *
 * File layout (all integers big-endian), as written by Dolphin's WIA/RVZ writer:
 *   0x00 file head  : magic "RVZ\x01", versions, size of the disc struct, ISO size, file size, hashes
 *   0x48 disc struct: disc type (1 = GameCube), compression, chunk size, first 0x80 bytes of the disc,
 *                     then where the raw-data table and the group table live in the file
 *   raw-data table  : which disc ranges are stored and which groups hold them (GameCube: one entry)
 *   group table     : per chunk of the disc: file offset (/4), stored size (+ bit 31 = compressed),
 *                     and the "packed" size (non-zero when the chunk is RVZ-packed, see unpack())
 * Each group decodes to one chunk (chunk_size bytes, 128 KiB by default) of the disc. Decoded chunks are
 * kept in a small cache because Aurora reads files in pieces much smaller than a chunk.
 * Hashes are not checked (Dolphin's own verify does that on the PC). */
#include "rvz.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

enum {
    kFileHeadSize = 0x48,
    kDiscHeadSize = 0x80,       /* disc bytes 0..0x7F are stored in the disc struct itself */
    kDiscStructMin = 0xDC,      /* bytes of the disc struct read here (Dolphin writes exactly this) */
    kRawEntrySize = 24,         /* data offset u64, data size u64, first group u32, group count u32 */
    kGroupEntrySize = 12,       /* RVZ group: offset/4 u32, size u32, packed size u32 */
    kBlockSize = 0x8000,        /* raw data starts are aligned down to this, like Dolphin's reader */
    kCacheSlots = 8,            /* decoded chunks kept (8 x 128 KiB = 1 MiB with the default size) */
    kCompressionNone = 0,
    kCompressionZstd = 5,
    kLfgK = 521,                /* lagged Fibonacci generator used for junk (padding) data */
    kLfgJ = 32,
    kLfgSeedWords = 17,
};

/* One stored disc range; offset/size are already aligned to kBlockSize like Dolphin's Read(). */
typedef struct RvzRaw {
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t group_index;
    uint32_t group_count;
} RvzRaw;

typedef struct RvzGroup {
    uint64_t file_offset; /* already multiplied by 4 */
    uint32_t stored_size; /* bytes in the file; 0 = the chunk is all zeros */
    uint32_t compressed;  /* 1 = zstd-compressed */
    uint32_t packed_size; /* non-zero = RVZ-packed stream of this many bytes */
} RvzGroup;

typedef struct RvzSlot {
    uint32_t group;    /* UINT32_MAX = empty */
    uint64_t last_use; /* for least-recently-used replacement */
    uint8_t* data;     /* chunk_size bytes */
} RvzSlot;

typedef struct RvzReader {
    GcStream inner;
    int owns_inner;
    uint64_t iso_size;
    uint32_t compression;
    uint32_t chunk_size;
    uint8_t disc_head[kDiscHeadSize];
    RvzRaw* raw;
    uint32_t raw_count;
    RvzGroup* groups;
    uint32_t group_count;
    pthread_mutex_t lock; /* reads can come from the DVD worker and other threads; one decoder state */
    ZSTD_DCtx* dctx;
    uint8_t* file_buf; /* stored group bytes read from the card */
    size_t file_cap;
    uint8_t* pack_buf; /* decompressed packed stream, before unpacking */
    size_t pack_cap;
    RvzSlot slots[kCacheSlots];
    uint64_t use_clock;
} RvzReader;

static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

int rvz_is_magic(const unsigned char magic[4]) {
    return magic[0] == 'R' && magic[1] == 'V' && magic[2] == 'Z' && magic[3] == 1;
}

/* Grows a scratch buffer to at least `need` bytes; returns 0 when out of memory. */
static int ensure_buf(uint8_t** buf, size_t* cap, size_t need) {
    if (*cap >= need) {
        return 1;
    }
    uint8_t* p = realloc(*buf, need);
    if (p == NULL) {
        return 0;
    }
    *buf = p;
    *cap = need;
    return 1;
}

/* Reads exactly len bytes at offset from the RVZ file. */
static int read_exact(RvzReader* r, uint64_t offset, void* out, size_t len) {
    return r->inner.read_at(r->inner.user_data, offset, out, len) == (int64_t)len;
}

/* ---- junk data: Dolphin's LaggedFibonacciGenerator, which reproduces the Nintendo SDK's disc padding ---- */

typedef struct Lfg {
    uint32_t buf[kLfgK]; /* kept byte-swapped so the bytes can be copied out in disc order */
    size_t pos;          /* byte position inside buf */
} Lfg;

/* Advances the generator by one full buffer (kLfgK words). */
static void lfg_forward(Lfg* g) {
    for (size_t i = 0; i < kLfgJ; ++i) {
        g->buf[i] ^= g->buf[i + kLfgK - kLfgJ];
    }
    for (size_t i = kLfgJ; i < kLfgK; ++i) {
        g->buf[i] ^= g->buf[i - kLfgJ];
    }
}

/* Converts a generator word to its in-memory output form: the SDK's odd 18-bit shift, then byte-swapped
 * so that copying the buffer out gives big-endian bytes (the Switch and the PC are little-endian). */
static uint32_t lfg_output_word(uint32_t x) {
    x = (x & 0xFF00FFFFu) | ((x >> 2) & 0x00FF0000u);
    return __builtin_bswap32(x);
}

/* Seeds from 17 big-endian words, expands to the full buffer and skips the first 4 buffers. */
static void lfg_seed(Lfg* g, const uint8_t* seed) {
    for (size_t i = 0; i < kLfgSeedWords; ++i) {
        g->buf[i] = rd32(seed + i * 4);
    }
    for (size_t i = kLfgSeedWords; i < kLfgK; ++i) {
        g->buf[i] = (g->buf[i - 17] << 23) ^ (g->buf[i - 16] >> 9) ^ g->buf[i - 1];
    }
    for (size_t i = 0; i < kLfgK; ++i) {
        g->buf[i] = lfg_output_word(g->buf[i]);
    }
    for (int i = 0; i < 4; ++i) {
        lfg_forward(g);
    }
    g->pos = 0;
}

/* Skips count output bytes. */
static void lfg_skip(Lfg* g, size_t count) {
    g->pos += count;
    while (g->pos >= sizeof g->buf) {
        lfg_forward(g);
        g->pos -= sizeof g->buf;
    }
}

/* Writes count output bytes. */
static void lfg_bytes(Lfg* g, uint8_t* out, size_t count) {
    while (count > 0) {
        size_t n = sizeof g->buf - g->pos;
        if (n > count) {
            n = count;
        }
        memcpy(out, (const uint8_t*)g->buf + g->pos, n);
        g->pos += n;
        out += n;
        count -= n;
        if (g->pos == sizeof g->buf) {
            lfg_forward(g);
            g->pos = 0;
        }
    }
}

/* Expands an RVZ-packed stream: a list of records, each a big-endian u32 size; bit 31 set means
 * "junk": a 68-byte seed follows and the bytes are regenerated (aligned to the disc's 32 KiB blocks),
 * otherwise `size` literal bytes follow. data_offset is the disc offset of the chunk's first byte. */
static int unpack(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, uint64_t data_offset) {
    size_t ip = 0;
    size_t op = 0;
    Lfg lfg;
    while (op < out_len) {
        if (in_len - ip < 4) {
            return 0;
        }
        uint32_t size = rd32(in + ip);
        ip += 4;
        const int junk = (size & 0x80000000u) != 0;
        size &= 0x7FFFFFFFu;
        if (size > out_len - op) {
            return 0;
        }
        if (junk) {
            if (in_len - ip < kLfgSeedWords * 4) {
                return 0;
            }
            lfg_seed(&lfg, in + ip);
            ip += kLfgSeedWords * 4;
            lfg_skip(&lfg, (size_t)(data_offset % kBlockSize));
            lfg_bytes(&lfg, out + op, size);
        } else {
            if (in_len - ip < size) {
                return 0;
            }
            memcpy(out + op, in + ip, size);
            ip += size;
        }
        op += size;
        data_offset += size;
    }
    return 1;
}

/* ---- groups ---- */

/* Decodes group gi (len disc bytes starting at disc offset data_offset) into dst. */
static int decode_group(RvzReader* r, uint32_t gi, uint64_t data_offset, uint8_t* dst, size_t len) {
    const RvzGroup* g = &r->groups[gi];
    if (g->stored_size == 0) {
        memset(dst, 0, len); /* Dolphin stores all-zero chunks as empty groups */
        return 1;
    }
    if (!ensure_buf(&r->file_buf, &r->file_cap, g->stored_size) ||
        !read_exact(r, g->file_offset, r->file_buf, g->stored_size)) {
        gc_disc_set_error("RVZ: could not read a compressed chunk");
        return 0;
    }
    const uint8_t* src = r->file_buf;
    size_t src_len = g->stored_size;
    if (g->compressed) {
        if (r->compression != kCompressionZstd) {
            gc_disc_set_error("RVZ: compressed chunk in an image without zstd");
            return 0;
        }
        /* Packed chunks decompress to the packed stream, others straight to the disc bytes. */
        uint8_t* target = dst;
        size_t want = len;
        if (g->packed_size != 0) {
            if (!ensure_buf(&r->pack_buf, &r->pack_cap, g->packed_size)) {
                gc_disc_set_error("RVZ: out of memory");
                return 0;
            }
            target = r->pack_buf;
            want = g->packed_size;
        }
        const size_t got = ZSTD_decompressDCtx(r->dctx, target, want, src, src_len);
        if (ZSTD_isError(got) || got != want) {
            gc_disc_set_error("RVZ: zstd decompression failed");
            return 0;
        }
        if (g->packed_size == 0) {
            return 1;
        }
        src = r->pack_buf;
        src_len = g->packed_size;
    }
    if (g->packed_size != 0) {
        if (!unpack(src, src_len < g->packed_size ? src_len : g->packed_size, dst, len, data_offset)) {
            gc_disc_set_error("RVZ: bad packed chunk");
            return 0;
        }
        return 1;
    }
    if (src_len < len) {
        gc_disc_set_error("RVZ: stored chunk is too short");
        return 0;
    }
    memcpy(dst, src, len);
    return 1;
}

/* Returns the decoded bytes of group gi from the cache, decoding it into the oldest slot on a miss. */
static const uint8_t* get_group(RvzReader* r, uint32_t gi, uint64_t data_offset, size_t len) {
    RvzSlot* victim = &r->slots[0];
    for (int i = 0; i < kCacheSlots; ++i) {
        RvzSlot* s = &r->slots[i];
        if (s->group == gi) {
            s->last_use = ++r->use_clock;
            return s->data;
        }
        if (s->last_use < victim->last_use) {
            victim = s;
        }
    }
    if (victim->data == NULL) {
        victim->data = malloc(r->chunk_size);
        if (victim->data == NULL) {
            gc_disc_set_error("RVZ: out of memory");
            return NULL;
        }
    }
    victim->group = UINT32_MAX; /* invalid until decoded, in case decoding fails */
    if (!decode_group(r, gi, data_offset, victim->data, len)) {
        return NULL;
    }
    victim->group = gi;
    victim->last_use = ++r->use_clock;
    return victim->data;
}

/* GcStream read callback: copies disc bytes [offset, offset+len), decoding chunks as needed. */
static int64_t rvz_read_at(void* user_data, uint64_t offset, void* out, size_t len) {
    RvzReader* r = (RvzReader*)user_data;
    if (offset >= r->iso_size) {
        return 0;
    }
    if (len > r->iso_size - offset) {
        len = (size_t)(r->iso_size - offset);
    }
    uint8_t* dst = (uint8_t*)out;
    size_t done = 0;
    pthread_mutex_lock(&r->lock);
    while (done < len) {
        const uint64_t pos = offset + done;
        size_t n;
        if (pos < kDiscHeadSize) {
            n = kDiscHeadSize - (size_t)pos;
            if (n > len - done) {
                n = len - done;
            }
            memcpy(dst + done, r->disc_head + pos, n);
        } else {
            const RvzRaw* raw = NULL;
            for (uint32_t i = 0; i < r->raw_count; ++i) {
                if (pos >= r->raw[i].data_offset && pos - r->raw[i].data_offset < r->raw[i].data_size) {
                    raw = &r->raw[i];
                    break;
                }
            }
            if (raw == NULL) {
                gc_disc_set_error("RVZ: disc offset not stored in the image");
                break;
            }
            const uint64_t rel = pos - raw->data_offset;
            const uint64_t group_rel = rel / r->chunk_size;
            const uint64_t group_start = group_rel * r->chunk_size; /* relative to the range start */
            uint64_t group_len = raw->data_size - group_start;
            if (group_len > r->chunk_size) {
                group_len = r->chunk_size;
            }
            if (group_rel >= raw->group_count) {
                gc_disc_set_error("RVZ: group index out of range");
                break;
            }
            const uint8_t* data = get_group(r, raw->group_index + (uint32_t)group_rel,
                                            raw->data_offset + group_start, (size_t)group_len);
            if (data == NULL) {
                break;
            }
            const size_t in_group = (size_t)(rel - group_start);
            n = (size_t)group_len - in_group;
            if (n > len - done) {
                n = len - done;
            }
            memcpy(dst + done, data + in_group, n);
        }
        done += n;
    }
    pthread_mutex_unlock(&r->lock);
    if (done == 0 && len != 0) {
        return -1;
    }
    return (int64_t)done;
}

static int64_t rvz_stream_len(void* user_data) { return (int64_t)((RvzReader*)user_data)->iso_size; }

/* Frees everything the reader owns except the inner stream. */
static void free_reader(RvzReader* r) {
    if (r == NULL) {
        return;
    }
    for (int i = 0; i < kCacheSlots; ++i) {
        free(r->slots[i].data);
    }
    free(r->raw);
    free(r->groups);
    free(r->file_buf);
    free(r->pack_buf);
    if (r->dctx != NULL) {
        ZSTD_freeDCtx(r->dctx);
    }
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static void rvz_close(void* user_data) {
    RvzReader* r = (RvzReader*)user_data;
    if (r->owns_inner && r->inner.close != NULL) {
        r->inner.close(r->inner.user_data);
    }
    free_reader(r);
}

void rvz_free_keep_inner(GcStream* wrapped) {
    if (wrapped != NULL && wrapped->read_at == rvz_read_at) {
        free_reader((RvzReader*)wrapped->user_data);
        wrapped->user_data = NULL;
    }
}

/* Reads a table of count*entry_size bytes stored at offset (zstd-compressed when the image uses zstd). */
static uint8_t* read_table(RvzReader* r, uint64_t offset, uint32_t stored, size_t want) {
    uint8_t* stored_buf = malloc(stored ? stored : 1);
    uint8_t* table = malloc(want ? want : 1);
    if (stored_buf == NULL || table == NULL || !read_exact(r, offset, stored_buf, stored)) {
        free(stored_buf);
        free(table);
        return NULL;
    }
    int ok;
    if (r->compression == kCompressionZstd) {
        const size_t got = ZSTD_decompressDCtx(r->dctx, table, want, stored_buf, stored);
        ok = !ZSTD_isError(got) && got == want;
    } else {
        ok = stored >= want;
        if (ok) {
            memcpy(table, stored_buf, want);
        }
    }
    free(stored_buf);
    if (!ok) {
        free(table);
        return NULL;
    }
    return table;
}

/* Fails rvz_wrap with a message, freeing the half-built reader (the inner stream stays open). */
static int fail(RvzReader* r, const char* message) {
    free_reader(r);
    gc_disc_set_error(message);
    return 0;
}

int rvz_wrap(const GcStream* in, int owns_inner, GcStream* out) {
    RvzReader* r = calloc(1, sizeof(RvzReader));
    if (r == NULL) {
        gc_disc_set_error("RVZ: out of memory");
        return 0;
    }
    r->inner = *in;
    r->owns_inner = owns_inner;
    pthread_mutex_init(&r->lock, NULL);
    for (int i = 0; i < kCacheSlots; ++i) {
        r->slots[i].group = UINT32_MAX;
    }

    uint8_t head[kFileHeadSize];
    if (!read_exact(r, 0, head, sizeof head) || !rvz_is_magic(head)) {
        return fail(r, "RVZ: bad file header");
    }
    /* version_compatible: the oldest reader version that can read this file (RVZ 1.0 = 0x01000000). */
    if (rd32(head + 8) > 0x01000000u) {
        return fail(r, "RVZ: image needs a newer reader (re-create it with Dolphin)");
    }
    const uint32_t disc_struct_size = rd32(head + 12);
    r->iso_size = rd64(head + 0x24);
    if (disc_struct_size < kDiscStructMin) {
        return fail(r, "RVZ: disc struct too small");
    }

    uint8_t disc[kDiscStructMin];
    if (!read_exact(r, kFileHeadSize, disc, sizeof disc)) {
        return fail(r, "RVZ: could not read the disc struct");
    }
    const uint32_t disc_type = rd32(disc + 0x00);
    r->compression = rd32(disc + 0x04);
    r->chunk_size = rd32(disc + 0x0C);
    memcpy(r->disc_head, disc + 0x10, kDiscHeadSize);
    /* After the disc head: partition count/size/offset/hash (0x90..0xB3), then the tables. */
    r->raw_count = rd32(disc + 0xB4);
    const uint64_t raw_offset = rd64(disc + 0xB8);
    const uint32_t raw_stored = rd32(disc + 0xC0);
    r->group_count = rd32(disc + 0xC4);
    const uint64_t group_offset = rd64(disc + 0xC8);
    const uint32_t group_stored = rd32(disc + 0xD0);

    if (disc_type != 1) {
        return fail(r, "RVZ: not a GameCube image");
    }
    if (r->compression != kCompressionNone && r->compression != kCompressionZstd) {
        return fail(r, "RVZ: only zstd or uncompressed RVZ is supported (convert it with Dolphin)");
    }
    if (r->chunk_size < kBlockSize || r->chunk_size % kBlockSize != 0 || r->chunk_size > (2u << 20)) {
        return fail(r, "RVZ: unsupported chunk size");
    }
    if (r->raw_count == 0 || r->raw_count > 1024 || r->group_count == 0 || r->group_count > (1u << 24)) {
        return fail(r, "RVZ: implausible table sizes");
    }
    r->dctx = ZSTD_createDCtx();
    if (r->dctx == NULL) {
        return fail(r, "RVZ: out of memory");
    }

    uint8_t* raw = read_table(r, raw_offset, raw_stored, (size_t)r->raw_count * kRawEntrySize);
    if (raw == NULL) {
        return fail(r, "RVZ: could not read the raw data table");
    }
    r->raw = calloc(r->raw_count, sizeof(RvzRaw));
    if (r->raw == NULL) {
        free(raw);
        return fail(r, "RVZ: out of memory");
    }
    for (uint32_t i = 0; i < r->raw_count; ++i) {
        const uint8_t* e = raw + (size_t)i * kRawEntrySize;
        const uint64_t off = rd64(e);
        const uint64_t skipped = off % kBlockSize; /* stored groups start on a 32 KiB boundary */
        r->raw[i].data_offset = off - skipped;
        r->raw[i].data_size = rd64(e + 8) + skipped;
        r->raw[i].group_index = rd32(e + 16);
        r->raw[i].group_count = rd32(e + 20);
        if ((uint64_t)r->raw[i].group_index + r->raw[i].group_count > r->group_count) {
            free(raw);
            return fail(r, "RVZ: raw data entry points past the group table");
        }
    }
    free(raw);

    uint8_t* groups = read_table(r, group_offset, group_stored, (size_t)r->group_count * kGroupEntrySize);
    if (groups == NULL) {
        return fail(r, "RVZ: could not read the group table");
    }
    r->groups = calloc(r->group_count, sizeof(RvzGroup));
    if (r->groups == NULL) {
        free(groups);
        return fail(r, "RVZ: out of memory");
    }
    for (uint32_t i = 0; i < r->group_count; ++i) {
        const uint8_t* e = groups + (size_t)i * kGroupEntrySize;
        const uint32_t size = rd32(e + 4);
        r->groups[i].file_offset = (uint64_t)rd32(e) << 2;
        r->groups[i].stored_size = size & 0x7FFFFFFFu;
        r->groups[i].compressed = size >> 31;
        r->groups[i].packed_size = rd32(e + 8);
    }
    free(groups);

    out->user_data = r;
    out->read_at = rvz_read_at;
    out->stream_len = rvz_stream_len;
    out->close = rvz_close;
    gc_disc_set_error(NULL);
    return 1;
}
