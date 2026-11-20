/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_disc.h"
#include "rvz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Disc header layout (all integers big-endian). The first 0x440 bytes hold everything read here. */
#define GC_MAGIC_OFFSET 0x1C
#define GC_DOL_OFFSET_FIELD 0x420
#define GC_FST_OFFSET_FIELD 0x424
#define GC_FST_SIZE_FIELD 0x428
#define GC_HEADER_READ_SIZE 0x440
#define GC_FST_ENTRY_SIZE 12
/* Single retail disc's FST is bounded well under this; guards against a
 * corrupt/foreign image driving an unbounded allocation. */
#define GC_FST_MAX_ENTRIES (1u << 20)

/* Last error message, shared by gc_disc and the nod shim (one disc is open at a time, so no locking). */
static char s_last_error[256];

const char* gc_disc_last_error(void) { return s_last_error[0] ? s_last_error : NULL; }

void gc_disc_set_error(const char* message) {
    if (message == NULL) {
        s_last_error[0] = '\0';
        return;
    }
    snprintf(s_last_error, sizeof s_last_error, "%s", message);
}

/* Big-endian reads: the disc was written for the GameCube's PowerPC. */
static uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t be24(const unsigned char* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* --- plain-path stream backend (used by gc_disc_open_path) --- */

static int64_t path_read_at(void* user_data, uint64_t offset, void* out, size_t len) {
    FILE* fp = (FILE*)user_data;
#if defined(_WIN32)
    if (_fseeki64(fp, (int64_t)offset, SEEK_SET) != 0) {
        return -1;
    }
#else
    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
        return -1;
    }
#endif
    size_t n = fread(out, 1, len, fp);
    if (n == 0 && ferror(fp)) {
        return -1;
    }
    return (int64_t)n;
}

static int64_t path_stream_len(void* user_data) {
    FILE* fp = (FILE*)user_data;
#if defined(_WIN32)
    int64_t cur = _ftelli64(fp);
    if (cur < 0 || _fseeki64(fp, 0, SEEK_END) != 0) {
        return -1;
    }
    int64_t end = _ftelli64(fp);
    _fseeki64(fp, cur, SEEK_SET);
#else
    off_t cur = ftello(fp);
    if (cur < 0 || fseeko(fp, 0, SEEK_END) != 0) {
        return -1;
    }
    off_t end = ftello(fp);
    fseeko(fp, cur, SEEK_SET);
#endif
    return (int64_t)end;
}

static void path_stream_close(void* user_data) {
    FILE* fp = (FILE*)user_data;
    if (fp != NULL) {
        fclose(fp);
    }
}

GcDisc* gc_disc_open_path(const char* path) {
    if (path == NULL) {
        gc_disc_set_error("null path");
        return NULL;
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        gc_disc_set_error("could not open disc image");
        return NULL;
    }
    GcStream stream = {
        .user_data = fp,
        .read_at = path_read_at,
        .stream_len = path_stream_len,
        .close = path_stream_close,
    };
    GcDisc* disc = gc_disc_open(&stream, 1);
    if (disc == NULL) {
        fclose(fp);
    }
    return disc;
}

/* --- core --- */

static GcDisc* gc_disc_open_checked(const GcStream* stream, int take_ownership);

/* Reads and checks the header: size, the GameCube magic word 0xC2339F3D, and an FST offset that lies
 * after main.dol and inside the image. */
GcDisc* gc_disc_open(const GcStream* stream, int take_ownership) {
    if (stream == NULL || stream->read_at == NULL || stream->stream_len == NULL) {
        gc_disc_set_error("invalid disc stream");
        return NULL;
    }

    /* RVZ images (Dolphin's compressed format) are decoded on the fly: swap in a stream that returns
     * the uncompressed disc bytes, and let gc_disc_open_checked check it like an ISO. */
    GcStream rvz_stream;
    int is_rvz = 0;
    unsigned char magic[4];
    if (stream->read_at(stream->user_data, 0, magic, sizeof magic) == (int64_t)sizeof magic &&
        rvz_is_magic(magic)) {
        if (!rvz_wrap(stream, take_ownership, &rvz_stream)) {
            return NULL;
        }
        stream = &rvz_stream;
        take_ownership = 1; /* the wrapper always has to be closed; it closes the file if we own it */
        is_rvz = 1;
    }

    GcDisc* disc = gc_disc_open_checked(stream, take_ownership);
    if (disc == NULL && is_rvz) {
        rvz_free_keep_inner(&rvz_stream); /* the caller still closes the file on failure */
    }
    return disc;
}

/* The header checks behind gc_disc_open, on a stream that already yields uncompressed disc bytes. */
static GcDisc* gc_disc_open_checked(const GcStream* stream, int take_ownership) {
    int64_t size = stream->stream_len(stream->user_data);
    if (size <= (int64_t)GC_HEADER_READ_SIZE) {
        gc_disc_set_error("disc image is too small");
        return NULL;
    }

    unsigned char header[GC_HEADER_READ_SIZE];
    int64_t got = stream->read_at(stream->user_data, 0, header, sizeof header);
    if (got != (int64_t)sizeof header) {
        gc_disc_set_error("could not read disc header");
        return NULL;
    }

    if (be32(header + GC_MAGIC_OFFSET) != 0xC2339F3Du) {
        gc_disc_set_error("not a GameCube disc image (bad magic)");
        return NULL;
    }

    GcDisc* disc = calloc(1, sizeof(GcDisc));
    if (disc == NULL) {
        gc_disc_set_error("out of memory");
        return NULL;
    }

    disc->stream = *stream;
    disc->owns_stream = take_ownership;
    disc->disc_size = (uint64_t)size;
    memcpy(disc->game_id, header, 6);
    disc->disc_num = header[6];
    disc->disc_version = header[7];
    disc->audio_streaming = header[8];
    disc->audio_stream_buf_size = header[9];
    memcpy(disc->gcn_magic, header + GC_MAGIC_OFFSET, 4);
    disc->dol_offset = be32(header + GC_DOL_OFFSET_FIELD);
    disc->fst_offset = be32(header + GC_FST_OFFSET_FIELD);
    disc->fst_size = be32(header + GC_FST_SIZE_FIELD);

    if (disc->fst_offset == 0 || disc->fst_offset <= disc->dol_offset ||
        (uint64_t)disc->fst_offset >= disc->disc_size) {
        gc_disc_set_error("implausible FST offset in disc header");
        free(disc);
        return NULL;
    }

    gc_disc_set_error(NULL);
    return disc;
}

int64_t gc_disc_read_at(GcDisc* disc, uint64_t offset, void* out, size_t len) {
    if (disc == NULL) {
        return -1;
    }
    return disc->stream.read_at(disc->stream.user_data, offset, out, len);
}

/* Reads the whole file system table and its names into memory once (a few hundred KiB for this game),
 * so file lookups never touch the card again. */
int gc_disc_ensure_fst(GcDisc* disc) {
    if (disc == NULL) {
        return 0;
    }
    if (disc->fst_parsed) {
        return disc->fst != NULL;
    }
    disc->fst_parsed = 1;

    unsigned char entry0[GC_FST_ENTRY_SIZE];
    if (gc_disc_read_at(disc, disc->fst_offset, entry0, sizeof entry0) != (int64_t)sizeof entry0) {
        gc_disc_set_error("could not read FST root entry");
        return 0;
    }
    if (entry0[0] != 1) {
        gc_disc_set_error("FST root entry is not a directory");
        return 0;
    }
    uint32_t entry_count = be32(entry0 + 8);
    if (entry_count == 0 || entry_count > GC_FST_MAX_ENTRIES) {
        gc_disc_set_error("implausible FST entry count");
        return 0;
    }

    size_t fst_bytes_len = (size_t)entry_count * GC_FST_ENTRY_SIZE;
    unsigned char* raw = malloc(fst_bytes_len);
    if (raw == NULL) {
        gc_disc_set_error("out of memory reading FST");
        return 0;
    }
    if (gc_disc_read_at(disc, disc->fst_offset, raw, fst_bytes_len) != (int64_t)fst_bytes_len) {
        gc_disc_set_error("could not read FST table");
        free(raw);
        return 0;
    }

    /* String table sits right after the entry array; read it whole (bounded
     * by fst_size from the header, which covers entries + names). */
    uint64_t string_table_offset = (uint64_t)disc->fst_offset + fst_bytes_len;
    size_t string_table_len = 0;
    if (disc->fst_size > fst_bytes_len) {
        string_table_len = disc->fst_size - fst_bytes_len;
    }
    /* Guard against a corrupt/foreign fst_size: cap to something sane. */
    if (string_table_len == 0 || string_table_len > (16u << 20)) {
        string_table_len = 16u << 20;
    }
    char* strings = malloc(string_table_len);
    if (strings == NULL) {
        gc_disc_set_error("out of memory reading FST string table");
        free(raw);
        return 0;
    }
    int64_t got = gc_disc_read_at(disc, string_table_offset, strings, string_table_len);
    if (got <= 0) {
        gc_disc_set_error("could not read FST string table");
        free(raw);
        free(strings);
        return 0;
    }
    string_table_len = (size_t)got;

    GcFstEntry* fst = calloc(entry_count, sizeof(GcFstEntry));
    if (fst == NULL) {
        gc_disc_set_error("out of memory allocating FST entries");
        free(raw);
        free(strings);
        return 0;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        const unsigned char* e = raw + (size_t)i * GC_FST_ENTRY_SIZE;
        fst[i].is_dir = e[0] != 0;
        uint32_t name_offset = be24(e + 1);
        uint32_t field2 = be32(e + 4);
        uint32_t field3 = be32(e + 8);

        if (i == 0) {
            fst[i].parent_index = 0;
            fst[i].next_index = field3;
            fst[i].name = calloc(1, 1); /* root: empty name */
        } else if (fst[i].is_dir) {
            fst[i].parent_index = field2;
            fst[i].next_index = field3;
        } else {
            fst[i].file_offset = field2;
            fst[i].file_length = field3;
        }

        if (i != 0) {
            if (name_offset >= string_table_len) {
                fst[i].name = calloc(1, 1);
            } else {
                size_t max_len = string_table_len - name_offset;
                size_t n = strnlen(strings + name_offset, max_len);
                fst[i].name = malloc(n + 1);
                if (fst[i].name != NULL) {
                    memcpy(fst[i].name, strings + name_offset, n);
                    fst[i].name[n] = '\0';
                }
            }
        }
    }

    free(raw);
    free(strings);

    disc->fst = fst;
    disc->fst_count = entry_count;
    gc_disc_set_error(NULL);
    return 1;
}

/* Calls `callback` for every entry after the root; the callback's return value is the next index, so
 * it can skip a directory by returning that directory's next_index. */
void gc_disc_walk_fst(GcDisc* disc, GcFstWalkFn callback, void* user_data) {
    if (disc == NULL || callback == NULL || !gc_disc_ensure_fst(disc)) {
        return;
    }
    uint32_t i = 1;
    while (i < disc->fst_count) {
        const GcFstEntry* e = &disc->fst[i];
        uint32_t size_or_next = e->is_dir ? e->next_index : e->file_length;
        uint32_t next = callback(i, e->is_dir, e->name ? e->name : "", size_or_next, user_data);
        if (next == GC_FST_WALK_STOP) {
            return;
        }
        i = (next > i) ? next : (i + 1);
    }
}

/* main.dol is assumed to end where the FST begins (true for retail discs); it is read once and kept. */
const void* gc_disc_dol(GcDisc* disc, size_t* out_size) {
    if (disc == NULL) {
        return NULL;
    }
    if (disc->dol_cache == NULL) {
        uint64_t dol_size = (uint64_t)disc->fst_offset - disc->dol_offset;
        if (dol_size == 0 || dol_size > (64u << 20)) {
            gc_disc_set_error("implausible DOL size");
            return NULL;
        }
        void* buf = malloc((size_t)dol_size);
        if (buf == NULL) {
            gc_disc_set_error("out of memory reading DOL");
            return NULL;
        }
        if (gc_disc_read_at(disc, disc->dol_offset, buf, (size_t)dol_size) != (int64_t)dol_size) {
            gc_disc_set_error("could not read main.dol");
            free(buf);
            return NULL;
        }
        disc->dol_cache = buf;
        disc->dol_cache_size = dol_size;
    }
    if (out_size != NULL) {
        *out_size = (size_t)disc->dol_cache_size;
    }
    return disc->dol_cache;
}

/* Frees the FST, the cached DOL and, when owned, closes the stream. */
void gc_disc_close(GcDisc* disc) {
    if (disc == NULL) {
        return;
    }
    if (disc->fst != NULL) {
        for (uint32_t i = 0; i < disc->fst_count; i++) {
            free(disc->fst[i].name);
        }
        free(disc->fst);
    }
    free(disc->dol_cache);
    if (disc->owns_stream && disc->stream.close != NULL) {
        disc->stream.close(disc->stream.user_data);
    }
    free(disc);
}
