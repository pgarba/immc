// Tests for the archive backend. It shells out to zip/unzip, tar and 7z, so
// the fixtures are real archives built with those same tools and the parsers
// are checked against their actual output. Kinds whose tool is missing are
// skipped rather than failed.

#include "../src/archive.h"

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
    int rc = system(cmd.c_str());
    (void)rc;
}

// Single-quote a string for the shell.
static std::string q(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

static void write_file(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
}

static bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }

static bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static long filesize(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

static std::string slurp(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static const ArchEntry* find(const std::vector<ArchEntry>& v, const char* rel) {
    for (const ArchEntry& e : v) if (!strcmp(e.rel, rel)) return &e;
    return nullptr;
}

// ---------------------------------------------------------------------------
// progress bookkeeping
// ---------------------------------------------------------------------------

struct Prog {
    int calls = 0;
    int max_files = 0;
    long long max_bytes = 0;
    int total_files = 0;
    long long total_bytes = 0;
    bool monotonic = true;
    int last_files = 0;
    long long last_bytes = 0;
    int stop_after = -1;           // abort once this many calls have happened
};

static bool prog_cb(long long done, long long total, int df, int tf, void* user) {
    Prog* p = (Prog*)user;
    if (df < p->last_files || done < p->last_bytes) p->monotonic = false;
    p->last_files = df;
    p->last_bytes = done;
    if (df > p->max_files) p->max_files = df;
    if (done > p->max_bytes) p->max_bytes = done;
    p->total_files = tf;
    p->total_bytes = total;
    p->calls++;
    return p->stop_after < 0 || p->calls < p->stop_after;
}

// ---------------------------------------------------------------------------

static void test_kind_of(void) {
    CHECK(arch_kind_of("/x/y.ZIP") == ARCH_ZIP);
    CHECK(arch_kind_of("a.tar") == ARCH_TAR);
    CHECK(arch_kind_of("a.tar.gz") == ARCH_TAR_GZ);
    CHECK(arch_kind_of("a.tgz") == ARCH_TAR_GZ);
    CHECK(arch_kind_of("a.tar.bz2") == ARCH_TAR_BZ2);
    CHECK(arch_kind_of("a.tar.xz") == ARCH_TAR_XZ);
    CHECK(arch_kind_of("a.7z") == ARCH_7Z);
    CHECK(arch_kind_of("a.rar") == ARCH_RAR);
    CHECK(arch_kind_of("a.txt") == ARCH_NONE);
    CHECK(arch_kind_of("a.gz") == ARCH_NONE);       // bare gzip is not a listing
    CHECK(arch_is_archive("a.tgz"));
    CHECK(!arch_is_archive("a.tgz.bak"));
    CHECK(arch_kind_of(nullptr) == ARCH_NONE);
    // rar has no writer
    CHECK(!arch_can_pack(ARCH_RAR));
    CHECK(arch_can_extract(ARCH_RAR));
}

static void test_list_zip(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/t.zip").c_str(), &v, &err));
    const ArchEntry* f = find(v, "a file.txt");
    CHECK(f != nullptr);
    if (f) {
        CHECK(f->size == 6);
        CHECK(S_ISREG(f->mode));
        CHECK(!strcmp(f->raw, "a file.txt"));
    }
    const ArchEntry* b = find(v, "sub/b.log");
    CHECK(b != nullptr);
    if (b) CHECK(b->size == 2);
    CHECK(find(v, "sub") != nullptr);
    CHECK(find(v, "sub/deep") != nullptr);
    const ArchEntry* d = find(v, "sub/deep");
    if (d) CHECK(S_ISDIR(d->mode));
    CHECK(find(v, "sub/deep/c.bin") != nullptr);
    CHECK(find(v, "./sub") == nullptr);            // no stray "./" rows
    CHECK(find(v, "sub/") == nullptr);             // dir rows carry no slash
}

// A zip holding only files still has to show its folders.
static void test_list_synthesizes_dirs(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/files.zip").c_str(), &v, &err));
    CHECK(find(v, "a file.txt") != nullptr);
    CHECK(find(v, "sub") != nullptr);
    CHECK(find(v, "sub/deep") != nullptr);
    CHECK(S_ISDIR(find(v, "sub")->mode));
}

static void test_list_tar(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/t.tar.gz").c_str(), &v, &err));
    const ArchEntry* f = find(v, "a file.txt");
    CHECK(f != nullptr);
    if (f) {
        CHECK(f->size == 6);
        // This fixture was created with explicit member names, so tar stores it
        // without a "./" prefix and rel and raw agree.
        CHECK(!strcmp(f->raw, "a file.txt"));
    }
    CHECK(find(v, "sub/b.log") != nullptr);
    CHECK(find(v, "sub/deep/c.bin") != nullptr);
    const ArchEntry* d = find(v, "sub");
    CHECK(d != nullptr);
    if (d) {
        CHECK(S_ISDIR(d->mode));
        CHECK(!strcmp(d->raw, "sub/"));
    }
    CHECK(find(v, "sub/b.log")->mtime != 0);
}

// tar created by packing a directory stores every member under "./". The pane
// must show clean paths while the raw name keeps the prefix, or asking tar for
// that member back would find nothing.
static void test_list_tar_dot_prefix(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/dot.tar.gz").c_str(), &v, &err));
    const ArchEntry* f = find(v, "a file.txt");
    CHECK(f != nullptr);
    if (f) {
        CHECK(!strcmp(f->rel, "a file.txt"));
        CHECK(!strcmp(f->raw, "./a file.txt"));
    }
    const ArchEntry* d = find(v, "sub");
    CHECK(d != nullptr);
    if (d) {
        CHECK(S_ISDIR(d->mode));
        CHECK(!strcmp(d->raw, "./sub/"));
    }
    // And the raw name has to be good enough to extract with.
    std::string out = root + "/outdot";
    mkdir(out.c_str(), 0755);
    std::vector<std::string> raw;
    for (const ArchEntry& e : v)
        if (!strcmp(e.rel, "sub/deep/c.bin")) raw.push_back(e.raw);
    CHECK(raw.size() == 1);
    Prog pr;
    CHECK(arch_extract_members((root + "/dot.tar.gz").c_str(), out.c_str(), raw, prog_cb, &pr, &err));
    CHECK(exists(out + "/sub/deep/c.bin"));
    sh("rm -rf '" + out + "'");
}

static void test_list_7z(const std::string& root) {
    if (!arch_tool_available(ARCH_7Z)) { printf("skip: 7z missing\n"); return; }
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/t.7z").c_str(), &v, &err));
    const ArchEntry* f = find(v, "a file.txt");
    CHECK(f != nullptr);
    if (f) CHECK(f->size == 6);
    CHECK(find(v, "sub/b.log") != nullptr);
    CHECK(find(v, "sub/deep") != nullptr);
    // The archive's own header block must not become a row.
    CHECK(find(v, "t.7z") == nullptr);
}

static void test_extract_whole(const std::string& root, const char* arch) {
    std::string out = root + "/out";
    mkdir(out.c_str(), 0755);
    Prog pr;
    std::string err;
    bool ok = arch_extract((root + "/" + arch).c_str(), out.c_str(), prog_cb, &pr, &err);
    if (!ok) printf("  extract %s -> %s\n", arch, err.c_str());
    CHECK(ok);
    CHECK(exists(out + "/a file.txt"));
    CHECK(exists(out + "/sub/b.log"));
    CHECK(exists(out + "/sub/deep/c.bin"));
    CHECK(filesize(out + "/a file.txt") == 6);
    CHECK(slurp(out + "/a file.txt") == "hello\n");
    CHECK(pr.monotonic);
    CHECK(pr.calls > 0);
    CHECK(pr.total_files >= 3);
    CHECK(pr.total_bytes >= 10);
    sh("rm -rf '" + out + "'");
}

// The whole point of passing members as argv: a name with a space, a quote and
// a glob character must come out the other end unchanged.
static void test_extract_members_with_tricky_names(const std::string& root) {
    std::string out = root + "/out2";
    mkdir(out.c_str(), 0755);
    std::vector<std::string> members;
    for (const char* m : { "sub/b.log", "we'ird*name?.txt" }) members.push_back(m);
    Prog pr;
    std::string err;
    CHECK(arch_extract_members((root + "/t.zip").c_str(), out.c_str(), members, prog_cb, &pr, &err));
    CHECK(exists(out + "/sub/b.log"));
    CHECK(exists(out + "/we'ird*name?.txt"));
    CHECK(filesize(out + "/we'ird*name?.txt") == 3);
    // Only the requested members came out.
    CHECK(!exists(out + "/a file.txt"));
    sh("rm -rf '" + out + "'");
}

static void test_extract_tar_members(const std::string& root) {
    std::string out = root + "/out3";
    mkdir(out.c_str(), 0755);
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/t.tar.gz").c_str(), &v, &err));
    std::vector<std::string> raw;
    for (const ArchEntry& e : v)
        if (!strcmp(e.rel, "sub/b.log") || !strcmp(e.rel, "a file.txt")) raw.push_back(e.raw);
    CHECK(raw.size() == 2);
    Prog pr;
    CHECK(arch_extract_members((root + "/t.tar.gz").c_str(), out.c_str(), raw, prog_cb, &pr, &err));
    CHECK(exists(out + "/sub/b.log"));
    CHECK(exists(out + "/a file.txt"));
    CHECK(!exists(out + "/sub/deep/c.bin"));
    sh("rm -rf '" + out + "'");
}

static void test_pack(const std::string& root, const char* ext) {
    std::string arch = root + "/pack" + ext;
    sh("rm -f '" + arch + "'");
    std::vector<std::string> sources{ "a file.txt", "sub" };
    Prog pr;
    std::string err;
    bool ok = arch_pack(arch.c_str(), root.c_str(), sources, prog_cb, &pr, &err);
    if (!ok) printf("  pack %s -> %s\n", ext, err.c_str());
    CHECK(ok);
    CHECK(exists(arch));
    CHECK(pr.total_files >= 4);
    CHECK(pr.total_bytes >= 10);

    // Read it back with the same backend: the round trip is the real test.
    std::vector<ArchEntry> v;
    std::string lerr;
    CHECK(arch_list(arch.c_str(), &v, &lerr));
    CHECK(find(v, "a file.txt") != nullptr);
    CHECK(find(v, "sub/b.log") != nullptr);
    CHECK(find(v, "sub/deep/c.bin") != nullptr);
    sh("rm -f '" + arch + "'");
}

static void test_test_integrity(const std::string& root) {
    Prog pr;
    std::string err;
    CHECK(arch_test((root + "/t.zip").c_str(), prog_cb, &pr, &err));
    CHECK(pr.total_files >= 3);

    // A truncated zip has to fail rather than report success.
    std::string bad = root + "/bad.zip";
    sh("cp '" + root + "/t.zip' '" + bad + "'");
    sh("truncate -s 40 '" + bad + "'");
    Prog pr2;
    std::string err2;
    CHECK(!arch_test(bad.c_str(), prog_cb, &pr2, &err2));
    CHECK(!err2.empty());
    sh("rm -f '" + bad + "'");
}

static void test_abort(const std::string& root) {
    std::string out = root + "/out4";
    mkdir(out.c_str(), 0755);
    Prog pr;
    pr.stop_after = 1;                 // the first callback says stop
    std::string err;
    bool ok = arch_extract((root + "/t.zip").c_str(), out.c_str(), prog_cb, &pr, &err);
    CHECK(!ok);
    CHECK(err == "aborted");
    sh("rm -rf '" + out + "'");
}

static void test_failures(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(!arch_list((root + "/nope.zip").c_str(), &v, &err));
    CHECK(!err.empty());
    CHECK(!arch_list((root + "/a file.txt").c_str(), &v, &err));   // not an archive
    CHECK(err == "not an archive");

    // A file with a zip name but no zip inside it must not look empty.
    std::string fake = root + "/fake.zip";
    FILE* f = fopen(fake.c_str(), "wb");
    if (f) { fputs("not a zip at all\n", f); fclose(f); }
    Prog pr;
    std::string err2;
    CHECK(!arch_extract(fake.c_str(), root.c_str(), prog_cb, &pr, &err2));
    CHECK(!err2.empty());
    sh("rm -f '" + fake + "'");
}

static void test_extract_temp(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string lerr;
    CHECK(arch_list((root + "/t.zip").c_str(), &v, &lerr));
    const ArchEntry* e = find(v, "sub/b.log");
    CHECK(e != nullptr);
    if (!e) return;
    char path[4096];
    std::string err;
    CHECK(arch_extract_temp((root + "/t.zip").c_str(), e->raw, path, sizeof(path), &err));
    CHECK(slurp(path) == "x\n");
    unlink(path);
    std::string dir = std::string(path).substr(0, std::string(path).find_last_of('/'));
    sh("rm -rf '" + dir + "'");
}

int main() {
    char tmpl[] = "/tmp/mc-clone-arch-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("cannot create temp dir\n"); return 1; }
    std::string root(dir);

    // Fixture tree. The names are chosen to be hostile to a shell-out: a space,
    // an apostrophe and glob characters. They are written directly, not through
    // the shell, so the fixture itself cannot be the thing that breaks.
    sh("mkdir -p '" + root + "/sub/deep'");
    write_file(root + "/a file.txt", "hello\n");
    write_file(root + "/sub/b.log", "x\n");
    write_file(root + "/sub/deep/c.bin", "yy\n");
    write_file(root + "/we'ird*name?.txt", "ab\n");

    sh("cd " + q(root) + " && zip -qr t.zip " + q("a file.txt") + " " + q("sub") + " " + q("we'ird*name?.txt"));
    sh("cd " + q(root) + " && zip -q files.zip " + q("a file.txt") + " " + q("sub/b.log") + " " + q("sub/deep/c.bin"));
    sh("cd " + q(root) + " && tar czf t.tar.gz " + q("a file.txt") + " sub");
    sh("cd " + q(root) + " && tar cf t.tar " + q("a file.txt") + " sub");
    sh("cd " + q(root) + " && 7z a -t7z -y t.7z " + q("a file.txt") + " sub >/dev/null");
    // tar by packing a directory rather than naming members, which is what
    // stores every name under "./". Kept in its own subtree so the archive does
    // not try to include itself.
    sh("mkdir -p " + q(root + "/dotsrc") + " && cp -r " + q(root + "/a file.txt") + " " + q(root + "/sub") + " " + q(root + "/dotsrc/") + " && tar czf " + q(root + "/dot.tar.gz") + " -C " + q(root + "/dotsrc") + " .");

    test_kind_of();
    test_list_zip(root);
    test_list_synthesizes_dirs(root);
    test_list_tar(root);
    test_list_tar_dot_prefix(root);
    test_list_7z(root);
    test_extract_whole(root, "t.zip");
    test_extract_whole(root, "t.tar.gz");
    test_extract_whole(root, "t.tar");
    if (arch_tool_available(ARCH_7Z)) test_extract_whole(root, "t.7z");
    test_extract_members_with_tricky_names(root);
    test_extract_tar_members(root);
    test_pack(root, ".zip");
    test_pack(root, ".tar.gz");
    if (arch_tool_available(ARCH_7Z)) test_pack(root, ".7z");
    test_test_integrity(root);
    test_abort(root);
    test_failures(root);
    test_extract_temp(root);

    sh("rm -rf '" + root + "'");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
