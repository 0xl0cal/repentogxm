/* Host behavioural oracle for the frozen, bounded PE loader.
 *
 * It exercises the pure parser directly so malformed metadata is not hidden
 * behind the production SHA gate, then exercises the exact transactional
 * file/allocator path with injected failures and leak counters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_pe.h"

typedef struct expected_section {
    const char *name;
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t copy_size;
} expected_section;

static const expected_section expected_sections[5] = {
    { ".text",  0x00001000U, 0x00604b34U, 0x00000400U,
      0x00604c00U, 0x00604b34U },
    { ".rdata", 0x00606000U, 0x001a2f5eU, 0x00605000U,
      0x001a3000U, 0x001a2f5eU },
    { ".data",  0x007a9000U, 0x000610e4U, 0x007a8000U,
      0x00044e00U, 0x00044e00U },
    { ".rsrc",  0x0080b000U, 0x00003b20U, 0x007ece00U,
      0x00003c00U, 0x00003b20U },
    { ".reloc", 0x0080f000U, 0x0004f3d8U, 0x007f0a00U,
      0x0004f400U, 0x0004f3d8U }
};

static int failures;

#define ORACLE_CHECK(condition, ...)                                      \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "guest PE oracle: ");                        \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static uint16_t oracle_u16(const unsigned char *data, size_t offset)
{
    return (uint16_t)((uint16_t)data[offset] |
                      ((uint16_t)data[offset + 1U] << 8));
}

static uint32_t oracle_u32(const unsigned char *data, size_t offset)
{
    return (uint32_t)data[offset] |
           ((uint32_t)data[offset + 1U] << 8) |
           ((uint32_t)data[offset + 2U] << 16) |
           ((uint32_t)data[offset + 3U] << 24);
}

static void oracle_put_u16(unsigned char *data, size_t offset, uint16_t value)
{
    data[offset] = (unsigned char)value;
    data[offset + 1U] = (unsigned char)(value >> 8);
}

static void oracle_put_u32(unsigned char *data, size_t offset, uint32_t value)
{
    data[offset] = (unsigned char)value;
    data[offset + 1U] = (unsigned char)(value >> 8);
    data[offset + 2U] = (unsigned char)(value >> 16);
    data[offset + 3U] = (unsigned char)(value >> 24);
}

static int oracle_hex_digest(
    const char *hex, unsigned char digest[32])
{
    unsigned i;
    if (!hex || strlen(hex) != 64U)
        return 0;
    for (i = 0U; i < 32U; ++i) {
        unsigned high, low;
        char a = hex[i * 2U], b = hex[i * 2U + 1U];
        high = a >= '0' && a <= '9' ? (unsigned)(a - '0')
             : a >= 'a' && a <= 'f' ? (unsigned)(a - 'a' + 10) : 99U;
        low = b >= '0' && b <= '9' ? (unsigned)(b - '0')
            : b >= 'a' && b <= 'f' ? (unsigned)(b - 'a' + 10) : 99U;
        if (high > 15U || low > 15U)
            return 0;
        digest[i] = (unsigned char)((high << 4) | low);
    }
    return 1;
}

static void oracle_sha_tests(void)
{
    static const struct {
        const char *message;
        const char *digest;
    } vectors[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" }
    };
    unsigned char actual[32], expected[32];
    size_t i;
    for (i = 0U; i < sizeof vectors / sizeof vectors[0]; ++i) {
        ORACLE_CHECK(oracle_hex_digest(vectors[i].digest, expected),
                     "invalid embedded SHA vector %u", (unsigned)i);
        guest_pe_sha256(vectors[i].message, strlen(vectors[i].message), actual);
        ORACLE_CHECK(memcmp(actual, expected, sizeof actual) == 0,
                     "SHA-256 vector %u failed", (unsigned)i);
    }
}

static unsigned char *oracle_read_file(const char *path, size_t *size_out)
{
    FILE *file;
    long measured;
    unsigned char *bytes;
    if (!path || !size_out)
        return NULL;
    file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0L, SEEK_END) != 0 ||
        (measured = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (unsigned char *)malloc((size_t)measured);
    if (!bytes || fread(bytes, 1U, (size_t)measured, file) !=
                  (size_t)measured || fclose(file) != 0) {
        free(bytes);
        return NULL;
    }
    *size_out = (size_t)measured;
    return bytes;
}

static void oracle_expect_parse(
    const char *name, const unsigned char *raw, size_t raw_size,
    guest_pe_error expected)
{
    guest_pe_plan plan;
    guest_pe_error actual = guest_pe_parse_bounded(
        raw, raw_size, GUEST_PE_VITA_TARGET_BASE, &plan);
    ORACLE_CHECK(actual == expected, "%s: expected %s, got %s",
                 name, guest_pe_error_string(expected),
                 guest_pe_error_string(actual));
}

static size_t oracle_find_reloc_entry(
    const unsigned char *raw, const guest_pe_plan *plan, uint32_t type)
{
    uint32_t directory_raw, offset = 0U;
    if (!guest_pe_rva_to_raw(plan, plan->directories[5].rva,
                             plan->directories[5].size, &directory_raw))
        return SIZE_MAX;
    while (offset < plan->directories[5].size) {
        uint32_t block_size = oracle_u32(raw, directory_raw + offset + 4U);
        uint32_t entry_offset;
        for (entry_offset = 8U; entry_offset < block_size;
             entry_offset += 2U) {
            uint16_t entry = oracle_u16(
                raw, directory_raw + offset + entry_offset);
            if (((uint32_t)entry >> 12) == type)
                return (size_t)directory_raw + offset + entry_offset;
        }
        offset += block_size;
    }
    return SIZE_MAX;
}

static size_t oracle_last_reloc_block(
    const unsigned char *raw, const guest_pe_plan *plan)
{
    uint32_t directory_raw, offset = 0U, previous = 0U;
    if (!guest_pe_rva_to_raw(plan, plan->directories[5].rva,
                             plan->directories[5].size, &directory_raw))
        return SIZE_MAX;
    while (offset < plan->directories[5].size) {
        uint32_t block_size = oracle_u32(raw, directory_raw + offset + 4U);
        previous = offset;
        offset += block_size;
    }
    return (size_t)directory_raw + previous;
}

static void oracle_exact_plan_tests(
    const unsigned char *raw, size_t raw_size, guest_pe_plan *vita_plan)
{
    guest_pe_plan pc_plan;
    guest_pe_error error;
    unsigned char digest[32];
    unsigned char *image;
    unsigned i;

    error = guest_pe_authenticate(raw, raw_size);
    ORACLE_CHECK(error == GUEST_PE_OK, "exact PE authentication: %s",
                 guest_pe_error_string(error));
    error = guest_pe_parse_bounded(raw, raw_size,
                                   GUEST_PE_VITA_TARGET_BASE, vita_plan);
    ORACLE_CHECK(error == GUEST_PE_OK, "exact Vita plan: %s",
                 guest_pe_error_string(error));
    error = guest_pe_parse_bounded(raw, raw_size,
                                   GUEST_PE_PC_TARGET_BASE, &pc_plan);
    ORACLE_CHECK(error == GUEST_PE_OK, "exact PC plan: %s",
                 guest_pe_error_string(error));
    if (error != GUEST_PE_OK)
        return;
    {
        guest_pe_plan rejected;
        error = guest_pe_parse_bounded(raw, raw_size, 0x40000000U, &rejected);
        ORACLE_CHECK(error == GUEST_PE_ERR_CONTRACT,
                     "unsupported target base was accepted: %s",
                     guest_pe_error_string(error));
    }
    ORACLE_CHECK(vita_plan->original_base == GUEST_PE_EXPECTED_ORIGINAL_BASE &&
                 vita_plan->target_base == GUEST_PE_VITA_TARGET_BASE &&
                 vita_plan->image_size == GUEST_PE_EXPECTED_IMAGE_SIZE &&
                 vita_plan->entry_rva == GUEST_PE_EXPECTED_ENTRY_RVA &&
                 vita_plan->header_size == GUEST_PE_EXPECTED_HEADER_SIZE,
                 "frozen image contract differs");
    ORACLE_CHECK(vita_plan->relocation_blocks ==
                     GUEST_PE_EXPECTED_RELOC_BLOCKS &&
                 vita_plan->highlow_count ==
                     GUEST_PE_EXPECTED_HIGHLOW_COUNT &&
                 vita_plan->absolute_count ==
                     GUEST_PE_EXPECTED_ABSOLUTE_COUNT,
                 "relocation census differs");
    for (i = 0U; i < GUEST_PE_EXPECTED_SECTION_COUNT; ++i) {
        const guest_pe_section_plan *actual = &vita_plan->sections[i];
        const expected_section *expected = &expected_sections[i];
        ORACLE_CHECK(strcmp(actual->name, expected->name) == 0 &&
                     actual->virtual_address == expected->virtual_address &&
                     actual->virtual_size == expected->virtual_size &&
                     actual->raw_offset == expected->raw_offset &&
                     actual->raw_size == expected->raw_size &&
                     actual->copy_size == expected->copy_size,
                     "section %u plan differs", i);
    }

    image = (unsigned char *)malloc(GUEST_PE_EXPECTED_IMAGE_SIZE);
    ORACLE_CHECK(image != NULL, "Vita image oracle allocation failed");
    if (image) {
        error = guest_pe_map_image(raw, raw_size, vita_plan, image,
                                   GUEST_PE_EXPECTED_IMAGE_SIZE);
        ORACLE_CHECK(error == GUEST_PE_OK, "exact Vita map: %s",
                     guest_pe_error_string(error));
        guest_pe_sha256(image, GUEST_PE_EXPECTED_IMAGE_SIZE, digest);
        ORACLE_CHECK(memcmp(digest, guest_pe_expected_map_sha256, 32U) == 0,
                     "Vita mapped digest differs");
        error = guest_pe_map_image(raw, raw_size, vita_plan, image,
                                   GUEST_PE_EXPECTED_IMAGE_SIZE - 1U);
        ORACLE_CHECK(error == GUEST_PE_ERR_CONTRACT,
                     "short destination image was accepted: %s",
                     guest_pe_error_string(error));
        error = guest_pe_map_image(raw, raw_size, &pc_plan, image,
                                   GUEST_PE_EXPECTED_IMAGE_SIZE);
        ORACLE_CHECK(error == GUEST_PE_OK, "exact PC map: %s",
                     guest_pe_error_string(error));
        guest_pe_sha256(image, GUEST_PE_EXPECTED_IMAGE_SIZE, digest);
        ORACLE_CHECK(memcmp(digest, guest_pe_expected_pc_map_sha256, 32U) == 0,
                     "PC mapped digest differs");
        free(image);
    }
}

static void oracle_truncation_tests(
    const unsigned char *raw, size_t raw_size, const guest_pe_plan *plan)
{
    uint32_t pe_offset = oracle_u32(raw, 0x3cU);
    uint32_t section_table = pe_offset + 24U + 0xe0U;
    uint32_t cutoffs[64];
    size_t count = 0U, i;

    /* Production authenticates before parsing.  Exercise every possible
     * strict prefix, not merely representative header/section boundaries. */
    for (i = 0U; i < raw_size; ++i) {
        guest_pe_error error = guest_pe_authenticate(raw, i);
        ORACLE_CHECK(error == GUEST_PE_ERR_FILE_SIZE,
                     "production truncation %u reached parsing/hash",
                     (unsigned)i);
    }
    ORACLE_CHECK(guest_pe_authenticate(raw, raw_size + 1U) ==
                     GUEST_PE_ERR_FILE_SIZE,
                 "oversize production input reached parsing/hash");

    for (i = 0U; i < section_table + 5U * 40U; ++i) {
        guest_pe_plan truncated;
        guest_pe_error error = guest_pe_parse_bounded(
            raw, i, GUEST_PE_VITA_TARGET_BASE, &truncated);
        ORACLE_CHECK(error != GUEST_PE_OK,
                     "header truncation %u was accepted", (unsigned)i);
    }
    cutoffs[count++] = 63U;
    cutoffs[count++] = pe_offset + 23U;
    cutoffs[count++] = pe_offset + 24U + 0xe0U - 1U;
    cutoffs[count++] = section_table + 5U * 40U - 1U;
    cutoffs[count++] = GUEST_PE_EXPECTED_HEADER_SIZE - 1U;
    for (i = 0U; i < plan->section_count; ++i)
        cutoffs[count++] = plan->sections[i].raw_offset +
                           plan->sections[i].raw_size - 1U;
    for (i = 0U; i < GUEST_PE_DIRECTORY_COUNT; ++i) {
        uint32_t raw_offset;
        if (plan->directories[i].size &&
            guest_pe_rva_to_raw(plan, plan->directories[i].rva,
                                plan->directories[i].size, &raw_offset))
            cutoffs[count++] = raw_offset + plan->directories[i].size - 1U;
    }
    cutoffs[count++] = (uint32_t)raw_size - 1U;
    for (i = 0U; i < count; ++i) {
        guest_pe_plan truncated;
        guest_pe_error error = guest_pe_parse_bounded(
            raw, cutoffs[i], GUEST_PE_VITA_TARGET_BASE, &truncated);
        ORACLE_CHECK(error != GUEST_PE_OK,
                     "structural truncation %08x was accepted", cutoffs[i]);
    }

    {
        uint32_t reloc_raw;
        ORACLE_CHECK(guest_pe_rva_to_raw(
                         plan, plan->directories[5].rva,
                         plan->directories[5].size, &reloc_raw),
                     "cannot locate relocation directory for truncation test");
        if (guest_pe_rva_to_raw(plan, plan->directories[5].rva,
                                plan->directories[5].size, &reloc_raw)) {
            guest_pe_plan copy = *plan;
            guest_pe_error error = guest_pe_validate_relocations_raw(
                raw, (size_t)reloc_raw + plan->directories[5].size - 1U,
                &copy);
            ORACLE_CHECK(error == GUEST_PE_ERR_RELOC_DIRECTORY,
                         "truncated relocation directory: %s",
                         guest_pe_error_string(error));
        }
    }
}

static void oracle_mutation_tests(
    const unsigned char *raw, size_t raw_size, const guest_pe_plan *plan)
{
    unsigned char *scratch = (unsigned char *)malloc(raw_size);
    unsigned char *image = (unsigned char *)malloc(GUEST_PE_EXPECTED_IMAGE_SIZE);
    uint32_t pe_offset = oracle_u32(raw, 0x3cU);
    uint32_t optional = pe_offset + 24U;
    uint32_t sections = optional + 0xe0U;
    size_t type3 = oracle_find_reloc_entry(raw, plan, 3U);
    size_t last_block = oracle_last_reloc_block(raw, plan);
    uint32_t reloc_raw = 0U;
    guest_pe_plan parsed;
    guest_pe_error error;
    unsigned index;

    ORACLE_CHECK(scratch != NULL && image != NULL,
                 "mutation buffers could not be allocated");
    if (!scratch || !image) {
        free(scratch); free(image); return;
    }
#define RESET_MUTATION() memcpy(scratch, raw, raw_size)

    RESET_MUTATION(); scratch[0] = 'N';
    oracle_expect_parse("bad MZ", scratch, raw_size, GUEST_PE_ERR_DOS_HEADER);
    RESET_MUTATION(); oracle_put_u32(scratch, 0x3cU, UINT32_MAX);
    oracle_expect_parse("PE offset overflow", scratch, raw_size,
                        GUEST_PE_ERR_PE_OFFSET);
    RESET_MUTATION(); oracle_put_u32(scratch, pe_offset, 0U);
    oracle_expect_parse("bad PE signature", scratch, raw_size,
                        GUEST_PE_ERR_PE_SIGNATURE);
    RESET_MUTATION(); oracle_put_u16(scratch, pe_offset + 4U, 0x8664U);
    oracle_expect_parse("wrong machine", scratch, raw_size,
                        GUEST_PE_ERR_COFF_HEADER);
    RESET_MUTATION(); oracle_put_u16(scratch, pe_offset + 6U, UINT16_MAX);
    oracle_expect_parse("section count overflow", scratch, raw_size,
                        GUEST_PE_ERR_COFF_HEADER);
    RESET_MUTATION(); oracle_put_u16(scratch, pe_offset + 20U, UINT16_MAX);
    oracle_expect_parse("optional size overflow", scratch, raw_size,
                        GUEST_PE_ERR_COFF_HEADER);
    RESET_MUTATION(); oracle_put_u16(scratch, optional, 0x020bU);
    oracle_expect_parse("PE32+ magic", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 16U, 0x1000U);
    oracle_expect_parse("wrong entry", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 28U, 0x00500000U);
    oracle_expect_parse("wrong image base", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 56U, UINT32_MAX);
    oracle_expect_parse("image size overflow", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 60U, UINT32_MAX);
    oracle_expect_parse("header size overflow", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 92U, UINT32_MAX);
    oracle_expect_parse("directory count overflow", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);

    RESET_MUTATION(); oracle_put_u32(scratch, sections + 20U, UINT32_MAX);
    oracle_expect_parse("section raw offset overflow", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_SOURCE);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 16U, UINT32_MAX);
    oracle_expect_parse("section raw size overflow", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_SOURCE);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 12U, UINT32_MAX);
    oracle_expect_parse("section VA overflow", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_DESTINATION);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 8U, UINT32_MAX);
    oracle_expect_parse("section virtual size overflow", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_DESTINATION);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 40U + 12U,
                                     plan->sections[0].virtual_address);
    oracle_expect_parse("mapped section overlap", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_OVERLAP);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 40U + 20U,
                                     plan->sections[0].raw_offset);
    oracle_expect_parse("raw section overlap", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_OVERLAP);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 12U,
                                     plan->sections[0].virtual_address + 1U);
    oracle_expect_parse("unaligned section VA", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_DESTINATION);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 20U,
                                     plan->sections[0].raw_offset + 1U);
    oracle_expect_parse("unaligned section raw", scratch, raw_size,
                        GUEST_PE_ERR_SECTION_SOURCE);
    RESET_MUTATION(); oracle_put_u32(scratch, sections + 36U,
                                     plan->sections[0].characteristics &
                                     ~0x20000000U);
    oracle_expect_parse("non-executable entry", scratch, raw_size,
                        GUEST_PE_ERR_ENTRY_POINT);

    for (index = 0U; index < plan->section_count; ++index) {
        guest_pe_plan rejected;
        size_t section = (size_t)sections + (size_t)index * 40U;
        RESET_MUTATION(); oracle_put_u32(scratch, section + 20U, UINT32_MAX);
        error = guest_pe_parse_bounded(scratch, raw_size,
                                       GUEST_PE_VITA_TARGET_BASE, &rejected);
        ORACLE_CHECK(error != GUEST_PE_OK,
                     "section %u accepted overflowing raw offset", index);
        RESET_MUTATION(); oracle_put_u32(scratch, section + 12U, UINT32_MAX);
        error = guest_pe_parse_bounded(scratch, raw_size,
                                       GUEST_PE_VITA_TARGET_BASE, &rejected);
        ORACLE_CHECK(error != GUEST_PE_OK,
                     "section %u accepted overflowing image VA", index);
    }

    RESET_MUTATION(); oracle_put_u32(scratch, optional + 96U, UINT32_MAX);
    oracle_expect_parse("directory RVA overflow", scratch, raw_size,
                        GUEST_PE_ERR_DIRECTORY);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 96U, 0U);
    oracle_put_u32(scratch, optional + 100U, 1U);
    oracle_expect_parse("half-empty directory", scratch, raw_size,
                        GUEST_PE_ERR_DIRECTORY);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 96U + 4U * 8U, 1U);
    oracle_put_u32(scratch, optional + 100U + 4U * 8U, 1U);
    oracle_expect_parse("security directory", scratch, raw_size,
                        GUEST_PE_ERR_DIRECTORY);
    RESET_MUTATION(); oracle_put_u32(
        scratch, optional + 96U,
        plan->sections[2].virtual_address + plan->sections[2].copy_size);
    oracle_put_u32(scratch, optional + 100U, 4U);
    oracle_expect_parse("directory in zero-fill", scratch, raw_size,
                        GUEST_PE_ERR_DIRECTORY_RAW);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 96U,
                                     plan->image_size - 3U);
    oracle_put_u32(scratch, optional + 100U, 4U);
    oracle_expect_parse("directory end overflow", scratch, raw_size,
                        GUEST_PE_ERR_DIRECTORY);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 100U + 5U * 8U,
                                     plan->directories[5].size - 2U);
    oracle_expect_parse("wrong relocation directory", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    RESET_MUTATION(); oracle_put_u32(scratch, optional + 100U + 9U * 8U,
                                     plan->directories[9].size - 1U);
    oracle_expect_parse("wrong TLS directory", scratch, raw_size,
                        GUEST_PE_ERR_CONTRACT);
    for (index = 0U; index < GUEST_PE_DIRECTORY_COUNT; ++index) {
        guest_pe_plan rejected;
        size_t directory = (size_t)optional + 96U + (size_t)index * 8U;
        RESET_MUTATION();
        oracle_put_u32(scratch, directory, plan->image_size - 3U);
        oracle_put_u32(scratch, directory + 4U, 4U);
        error = guest_pe_parse_bounded(scratch, raw_size,
                                       GUEST_PE_VITA_TARGET_BASE, &rejected);
        ORACLE_CHECK(error != GUEST_PE_OK,
                     "directory %u accepted an overflowing span", index);
        RESET_MUTATION();
        oracle_put_u32(scratch, directory, 0U);
        oracle_put_u32(scratch, directory + 4U, 1U);
        error = guest_pe_parse_bounded(scratch, raw_size,
                                       GUEST_PE_VITA_TARGET_BASE, &rejected);
        ORACLE_CHECK(error != GUEST_PE_OK,
                     "directory %u accepted a half-empty span", index);
    }

    ORACLE_CHECK(guest_pe_rva_to_raw(
                     plan, plan->directories[5].rva,
                     plan->directories[5].size, &reloc_raw),
                 "cannot locate relocation raw data");
    if (reloc_raw) {
        RESET_MUTATION(); oracle_put_u32(scratch, reloc_raw + 4U, 6U);
        oracle_expect_parse("short relocation block", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_BLOCK);
        RESET_MUTATION(); oracle_put_u32(scratch, reloc_raw + 4U, 9U);
        oracle_expect_parse("odd relocation block", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_BLOCK);
        RESET_MUTATION(); oracle_put_u32(
            scratch, reloc_raw + 4U, plan->directories[5].size + 2U);
        oracle_expect_parse("oversize relocation block", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_BLOCK);
        RESET_MUTATION(); oracle_put_u32(
            scratch, reloc_raw, oracle_u32(raw, reloc_raw) | 1U);
        oracle_expect_parse("unaligned relocation page", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_BLOCK);
        RESET_MUTATION(); oracle_put_u32(scratch, reloc_raw, plan->image_size);
        oracle_expect_parse("relocation page outside image", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_BLOCK);
    }
    ORACLE_CHECK(type3 != SIZE_MAX, "no HIGHLOW entry found");
    if (type3 != SIZE_MAX) {
        uint16_t original = oracle_u16(raw, type3);
        RESET_MUTATION();
        oracle_put_u16(scratch, type3,
                       (uint16_t)(0x2000U | (original & 0x0fffU)));
        oracle_expect_parse("unsupported relocation type", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_TYPE);
        RESET_MUTATION(); oracle_put_u16(scratch, type3,
                                         (uint16_t)(original & 0x0fffU));
        oracle_expect_parse("relocation census change", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_COUNT);
        RESET_MUTATION();
        oracle_put_u32(scratch, reloc_raw, plan->image_size - 0x1000U);
        oracle_put_u16(scratch, type3, 0x3ffdU);
        oracle_expect_parse("relocation target crosses image", scratch,
                            raw_size, GUEST_PE_ERR_RELOC_TARGET);

        RESET_MUTATION();
        oracle_put_u16(scratch, type3,
                       (uint16_t)(0x2000U | (original & 0x0fffU)));
        error = guest_pe_map_image(scratch, raw_size, plan, image,
                                   GUEST_PE_EXPECTED_IMAGE_SIZE);
        ORACLE_CHECK(error == GUEST_PE_ERR_RELOC_TYPE,
                     "mapper did not revalidate relocations: %s",
                     guest_pe_error_string(error));
    }
    ORACLE_CHECK(last_block != SIZE_MAX, "cannot locate last relocation block");
    if (last_block != SIZE_MAX) {
        uint32_t last_size = oracle_u32(raw, last_block + 4U);
        RESET_MUTATION();
        oracle_put_u32(scratch, last_block + 4U, last_size - 2U);
        oracle_expect_parse("trailing relocation bytes", scratch, raw_size,
                            GUEST_PE_ERR_RELOC_BLOCK);
    }

    parsed = *plan;
    parsed.sections[0].raw_offset = UINT32_MAX;
    error = guest_pe_map_image(raw, raw_size, &parsed, image,
                               GUEST_PE_EXPECTED_IMAGE_SIZE);
    ORACLE_CHECK(error == GUEST_PE_ERR_SECTION_SOURCE,
                 "mapper accepted corrupt source plan: %s",
                 guest_pe_error_string(error));
    parsed = *plan;
    parsed.sections[0].virtual_address = UINT32_MAX;
    error = guest_pe_map_image(raw, raw_size, &parsed, image,
                               GUEST_PE_EXPECTED_IMAGE_SIZE);
    ORACLE_CHECK(error == GUEST_PE_ERR_SECTION_DESTINATION,
                 "mapper accepted corrupt destination plan: %s",
                 guest_pe_error_string(error));
    RESET_MUTATION(); scratch[2] ^= 1U;
    error = guest_pe_map_image(scratch, raw_size, plan, image,
                               GUEST_PE_EXPECTED_IMAGE_SIZE);
    ORACLE_CHECK(error == GUEST_PE_ERR_MAP_HASH,
                 "mapper accepted changed authenticated bytes: %s",
                 guest_pe_error_string(error));

    RESET_MUTATION(); scratch[raw_size / 2U] ^= 1U;
    ORACLE_CHECK(guest_pe_authenticate(scratch, raw_size) ==
                     GUEST_PE_ERR_FILE_HASH,
                 "same-size file mutation passed SHA gate");
    ORACLE_CHECK(guest_pe_authenticate(raw, raw_size - 1U) ==
                     GUEST_PE_ERR_FILE_SIZE,
                 "wrong file size passed exact gate");

#undef RESET_MUTATION
    free(image);
    free(scratch);
}

enum fake_failure {
    FAKE_NONE = 0,
    FAKE_OPEN,
    FAKE_SIZE_IO,
    FAKE_WRONG_SIZE,
    FAKE_REWIND,
    FAKE_RAW_ALLOC,
    FAKE_SHORT_READ,
    FAKE_CLOSE,
    FAKE_IMAGE_ALLOC,
    FAKE_BAD_HASH
};

typedef struct fake_loader {
    const unsigned char *source;
    size_t source_size;
    enum fake_failure failure;
    unsigned open_calls, close_calls, raw_alloc_calls, raw_free_calls;
    unsigned image_alloc_calls, image_free_calls;
    int open_live, raw_live, image_live;
} fake_loader;

static void *fake_open(void *context, const char *path)
{
    fake_loader *fake = (fake_loader *)context;
    (void)path;
    ++fake->open_calls;
    if (fake->failure == FAKE_OPEN)
        return NULL;
    ++fake->open_live;
    return fake;
}

static int fake_size(void *context, void *file, uint64_t *size_out)
{
    fake_loader *fake = (fake_loader *)context;
    (void)file;
    if (fake->failure == FAKE_SIZE_IO)
        return 1;
    *size_out = fake->failure == FAKE_WRONG_SIZE
        ? (uint64_t)fake->source_size - 1U : (uint64_t)fake->source_size;
    return 0;
}

static int fake_rewind(void *context, void *file)
{
    fake_loader *fake = (fake_loader *)context;
    (void)file;
    return fake->failure == FAKE_REWIND ? 1 : 0;
}

static size_t fake_read(
    void *context, void *file, unsigned char *destination, size_t size)
{
    fake_loader *fake = (fake_loader *)context;
    size_t actual = fake->failure == FAKE_SHORT_READ ? size - 1U : size;
    (void)file;
    if (actual > fake->source_size)
        actual = fake->source_size;
    memcpy(destination, fake->source, actual);
    if (fake->failure == FAKE_BAD_HASH && actual)
        destination[actual / 2U] ^= 1U;
    return actual;
}

static int fake_close(void *context, void *file)
{
    fake_loader *fake = (fake_loader *)context;
    (void)file;
    ++fake->close_calls;
    --fake->open_live;
    return fake->failure == FAKE_CLOSE ? 1 : 0;
}

static void *fake_raw_alloc(void *context, size_t size)
{
    fake_loader *fake = (fake_loader *)context;
    void *allocation;
    ++fake->raw_alloc_calls;
    if (fake->failure == FAKE_RAW_ALLOC)
        return NULL;
    allocation = malloc(size);
    if (allocation)
        ++fake->raw_live;
    return allocation;
}

static void fake_raw_free(void *context, void *allocation)
{
    fake_loader *fake = (fake_loader *)context;
    ++fake->raw_free_calls;
    --fake->raw_live;
    free(allocation);
}

static void *fake_image_alloc(
    void *context, uint32_t address, uint32_t size)
{
    fake_loader *fake = (fake_loader *)context;
    void *allocation;
    ORACLE_CHECK(address == GUEST_PE_VITA_TARGET_BASE &&
                 size == GUEST_PE_EXPECTED_IMAGE_SIZE,
                 "transaction requested wrong image contract");
    ++fake->image_alloc_calls;
    if (fake->failure == FAKE_IMAGE_ALLOC)
        return NULL;
    allocation = malloc(size);
    if (allocation)
        ++fake->image_live;
    return allocation;
}

static void fake_image_free(void *context, void *allocation)
{
    fake_loader *fake = (fake_loader *)context;
    ++fake->image_free_calls;
    --fake->image_live;
    free(allocation);
}

static guest_pe_loader_ops fake_ops(fake_loader *fake)
{
    guest_pe_loader_ops ops = {
        fake, fake_open, fake_size, fake_rewind, fake_read, fake_close,
        fake_raw_alloc, fake_raw_free, fake_image_alloc, fake_image_free
    };
    return ops;
}

static void oracle_transaction_tests(
    const unsigned char *raw, size_t raw_size)
{
    static const struct {
        enum fake_failure failure;
        guest_pe_error error;
    } cases[] = {
        { FAKE_OPEN, GUEST_PE_ERR_OPEN },
        { FAKE_SIZE_IO, GUEST_PE_ERR_SIZE_IO },
        { FAKE_WRONG_SIZE, GUEST_PE_ERR_FILE_SIZE },
        { FAKE_REWIND, GUEST_PE_ERR_REWIND },
        { FAKE_RAW_ALLOC, GUEST_PE_ERR_RAW_ALLOC },
        { FAKE_SHORT_READ, GUEST_PE_ERR_READ },
        { FAKE_CLOSE, GUEST_PE_ERR_CLOSE },
        { FAKE_BAD_HASH, GUEST_PE_ERR_FILE_HASH },
        { FAKE_IMAGE_ALLOC, GUEST_PE_ERR_IMAGE_ALLOC }
    };
    fake_loader fake;
    guest_pe_loader_ops ops;
    guest_pe_loaded_image loaded;
    guest_pe_error error;
    size_t i;

    memset(&loaded, 0xa5, sizeof loaded);
    error = guest_pe_load_file(NULL, "memory", GUEST_PE_VITA_TARGET_BASE,
                               &loaded);
    ORACLE_CHECK(error == GUEST_PE_ERR_ARGUMENT && loaded.image == NULL,
                 "null loader ops did not fail closed");

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(&fake, 0, sizeof fake);
        fake.source = raw;
        fake.source_size = raw_size;
        fake.failure = cases[i].failure;
        ops = fake_ops(&fake);
        error = guest_pe_load_file(&ops, "memory", GUEST_PE_VITA_TARGET_BASE,
                                   &loaded);
        ORACLE_CHECK(error == cases[i].error,
                     "transaction failure %u: expected %s, got %s",
                     (unsigned)cases[i].failure,
                     guest_pe_error_string(cases[i].error),
                     guest_pe_error_string(error));
        ORACLE_CHECK(loaded.image == NULL && fake.open_live == 0 &&
                     fake.raw_live == 0 && fake.image_live == 0,
                     "transaction failure %u leaked state",
                     (unsigned)cases[i].failure);

        /* The exact same ops/context must be reusable after every failure. */
        fake.failure = FAKE_NONE;
        error = guest_pe_load_file(&ops, "memory", GUEST_PE_VITA_TARGET_BASE,
                                   &loaded);
        ORACLE_CHECK(error == GUEST_PE_OK && loaded.image != NULL &&
                     fake.open_live == 0 && fake.raw_live == 0 &&
                     fake.image_live == 1,
                     "retry after failure %u did not succeed: %s",
                     (unsigned)cases[i].failure,
                     guest_pe_error_string(error));
        guest_pe_release_loaded(&ops, &loaded);
        ORACLE_CHECK(fake.image_live == 0,
                     "retry release after failure %u leaked image",
                     (unsigned)cases[i].failure);
    }
}

int main(int argc, char **argv)
{
    unsigned char *raw;
    size_t raw_size = 0U;
    guest_pe_plan vita_plan;

    if (argc != 2) {
        fprintf(stderr, "usage: guest-pe-loader-oracle <exact-unpacked-pe>\n");
        return 2;
    }
    raw = oracle_read_file(argv[1], &raw_size);
    if (!raw) {
        fprintf(stderr, "guest PE oracle: cannot read %s\n", argv[1]);
        return 2;
    }
    ORACLE_CHECK(raw_size == GUEST_PE_EXPECTED_FILE_SIZE,
                 "input size %u differs from %u", (unsigned)raw_size,
                 GUEST_PE_EXPECTED_FILE_SIZE);
    oracle_sha_tests();
    oracle_exact_plan_tests(raw, raw_size, &vita_plan);
    if (!failures) {
        oracle_truncation_tests(raw, raw_size, &vita_plan);
        oracle_mutation_tests(raw, raw_size, &vita_plan);
        oracle_transaction_tests(raw, raw_size);
    }
    free(raw);
    if (failures) {
        fprintf(stderr, "guest PE loader oracle: FAIL (%d checks)\n", failures);
        return 1;
    }
    puts("guest PE loader oracle: PASS (bounded parse/map/auth/transaction)");
    return 0;
}
