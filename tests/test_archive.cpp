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
static off_t bytes_of(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 ? st.st_size : -1;
}

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
    // A split archive names its parts after the whole, so the part number is not
    // the format. Without this the pack dialog's own volumes would be unreadable.
    CHECK(arch_kind_of("a.7z.001") == ARCH_7Z);
    CHECK(arch_kind_of("a.tar.gz.002") == ARCH_TAR_GZ);
    CHECK(arch_kind_of("movie.mkv.001") == ARCH_NONE);
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

// A folder row in the archive pane is one row standing for a whole subtree, so
// extracting it has to bring the subtree with it. unzip matches a member name
// literally and would unpack an empty directory; tar and 7z recurse on their own.
static void test_extract_directory_member(const std::string& root, const char* arch) {
    std::string out = root + "/out_dir";
    sh("rm -rf '" + out + "'");
    mkdir(out.c_str(), 0755);
    std::vector<ArchEntry> v;
    std::string err;
    CHECK(arch_list((root + "/" + arch).c_str(), &v, &err));
    std::vector<std::string> raw;
    for (const ArchEntry& e : v)
        if (!strcmp(e.rel, "sub")) raw.push_back(e.raw);
    CHECK(raw.size() == 1);
    Prog pr;
    CHECK(arch_extract_members((root + "/" + arch).c_str(), out.c_str(), raw, prog_cb, &pr, &err));
    CHECK(exists(out + "/sub/b.log"));
    CHECK(exists(out + "/sub/deep/c.bin"));
    CHECK(!exists(out + "/a file.txt"));       // still only what was asked for
    sh("rm -rf '" + out + "'");
}

// A folder that holds nothing has no file to name, so the directory member
// itself is what gets handed to the tool, and it still has to come out as a
// directory rather than nothing at all.
static void test_extract_empty_directory(const std::string& root) {
    std::string src = root + "/emptysrc";
    sh("rm -rf '" + src + "'");
    sh("mkdir -p '" + src + "/blank'");
    std::string err;
    std::vector<std::string> sources{ "blank" };
    CHECK(arch_pack((root + "/empty.zip").c_str(), src.c_str(), sources, nullptr, nullptr, &err));

    std::vector<ArchEntry> v;
    CHECK(arch_list((root + "/empty.zip").c_str(), &v, &err));
    const ArchEntry* blank = find(v, "blank");
    CHECK(blank && S_ISDIR(blank->mode));
    std::vector<std::string> raw;
    if (blank) raw.push_back(blank->raw);
    std::string out = root + "/out_empty";
    sh("rm -rf '" + out + "'");
    mkdir(out.c_str(), 0755);
    Prog pr;
    CHECK(arch_extract_members((root + "/empty.zip").c_str(), out.c_str(), raw, prog_cb, &pr, &err));
    CHECK(is_dir(out + "/blank"));
    sh("rm -rf '" + out + "' '" + src + "'");
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

static void test_split_virtual(const std::string& root) {
    char arch[4096], prefix[1024];
    // The archive component is found wherever it sits in the path.
    CHECK(arch_split_virtual((root + "/t.zip").c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
    CHECK(!strcmp(arch, (root + "/t.zip").c_str()));
    CHECK(prefix[0] == '\0');
    CHECK(arch_split_virtual((root + "/t.zip/sub/deep").c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
    CHECK(!strcmp(arch, (root + "/t.zip").c_str()));
    CHECK(!strcmp(prefix, "sub/deep"));
    CHECK(arch_split_virtual((root + "/t.tar.gz/sub/").c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
    CHECK(!strcmp(prefix, "sub"));
    // Ordinary directories, and a directory that merely looks like an archive.
    CHECK(!arch_split_virtual(root.c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
    CHECK(!arch_split_virtual((root + "/sub").c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
    CHECK(!arch_split_virtual("", arch, sizeof(arch), prefix, sizeof(prefix)));
    sh("mkdir -p '" + root + "/looks.zip'");
    CHECK(!arch_split_virtual((root + "/looks.zip").c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
    sh("rm -rf '" + root + "/looks.zip'");
    // A non-archive file in the middle of the path is not a boundary either.
    CHECK(!arch_split_virtual((root + "/a file.txt/x").c_str(), arch, sizeof(arch), prefix, sizeof(prefix)));
}

// The options a pack dialog can ask for: a level, a password, volumes. Each one
// has to reach the tool, and a format that cannot honour one has to say so
// before it writes an archive that is not what was asked for.
static void test_pack_options(const std::string& root) {
    std::string src = root + "/opts";
    sh("rm -rf " + q(src) + " && mkdir -p " + q(src));
    std::string filler;
    for (int i = 0; i < 20000; i++) filler += "abcdefgh";
    write_file(src + "/f0.txt", filler);
    write_file(src + "/f1.txt", filler);
    const std::vector<std::string> one{ "f0.txt" };
    const std::vector<std::string> two{ "f0.txt", "f1.txt" };
    std::string err, why;

    // A level reaches the compressor: level 0 stores, so the archive is bigger
    // than the same content at level 9.
    ArchPackOpts store = arch_pack_opts_default(ARCH_ZIP);
    store.level = 0;
    CHECK(arch_pack_opts((src + "/s.zip").c_str(), src.c_str(), two, store, nullptr, nullptr, &err));
    ArchPackOpts best = arch_pack_opts_default(ARCH_ZIP);
    best.level = 9;
    CHECK(arch_pack_opts((src + "/b.zip").c_str(), src.c_str(), two, best, nullptr, nullptr, &err));
    CHECK(bytes_of(src + "/s.zip") > bytes_of(src + "/b.zip"));

    // A password really encrypts: the tool cannot read the archive back without it.
    ArchPackOpts pw = arch_pack_opts_default(ARCH_ZIP);
    pw.password = "hunter2";
    CHECK(arch_pack_opts((src + "/p.zip").c_str(), src.c_str(), one, pw, nullptr, nullptr, &err));
    sh("mkdir -p " + q(src + "/out"));
    CHECK(!arch_extract((src + "/p.zip").c_str(), (src + "/out").c_str(), nullptr, nullptr, &err));
    CHECK(!exists(src + "/out/f0.txt"));
    sh("cd " + q(src) + " && unzip -q -o -P hunter2 p.zip -d out > /dev/null 2>&1");
    CHECK(exists(src + "/out/f0.txt"));

    // Refusals happen before anything is written.
    ArchPackOpts bad = arch_pack_opts_default(ARCH_TAR_GZ);
    bad.password = "hunter2";
    CHECK(!arch_pack_supported(bad, &why));
    CHECK(why.find("encrypt") != std::string::npos);
    CHECK(!arch_pack_opts((src + "/bad.tar.gz").c_str(), src.c_str(), one, bad, nullptr, nullptr, &err));
    CHECK(!exists(src + "/bad.tar.gz"));

    ArchPackOpts vol = arch_pack_opts_default(ARCH_ZIP);
    vol.volume_bytes = 1024 * 1024;
    CHECK(!arch_pack_supported(vol, &why));
    CHECK(why.find("volumes") != std::string::npos);

    ArchPackOpts lvl = arch_pack_opts_default(ARCH_TAR);
    lvl.level = 9;
    CHECK(!arch_pack_supported(lvl, &why));

    // 7z volumes read back as one archive, by name and by extraction. The
    // content has to be incompressible or the set never reaches a second volume.
    std::string noise;
    unsigned x = 12345u;
    for (int i = 0; i < 200000; i++) { x = x * 1103515245u + 12345u; noise.push_back((char)((x >> 16) & 0xFF)); }
    write_file(src + "/n0.bin", noise);
    const std::vector<std::string> noisy{ "n0.bin" };
    ArchPackOpts v7 = arch_pack_opts_default(ARCH_7Z);
    v7.volume_bytes = 100 * 1024;
    CHECK(arch_pack_supported(v7, &why));
    CHECK(arch_pack_opts((src + "/v.7z").c_str(), src.c_str(), noisy, v7, nullptr, nullptr, &err));
    CHECK(exists(src + "/v.7z.001"));
    CHECK(exists(src + "/v.7z.002"));
    std::vector<ArchEntry> members;
    CHECK(arch_list((src + "/v.7z.001").c_str(), &members, &err));
    CHECK(arch_find(members, "n0.bin") != nullptr);
    sh("mkdir -p " + q(src + "/vout"));
    CHECK(arch_extract((src + "/v.7z.001").c_str(), (src + "/vout").c_str(), nullptr, nullptr, &err));
    CHECK(bytes_of(src + "/vout/n0.bin") == (off_t)noise.size());

    // The name has to carry the format being written.
    ArchPackOpts zipped = arch_pack_opts_default(ARCH_7Z);
    CHECK(!arch_pack_opts((src + "/wrong.zip").c_str(), src.c_str(), one, zipped, nullptr, nullptr, &err));
    CHECK(!exists(src + "/wrong.zip"));

    sh("rm -rf " + q(src));
}

static void test_extract_temp(const std::string& root) {
    std::vector<ArchEntry> v;
    std::string lerr;
    CHECK(arch_list((root + "/t.zip").c_str(), &v, &lerr));
    const ArchEntry* e = find(v, "sub/b.log");
    CHECK(e != nullptr);
    if (!e) return;
    char path[4096], dir[4096];
    std::string err;
    CHECK(arch_extract_temp((root + "/t.zip").c_str(), e->raw, path, sizeof(path),
                            dir, sizeof(dir), &err));
    CHECK(slurp(path) == "x\n");
    CHECK(is_dir(dir));
    sh("rm -rf '" + std::string(dir) + "'");
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
    test_extract_directory_member(root, "t.zip");
    test_extract_directory_member(root, "t.tar.gz");
    test_extract_directory_member(root, "t.tar");
    test_extract_directory_member(root, "t.7z");
    test_extract_empty_directory(root);
    test_pack(root, ".zip");
    test_pack(root, ".tar.gz");
    if (arch_tool_available(ARCH_7Z)) test_pack(root, ".7z");
    test_test_integrity(root);
    test_abort(root);
    test_failures(root);
    test_extract_temp(root);
    test_pack_options(root);
    test_split_virtual(root);

    sh("rm -rf '" + root + "'");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
