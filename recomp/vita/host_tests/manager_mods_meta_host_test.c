/*
 * Mod display-name host test (manager_mods_meta_host_test):
 * <name> then <title>, attributes on the tag, <name/>, entities (named,
 * decimal, hex, unknown, bare '&'), whitespace collapse, control characters
 * dropped, the 90-character cut on a code point boundary (Cyrillic), the
 * 4 KiB head (a name past it is not seen), nested markup skipped, version,
 * and the directory-name fallback for absent / unreadable / nameless files.
 */
#define FAKE_DATA_CAPACITY 8192u
#include "fake_fs.h"
#include "manager_font.h"
#include "manager_mods_meta.h"

static struct fake_fs fs;

static void scan(const char *xml, struct manager_mod_meta *meta)
{
    (void)manager_mods_meta_scan(xml, strlen(xml), meta);
}

static void test_scan(void)
{
    struct manager_mod_meta meta;
    char xml[1024];
    char expected[400];
    size_t index;

    scan("<metadata><name>External Item Descriptions</name>"
         "<version>4.92</version></metadata>", &meta);
    require(strcmp(meta.name, "External Item Descriptions") == 0 &&
                strcmp(meta.version, "4.92") == 0 && meta.from_metadata == 1u,
            "name and version");

    scan("<metadata><title>Only a title</title></metadata>", &meta);
    require(strcmp(meta.name, "Only a title") == 0, "title fallback");

    scan("<metadata><name/><name>   </name><title>T</title></metadata>", &meta);
    require(strcmp(meta.name, "T") == 0,
            "self-closed and blank <name> fall back to <title>");

    scan("<metadata><name lang=\"en\">\n\t  Spaced \r\n  out\t</name></metadata>",
         &meta);
    require(strcmp(meta.name, "Spaced out") == 0,
            "attributes, whitespace collapsed and trimmed");

    scan("<metadata><name>A &amp; B &lt;3&gt; &quot;x&quot; &apos;y&apos; "
         "&#1046;&#x416; &bogus; & end</name></metadata>", &meta);
    require(strcmp(meta.name,
                   "A & B <3> \"x\" 'y' \xd0\x96\xd0\x96 end") == 0,
            "entities decoded, unknown and bare & dropped");

    scan("<metadata><name>ctl\x01\x02\x7f""chars</name></metadata>", &meta);
    require(strcmp(meta.name, "ctlchars") == 0, "control characters dropped");

    scan("<metadata><name>Mod <b>bold</b> name</name></metadata>", &meta);
    require(strcmp(meta.name, "Mod bold name") == 0, "nested markup skipped");

    scan("<metadata><namespace>x</namespace><name>Real</name></metadata>",
         &meta);
    require(strcmp(meta.name, "Real") == 0, "<namespace> is not <name>");

    scan("<metadata><name>unterminated", &meta);
    require(meta.name[0] == '\0' && meta.from_metadata == 0u,
            "unterminated element -> no name");

    /* 120 Cyrillic letters: cut at 90 code points = 180 bytes. */
    strcpy(xml, "<metadata><name>");
    for (index = 0u; index < 120u; ++index)
        strcat(xml, "\xd0\x96");
    strcat(xml, "</name></metadata>");
    scan(xml, &meta);
    require(manager_utf8_length(meta.name) == 90u && strlen(meta.name) == 180u,
            "cut at 90 code points");
    expected[0] = '\0';
    for (index = 0u; index < 90u; ++index)
        strcat(expected, "\xd0\x96");
    require(strcmp(meta.name, expected) == 0, "cut on a code point boundary");

    scan("<metadata><name>n</name><version> 1.0  beta\n</version></metadata>",
         &meta);
    require(strcmp(meta.version, "1.0 beta") == 0, "version cleaned");
    scan("<metadata><name>n</name><version>12345678901234567890</version>",
         &meta);
    require(strcmp(meta.version, "1234567890123456") == 0, "version cut at 16");
}

static void test_read(void)
{
    struct manager_storage_io io;
    struct manager_mod_meta meta;
    static char big[6000];
    int result;

    fake_init(&fs);
    io = fake_io(&fs);
    fake_add_dir(&fs, MANAGER_STORAGE_MOD_ROOT "/836319872");
    fake_add_file(&fs, MANAGER_STORAGE_MOD_ROOT "/836319872/metadata.xml",
                  "<?xml version=\"1.0\"?>\n<metadata>\n"
                  "    <name>External Item Descriptions</name>\n"
                  "    <directory>external item descriptions_836319872"
                  "</directory>\n    <id>836319872</id>\n"
                  "    <version>4.92</version>\n</metadata>\n");
    result = manager_mods_meta_read(&io, "836319872", &meta);
    require(result == MANAGER_STORAGE_OK &&
                strcmp(meta.name, "External Item Descriptions") == 0 &&
                strcmp(meta.version, "4.92") == 0 && meta.from_metadata,
            "read from the fake FS");

    fake_add_dir(&fs, MANAGER_STORAGE_MOD_ROOT "/vita-perf");
    result = manager_mods_meta_read(&io, "vita-perf", &meta);
    require(result == MANAGER_STORAGE_ABSENT &&
                strcmp(meta.name, "vita-perf") == 0 && !meta.from_metadata &&
                meta.version[0] == '\0',
            "no metadata.xml -> directory name");

    fake_fail(&fs, FAKE_OP_READ, 1);
    result = manager_mods_meta_read(&io, "836319872", &meta);
    fake_no_fail(&fs);
    require(result == MANAGER_STORAGE_IO_ERROR &&
                strcmp(meta.name, "836319872") == 0 && !meta.from_metadata,
            "unreadable metadata.xml -> directory name");

    /* A name beyond the first 4 KiB is not seen. */
    memset(big, ' ', sizeof(big) - 1u);
    memcpy(big, "<metadata><description>", 23u);
    memcpy(big + 5000, "</description><name>Late</name></metadata>", 42u);
    big[sizeof(big) - 1u] = '\0';
    fake_add_dir(&fs, MANAGER_STORAGE_MOD_ROOT "/late");
    fake_add_file(&fs, MANAGER_STORAGE_MOD_ROOT "/late/metadata.xml", big);
    result = manager_mods_meta_read(&io, "late", &meta);
    require(result == MANAGER_STORAGE_OK && strcmp(meta.name, "late") == 0,
            "name past the 4 KiB head -> directory name");

    fake_add_dir(&fs, MANAGER_STORAGE_MOD_ROOT "/nameless");
    fake_add_file(&fs, MANAGER_STORAGE_MOD_ROOT "/nameless/metadata.xml",
                  "<metadata><version>2</version></metadata>");
    result = manager_mods_meta_read(&io, "nameless", &meta);
    require(result == MANAGER_STORAGE_OK && strcmp(meta.name, "nameless") == 0 &&
                strcmp(meta.version, "2") == 0,
            "version without a name keeps the version");
    require_file(&fs, MANAGER_STORAGE_MOD_ROOT "/nameless/metadata.xml",
                 "<metadata><version>2</version></metadata>",
                 "reading never writes");
    require(fs.operation_counts[FAKE_OP_OPEN_WRITE] == 0 &&
                fs.operation_counts[FAKE_OP_RENAME] == 0 &&
                fs.operation_counts[FAKE_OP_REMOVE] == 0,
            "no write, rename or remove");
}

int main(void)
{
    test_scan();
    test_read();
    printf("manager mods meta host test: %u checks PASS\n", checks);
    return 0;
}
