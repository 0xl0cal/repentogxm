/*
 * ARCH000 reader for the manager's game skin; see manager_arch.h for the
 * format summary and its sources.  Every constant below is copied from
 * tools/isaac_vita_repack.py (line references are to that file at fb4aa9d)
 * or recomp/test_room_entry_corpus.py name_hash().
 */
#include "manager_arch.h"

#include "manager_tinfl.h"

#include <string.h>

/* MAGIC = b"ARCH000" */
static const uint8_t manager_arch_magic[7] = {'A', 'R', 'C', 'H', '0', '0', '0'};

const char *const manager_arch_prefixes[2] = {"resources-dlc3/", "resources/"};
const char *const manager_arch_default_order[3] = {
    "repentance.a", "afterbirthp.a", "afterbirth.a"};

static uint32_t arch_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t arch_le16(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

void manager_arch_init(struct manager_arch *arch)
{
    if (arch == NULL)
        return;
    memset(arch, 0, sizeof(*arch));
    arch->handle = -1;
}

/* Loops over short reads; 0 bytes (EOF), an error or an over-long answer
 * from the blob layer all fail the read. */
static int arch_read_exact(const struct manager_blob_io *io, int handle,
                           void *buffer, uint32_t size, uint64_t offset,
                           uint32_t *preads)
{
    uint8_t *cursor = (uint8_t *)buffer;

    while (size > 0u) {
        int got = io->pread(io->context, handle, cursor, size, offset);
        if (preads != NULL)
            ++*preads;
        if (got <= 0 || (uint32_t)got > size)
            return MANAGER_ARCH_E_IO;
        cursor += got;
        size -= (uint32_t)got;
        offset += (uint32_t)got;
    }
    return MANAGER_ARCH_OK;
}

int manager_arch_open(struct manager_arch *arch,
                      const struct manager_blob_io *io, const char *path)
{
    uint8_t header[MANAGER_ARCH_HEADER_BYTES];
    int handle = -1;
    uint64_t size = 0u;
    uint64_t index_end;
    uint32_t index_offset;
    uint32_t count;
    int status;

    if (arch == NULL || io == NULL || io->open == NULL || io->pread == NULL ||
        io->close == NULL || path == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    manager_arch_init(arch);
    if (io->open(io->context, path, &handle, &size) < 0)
        return MANAGER_ARCH_E_IO;

    /* Archive._parse: "shorter than the 14-byte header", "not an ARCH000
     * archive", "index does not close at EOF" (index_offset < 14 or
     * index_offset + 20 * count != size). */
    if (size < MANAGER_ARCH_HEADER_BYTES) {
        status = MANAGER_ARCH_E_HEADER;
        goto fail;
    }
    status = arch_read_exact(io, handle, header, sizeof(header), 0u, NULL);
    if (status != MANAGER_ARCH_OK)
        goto fail;
    if (memcmp(header, manager_arch_magic, sizeof(manager_arch_magic)) != 0) {
        status = MANAGER_ARCH_E_HEADER;
        goto fail;
    }
    index_offset = arch_le32(header + 8);
    count = arch_le16(header + 12);
    index_end = (uint64_t)index_offset +
                (uint64_t)MANAGER_ARCH_INDEX_ENTRY_BYTES * count;
    if (index_offset < MANAGER_ARCH_HEADER_BYTES || index_end != size) {
        status = MANAGER_ARCH_E_INDEX;
        goto fail;
    }

    arch->io = io;
    arch->handle = handle;
    arch->is_open = 1;
    arch->file_size = size;
    arch->mode = header[7];
    arch->index_offset = index_offset;
    arch->count = count;
    arch->index = NULL;
    return MANAGER_ARCH_OK;

fail:
    (void)io->close(io->context, handle);
    return status;
}

uint32_t manager_arch_index_bytes(const struct manager_arch *arch)
{
    if (arch == NULL || !arch->is_open)
        return 0u;
    return arch->count * MANAGER_ARCH_INDEX_ENTRY_BYTES;
}

int manager_arch_load_index(struct manager_arch *arch, void *buffer,
                            size_t capacity)
{
    const uint8_t *rows = (const uint8_t *)buffer;
    uint32_t bytes;
    uint32_t row;
    int status;

    if (arch == NULL || buffer == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    if (!arch->is_open)
        return MANAGER_ARCH_E_STATE;
    arch->index = NULL;
    bytes = manager_arch_index_bytes(arch);
    if (capacity < bytes)
        return MANAGER_ARCH_E_CAPACITY;
    if (bytes != 0u) {
        status = arch_read_exact(arch->io, arch->handle, buffer, bytes,
                                 arch->index_offset, NULL);
        if (status != MANAGER_ARCH_OK)
            return status;
    }
    /* Archive._parse: "entry %d offset 0x%x outside the data area" when
     * offset < 14 or offset > index_offset. */
    for (row = 0u; row < arch->count; ++row) {
        uint32_t offset =
            arch_le32(rows + (size_t)row * MANAGER_ARCH_INDEX_ENTRY_BYTES + 8u);
        if (offset < MANAGER_ARCH_HEADER_BYTES || offset > arch->index_offset)
            return MANAGER_ARCH_E_INDEX;
    }
    arch->index = rows;
    return MANAGER_ARCH_OK;
}

void manager_arch_close(struct manager_arch *arch)
{
    if (arch == NULL)
        return;
    if (arch->is_open && arch->io != NULL && arch->io->close != NULL)
        (void)arch->io->close(arch->io->context, arch->handle);
    manager_arch_init(arch);
}

/* name_hash(): hash_a = 5381, hash_a = hash_a * 33 + byte; hash_b =
 * 0x5BB2220E, hash_b = (hash_b ^ byte) * 0x1000193; over the ASCII bytes of
 * path.lower(). */
static int arch_hash_fold(const char *text, uint32_t *hash_a, uint32_t *hash_b)
{
    for (; *text != '\0'; ++text) {
        uint32_t byte = (unsigned char)*text;
        if (byte >= 0x80u)
            return MANAGER_ARCH_E_PATH;
        if (byte >= 'A' && byte <= 'Z')
            byte += (uint32_t)('a' - 'A');
        *hash_a = *hash_a * 33u + byte;
        *hash_b = (*hash_b ^ byte) * 0x1000193u;
    }
    return MANAGER_ARCH_OK;
}

int manager_arch_name_hash(const char *prefix, const char *path,
                           uint32_t *hash_a, uint32_t *hash_b)
{
    uint32_t a = 5381u;
    uint32_t b = 0x5BB2220Eu;
    int status;

    if (path == NULL || hash_a == NULL || hash_b == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    if (prefix != NULL) {
        status = arch_hash_fold(prefix, &a, &b);
        if (status != MANAGER_ARCH_OK)
            return status;
    }
    status = arch_hash_fold(path, &a, &b);
    if (status != MANAGER_ARCH_OK)
        return status;
    *hash_a = a;
    *hash_b = b;
    return MANAGER_ARCH_OK;
}

int manager_arch_entry_at(const struct manager_arch *arch, uint32_t row,
                          struct manager_arch_entry *entry)
{
    const uint8_t *bytes;
    uint32_t limit;
    uint32_t other;

    if (arch == NULL || entry == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    if (!arch->is_open || arch->index == NULL)
        return MANAGER_ARCH_E_STATE;
    if (row >= arch->count)
        return MANAGER_ARCH_E_ARGUMENT;
    bytes = arch->index + (size_t)row * MANAGER_ARCH_INDEX_ENTRY_BYTES;
    entry->row = row;
    entry->hash_a = arch_le32(bytes);
    entry->hash_b = arch_le32(bytes + 4);
    entry->offset = arch_le32(bytes + 8);
    entry->size = arch_le32(bytes + 12);
    entry->checksum = arch_le32(bytes + 16);
    /* payload_limits(): the next higher distinct offset, else the index. */
    limit = arch->index_offset;
    for (other = 0u; other < arch->count; ++other) {
        uint32_t offset = arch_le32(arch->index +
                                    (size_t)other * MANAGER_ARCH_INDEX_ENTRY_BYTES +
                                    8u);
        if (offset > entry->offset && offset < limit)
            limit = offset;
    }
    entry->limit = limit;
    return MANAGER_ARCH_OK;
}

int manager_arch_find(const struct manager_arch *arch, const char *prefix,
                      const char *path, struct manager_arch_entry *entry)
{
    uint32_t hash_a;
    uint32_t hash_b;
    uint32_t row;
    int status;

    if (arch == NULL || path == NULL || entry == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    if (!arch->is_open || arch->index == NULL)
        return MANAGER_ARCH_E_STATE;
    status = manager_arch_name_hash(prefix, path, &hash_a, &hash_b);
    if (status != MANAGER_ARCH_OK)
        return status;
    for (row = 0u; row < arch->count; ++row) {
        const uint8_t *bytes =
            arch->index + (size_t)row * MANAGER_ARCH_INDEX_ENTRY_BYTES;
        if (arch_le32(bytes) == hash_a && arch_le32(bytes + 4) == hash_b) {
            status = manager_arch_entry_at(arch, row, entry);
            return status == MANAGER_ARCH_OK ? 1 : status;
        }
    }
    return 0;
}

/* ---- ISAAC keystream (IsaacKeystream, isaac_vita_repack.py:346-439) --- */

static void keystream_mix(uint32_t v[8])
{
    uint32_t a = v[0], b = v[1], c = v[2], d = v[3];
    uint32_t e = v[4], f = v[5], g = v[6], h = v[7];

    a ^= b << 11; d += a; b += c;
    b ^= c >> 2;  e += b; c += d;
    c ^= d << 8;  f += c; d += e;
    d ^= e >> 16; g += d; e += f;
    e ^= f << 10; h += e; f += g;
    f ^= g >> 4;  a += f; g += h;
    g ^= h << 8;  b += g; h += a;
    h ^= a >> 9;  c += h; a += b;
    v[0] = a; v[1] = b; v[2] = c; v[3] = d;
    v[4] = e; v[5] = f; v[6] = g; v[7] = h;
}

static void keystream_generate(struct manager_arch_keystream *keystream)
{
    uint32_t *mm = keystream->mm;
    uint32_t *rsl = keystream->rsl;
    uint32_t aa = keystream->aa;
    uint32_t bb = keystream->bb;
    uint32_t i;

    keystream->cc += 1u;
    bb += keystream->cc;
    for (i = 0u; i < 256u; ++i) {
        uint32_t x = mm[i];
        uint32_t y;
        switch (i & 3u) {
        case 0u: aa ^= aa << 13; break;
        case 1u: aa ^= aa >> 6; break;
        case 2u: aa ^= aa << 2; break;
        default: aa ^= aa >> 16; break;
        }
        aa += mm[(i + 128u) & 0xFFu];
        y = mm[(x >> 2) & 0xFFu] + aa + bb;
        mm[i] = y;
        bb = mm[(y >> 10) & 0xFFu] + x;
        rsl[i] = bb;
    }
    keystream->aa = aa;
    keystream->bb = bb;
}

static void keystream_randinit(struct manager_arch_keystream *keystream)
{
    uint32_t v[8];
    uint32_t pass;
    uint32_t i;
    uint32_t j;

    for (j = 0u; j < 8u; ++j)
        v[j] = 0x9E3779B9u;
    for (i = 0u; i < 4u; ++i)
        keystream_mix(v);
    for (pass = 0u; pass < 2u; ++pass) {
        const uint32_t *source = pass == 0u ? keystream->rsl : keystream->mm;
        for (i = 0u; i < 256u; i += 8u) {
            for (j = 0u; j < 8u; ++j)
                v[j] += source[i + j];
            keystream_mix(v);
            for (j = 0u; j < 8u; ++j)
                keystream->mm[i + j] = v[j];
        }
    }
    keystream_generate(keystream);
}

void manager_arch_keystream_init(struct manager_arch_keystream *keystream,
                                 uint32_t seed)
{
    /* low = ((seed << 15 ^ seed) << 8) ^ (seed >> 9) ^ seed (mod 2^32),
     * state = seed << 32 | low; PCG32 mult 0x5851F42D4C957F2D, inc 0x7F. */
    uint64_t wide = (uint64_t)seed;
    uint32_t low = (uint32_t)((((wide << 15) ^ wide) << 8) ^ (wide >> 9) ^ wide);
    uint64_t state = (wide << 32) | low;
    uint32_t i;

    for (i = 0u; i < 256u; ++i) {
        uint32_t xs = (uint32_t)((state ^ (state >> 18)) >> 27);
        uint32_t rot = (uint32_t)(state >> 59);
        keystream->rsl[i] = (xs >> rot) | (xs << ((32u - rot) & 31u));
        state = state * 0x5851F42D4C957F2DULL + 0x7FULL;
    }
    memset(keystream->mm, 0, sizeof(keystream->mm));
    keystream->aa = 0u;
    keystream->bb = 0u;
    keystream->cc = 0u;
    keystream_randinit(keystream);
    keystream->index = 0u;
}

/* One sub_005b06f0 call: ceil(n / 4) words, low byte first, regenerating as
 * soon as word 256 has been taken (IsaacKeystream.apply).  Only the first
 * `keep` bytes are stored; the rest of the words are still consumed. */
static void keystream_xor_keep(struct manager_arch_keystream *keystream,
                               uint8_t *dst, const uint8_t *src,
                               uint32_t keep, uint32_t n)
{
    uint32_t i = 0u;

    while (i < n) {
        uint32_t word = keystream->rsl[keystream->index];
        uint32_t lane;
        keystream->index += 1u;
        if (keystream->index >= 256u) {
            keystream_generate(keystream);
            keystream->index = 0u;
        }
        for (lane = 0u; lane < 4u && i < n; ++lane, ++i) {
            if (i < keep)
                dst[i] = (uint8_t)(src[i] ^ (uint8_t)(word >> (8u * lane)));
        }
    }
}

void manager_arch_keystream_xor(struct manager_arch_keystream *keystream,
                                uint8_t *dst, const uint8_t *src, uint32_t n)
{
    keystream_xor_keep(keystream, dst, src, n, n);
}

/* mount_checksum(), isaac_vita_repack.py:311-339: one zeroed 0x200 buffer,
 * ceil(got / 4) dwords per chunk, so a final partial dword is padded with
 * whatever the previous chunk left in the buffer. */
uint32_t manager_arch_mount_checksum(const uint8_t *data, uint32_t size)
{
    uint8_t buffer[MANAGER_ARCH_CHECKSUM_CHUNK];
    uint32_t h = MANAGER_ARCH_CHECKSUM_SEED;
    uint32_t pos = 0u;
    uint32_t remaining = size;

    memset(buffer, 0, sizeof(buffer));
    for (;;) {
        uint32_t want = remaining < MANAGER_ARCH_CHECKSUM_CHUNK
                            ? remaining : MANAGER_ARCH_CHECKSUM_CHUNK;
        uint32_t got = want;
        uint32_t dwords;
        uint32_t i;
        if (got != 0u)
            memcpy(buffer, data + pos, got);
        dwords = (got + 3u) >> 2;
        for (i = 0u; i < dwords; ++i)
            h = ((h >> 1) | (h << 31)) + arch_le32(buffer + 4u * i);
        remaining -= want;
        pos += want;
        if (!(remaining > 0u && got == want))
            break;
    }
    return h;
}

/* ---- entry decode ------------------------------------------------------ */

/* [pos, pos + need) through the read-ahead window; the caller guarantees
 * pos + need <= limit and need <= MANAGER_ARCH_WINDOW_BYTES. */
static int arch_window(struct manager_arch *arch, struct manager_arch_work *work,
                       uint64_t pos, uint32_t need, uint64_t limit,
                       const uint8_t **data)
{
    uint64_t available;
    uint32_t want;
    int status;

    if (work->window_bytes != 0u && pos >= work->window_offset &&
        pos + need <= work->window_offset + work->window_bytes) {
        *data = work->window + (size_t)(pos - work->window_offset);
        return MANAGER_ARCH_OK;
    }
    available = limit - pos;
    want = available < MANAGER_ARCH_WINDOW_BYTES ? (uint32_t)available
                                                 : MANAGER_ARCH_WINDOW_BYTES;
    work->window_bytes = 0u;
    status = arch_read_exact(arch->io, arch->handle, work->window, want, pos,
                             &work->preads);
    if (status != MANAGER_ARCH_OK)
        return status;
    work->window_offset = pos;
    work->window_bytes = want;
    *data = work->window;
    return MANAGER_ARCH_OK;
}

int manager_arch_read(struct manager_arch *arch,
                      const struct manager_arch_entry *entry, void *output,
                      size_t capacity, struct manager_arch_work *work)
{
    uint8_t *out = (uint8_t *)output;
    uint32_t size;
    uint32_t produced = 0u;
    uint64_t pos;
    uint64_t limit;
    int latched = 0;
    int status;

    if (arch == NULL || entry == NULL || work == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    if (!arch->is_open || arch->index == NULL)
        return MANAGER_ARCH_E_STATE;
    if (arch->mode != MANAGER_ARCH_MODE_MINIZ)
        return MANAGER_ARCH_E_MODE;
    size = entry->size;
    if (out == NULL && size != 0u)
        return MANAGER_ARCH_E_ARGUMENT;
    if (capacity < size)
        return MANAGER_ARCH_E_CAPACITY;
    /* The entry must describe this archive (manager_arch_entry_at output). */
    if (entry->offset < MANAGER_ARCH_HEADER_BYTES ||
        entry->offset > arch->index_offset || entry->limit < entry->offset ||
        entry->limit > arch->index_offset)
        return MANAGER_ARCH_E_ARGUMENT;

    work->preads = 0u;
    work->deflate_pieces = 0u;
    work->stored_pieces = 0u;
    work->window_bytes = 0u;
    /* reset (sub_0059c170): tinfl state <- 0, latch <- 0, ISAAC re-seeded
     * from hash_b (seeded lazily here, at the latch). */
    memset(work->tinfl_state, 0, sizeof(work->tinfl_state));
    pos = entry->offset;
    limit = entry->limit;

    /* iter_pieces(): an entry of size 0 has no pieces; otherwise pieces
     * follow each other until one carries the final bit. */
    while (size != 0u) {
        const uint8_t *piece;
        uint32_t header;
        uint32_t length;
        int final;

        if (limit - pos < 4u)
            return MANAGER_ARCH_E_PIECE;
        status = arch_window(arch, work, pos, 4u, limit, &piece);
        if (status != MANAGER_ARCH_OK)
            return status;
        header = arch_le32(piece);
        length = header & 0x7FFFFFFFu;
        final = (header & 0x80000000u) != 0u;
        if (length > MANAGER_ARCH_PIECE_MAX || limit - pos - 4u < length)
            return MANAGER_ARCH_E_PIECE;
        status = arch_window(arch, work, pos, 4u + length, limit, &piece);
        if (status != MANAGER_ARCH_OK)
            return status;
        piece += 4;

        /* Sticky stored latch: the first non-final piece of exactly 0x400
         * bytes (latch_index()). */
        if (!latched && !final && length == MANAGER_ARCH_BLOCK) {
            latched = 1;
            manager_arch_keystream_init(&work->keystream, entry->hash_b);
        }
        if (latched) {
            uint32_t keep = size - produced;
            /* The engine copies a stored piece into its 0x400 ring. */
            if (length > MANAGER_ARCH_BLOCK)
                return MANAGER_ARCH_E_PIECE;
            if (keep > length)
                keep = length;
            keystream_xor_keep(&work->keystream, out + produced, piece, keep,
                               length);
            produced += keep;
            ++work->stored_pieces;
        } else {
            /* NativeTinfl.decode_prefix (tools/test_isaac_vita_repack.py):
             * whole piece consumed, 0x400 bytes (final: the rest of the
             * entry), status DONE for the final piece, NEEDS_MORE_INPUT
             * otherwise.  The engine's wrapping 0x400 ring is modelled as a
             * linear window over the output: history is the previous 0x400
             * decoded bytes, which is what the ring holds after a full
             * piece; the mount checksum below backs the equivalence. */
            uint32_t want = final ? size - produced : MANAGER_ARCH_BLOCK;
            uint32_t in_size = length;
            uint32_t out_size = want;
            uint32_t reserved = 0u;
            uint32_t history = produced > MANAGER_ARCH_BLOCK
                                   ? produced - MANAGER_ARCH_BLOCK : 0u;
            int expected = final ? ISAAC_VITA_ARCHIVE_MINIZ_DONE
                                 : ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT;
            int inflate;
            if (want > MANAGER_ARCH_BLOCK || want > size - produced)
                return MANAGER_ARCH_E_SIZE;
            inflate = manager_tinfl_decompress(
                work->tinfl_state, piece, &in_size, out + history,
                out + produced, &out_size,
                (final ? 0u : (uint32_t)MANAGER_TINFL_HAS_MORE_INPUT) |
                    (uint32_t)MANAGER_TINFL_NON_WRAPPING_OUTPUT,
                &reserved);
            if (reserved != 0u || inflate != expected || in_size != length ||
                out_size != want)
                return MANAGER_ARCH_E_INFLATE;
            produced += out_size;
            ++work->deflate_pieces;
        }
        pos += 4u + length;
        if (final)
            break;
    }
    if (produced != size)
        return MANAGER_ARCH_E_SIZE;
    if (manager_arch_mount_checksum(out, size) != entry->checksum)
        return MANAGER_ARCH_E_CHECKSUM;
    return MANAGER_ARCH_OK;
}

int manager_arch_set_find(const struct manager_arch_set *set,
                          const char *path, uint32_t *archive_index,
                          struct manager_arch_entry *entry)
{
    uint32_t a;
    uint32_t p;

    if (set == NULL || path == NULL || archive_index == NULL || entry == NULL)
        return MANAGER_ARCH_E_ARGUMENT;
    for (a = 0u; a < set->count && a < MANAGER_ARCH_SET_MAX; ++a) {
        const struct manager_arch *arch = set->archives[a];
        if (arch == NULL || !arch->is_open || arch->index == NULL ||
            arch->mode != MANAGER_ARCH_MODE_MINIZ)
            continue;
        for (p = 0u; p < 2u; ++p) {
            int found = manager_arch_find(arch, manager_arch_prefixes[p], path,
                                          entry);
            if (found < 0)
                return found;
            if (found == 1) {
                *archive_index = a;
                return 1;
            }
        }
    }
    return 0;
}

const char *manager_arch_status_text(int status)
{
    switch (status) {
    case MANAGER_ARCH_OK: return "ok";
    case MANAGER_ARCH_E_ARGUMENT: return "argument";
    case MANAGER_ARCH_E_IO: return "io";
    case MANAGER_ARCH_E_HEADER: return "header";
    case MANAGER_ARCH_E_INDEX: return "index";
    case MANAGER_ARCH_E_CAPACITY: return "capacity";
    case MANAGER_ARCH_E_MODE: return "mode";
    case MANAGER_ARCH_E_PIECE: return "piece";
    case MANAGER_ARCH_E_INFLATE: return "inflate";
    case MANAGER_ARCH_E_SIZE: return "size";
    case MANAGER_ARCH_E_CHECKSUM: return "checksum";
    case MANAGER_ARCH_E_PATH: return "path";
    case MANAGER_ARCH_E_STATE: return "state";
    default: return "unknown";
    }
}
