// Tests for the .sfv half: the CRC-32 implementation against the published check
// value, and the parser against the shapes real .sfv writers produce rather than
// against the one shape the format description shows.

#include "../src/sfv.h"
#include "test_os.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static void sh(const std::string& cmd) {
    int rc = test_system(cmd);
    (void)rc;
}

// Single-quote a string for the shell.
static std::string q(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

static void write_file(const std::string& path, const std::string& content) {
    // Written with C rather than through the shell: a fixture line with a backslash
    // in it must reach the file unchanged, and Linux caps one shell argument at 128
    // KiB, which the multi-chunk CRC fixture is bigger than.
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { failures++; printf("FAIL cannot write %s\n", path.c_str()); return; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

static std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// The table and the loop, checked against the value every CRC-32 implementation
// agrees on. Everything else in this part is built on top of this one number.
static void test_crc(void) {
    const char* probe = "123456789";
    CHECK(sfv_crc32(probe, 9) == 0xCBF43926UL);
    CHECK(sfv_crc32("", 0) == 0UL);
    CHECK(sfv_crc32("a", 1) == 0xE8B7BE43UL);

    char hex[16];
    sfv_format_crc(0xCBF43926UL, hex, sizeof(hex));
    CHECK(!strcmp(hex, "CBF43926"));
    sfv_format_crc(0x0000000FUL, hex, sizeof(hex));
    CHECK(!strcmp(hex, "0000000F"));   // eight digits, leading zeros kept
}

static void test_crc_file(const std::string& root) {
    const std::string f = root + "/crc-probe.bin";
    write_file(f, "123456789");
    unsigned long got = 0;
    std::string err;
    CHECK(sfv_crc32_file(f.c_str(), &got, &err));
    CHECK(got == 0xCBF43926UL);

    // The chunked read has to agree with the buffer form on something bigger than
    // one chunk, which is where an off-by-sixty-four-kilobytes bug would show up.
    std::string big;
    for (int i = 0; i < 200; i++) big += std::string(1024, (char)('A' + (i % 26)));
    write_file(f, big);
    unsigned long via_file = 0, via_buffer = sfv_crc32(big.data(), big.size());
    CHECK(sfv_crc32_file(f.c_str(), &via_file, &err));
    CHECK(via_file == via_buffer);
    CHECK(via_file != 0UL);

    // A file that is not there is a different answer from a file whose checksum
    // does not match, and the call has to say which one it is.
    CHECK(!sfv_crc32_file((root + "/no-such-file.bin").c_str(), &got, &err));
    CHECK(!err.empty());
    sh("rm -f " + q(f));
}

static void test_parse(const std::string& root) {
    const std::string sfv = root + "/set.sfv";

    // The ordinary shape, plus the ones the format description leaves out.
    write_file(sfv,
               "; a comment line\r\n"
               "# another comment\r\n"
               "\r\n"
               "one.bin 1A2B3C4D\r\n"
               "two.bin 0000ABCD\n"
               "The Set (1998)/disc 1.bin 12345678\n"
               "   spaced.bin   DEADBEEF   \n"
               "87654321 crc-first.bin\n"
               "folder\\sub\\file.rar CAFE0001\n"
               "set.sfv 11111111\n"
               "not-a-checksum here\n"
               "12345678\n");
    SfvParse p;
    std::string err;
    CHECK(sfv_parse(sfv.c_str(), sfv.c_str(), &p, &err));
    CHECK(p.entries.size() == 6);
    CHECK(p.malformed == 2);            // the two lines with no name-and-checksum pair
    CHECK(p.self_named == 1);           // the .sfv that lists itself
    CHECK(p.crc_first == 1);

    bool found = false;
    for (const SfvEntry& e : p.entries) {
        if (e.name == "one.bin") { found = true; CHECK(e.crc == 0x1A2B3C4DUL); }
        if (e.name == "two.bin") CHECK(e.crc == 0x0000ABCDUL);
        // A name with spaces in it is one entry, not a malformed line: the
        // checksum is the last token and the name is everything before it.
        if (e.name == "The Set (1998)/disc 1.bin") { found = true; CHECK(e.crc == 0x12345678UL); }
        if (e.name == "spaced.bin") CHECK(e.crc == 0xDEADBEEFUL);
        // A Windows writer's separators are translated, not dropped: the file is
        // here and correct, and reporting it missing would be a wrong failure.
        if (e.name == "folder/sub/file.rar") CHECK(e.crc == 0xCAFE0001UL);
        if (e.name == "crc-first.bin") { found = true; CHECK(e.crc == 0x87654321UL); CHECK(e.crc_first); }
    }
    CHECK(found);
    for (const SfvEntry& e : p.entries)
        CHECK(e.name.find('\r') == std::string::npos);   // no name ends in a CR

    // A file genuinely named with eight hex digits keeps its name: the last token
    // is the checksum, and the one before it is the name.
    write_file(sfv, "deadbeef 12345678\n");
    CHECK(sfv_parse(sfv.c_str(), sfv.c_str(), &p, &err));
    CHECK(p.entries.size() == 1);
    CHECK(p.entries[0].name == "deadbeef");
    CHECK(p.entries[0].crc == 0x12345678UL);
    CHECK(p.malformed == 0);

    // An .sfv with no comment and no CRLF at all, and one that is empty.
    write_file(sfv, "plain.bin AAAAAAAA\n");
    CHECK(sfv_parse(sfv.c_str(), sfv.c_str(), &p, &err));
    CHECK(p.entries.size() == 1 && p.entries[0].name == "plain.bin");
    write_file(sfv, "");
    CHECK(sfv_parse(sfv.c_str(), sfv.c_str(), &p, &err));
    CHECK(p.entries.empty());
    CHECK(p.malformed == 0);

    // A name longer than the read buffer arrives in pieces and is joined: one long
    // name is one entry, not a name plus a malformed line.
    std::string long_name(5000, 'n');
    write_file(sfv, long_name + " 1234ABCD\n");
    CHECK(sfv_parse(sfv.c_str(), sfv.c_str(), &p, &err));
    CHECK(p.entries.size() == 1);
    CHECK(p.entries.size() == 1 && p.entries[0].name == long_name);
    CHECK(p.malformed == 0);

    // The file is not there: false with a reason, not an empty parse.
    CHECK(!sfv_parse((root + "/no-such.sfv").c_str(), nullptr, &p, &err));
    CHECK(!err.empty());

    sh("rm -f " + q(sfv));
}

static void test_find(const std::string& root) {
    const std::string d = root + "/sfv-find";
    sh("rm -rf " + q(d) + " && mkdir -p " + q(d));
    write_file(d + "/notes.txt", "not a checksum file\n");
    std::string found;
    CHECK(!sfv_find(d.c_str(), &found));            // no .sfv here

    write_file(d + "/the-set-of-files.sfv", "a.bin 11111111\n");
    write_file(d + "/set.sfv", "b.bin 22222222\n");
    CHECK(sfv_find(d.c_str(), &found));
    // The shortest name is the one a set ships: `set.sfv`, not the long one.
    CHECK(found == d + "/set.sfv");

    CHECK(!sfv_find((root + "/no-such-dir").c_str(), &found));
    sh("rm -rf " + q(d));
}

int main(void) {
    std::string tmp_dir = test_mkdtemp("mc-sfv");
    const char* dir = tmp_dir.empty() ? nullptr : tmp_dir.c_str();
    if (!dir) { printf("cannot make a temp dir\n"); return 1; }
    std::string root(dir);

    test_crc();
    test_crc_file(root);
    test_parse(root);
    test_find(root);

    sh("rm -rf '" + root + "'");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
