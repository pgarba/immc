// Tests for the remote backend.
//
// The pure half — the URL grammar, the sites file, the listing parsers — needs
// nothing but the module, and it runs everywhere. The transport half needs a
// server, so tests/ftp_server.py is started on a port the kernel picked, over a
// root this file populates and destroys. A build without libcurl, or a machine
// without python3, skips the transport half instead of failing it: the thing
// under test is not present, and a red test about a missing dependency teaches
// nobody anything.

#include "../src/remote.h"
#include "../src/xfer.h"
#include "test_os.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <atomic>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include "proc.h"
#else
#include <poll.h>
#include <sys/wait.h>
#endif
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef MC_FTP_SERVER
#define MC_FTP_SERVER "tests/ftp_server.py"
#endif

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

static const char* kUser = "tester";
static const char* kPass = "hunter2";

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
static bool is_dir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static long long file_size(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}

static void write_file(const std::string& path, const std::string& body) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(body.data(), 1, body.size(), f); fclose(f); }
}

static std::string read_file(const std::string& path, bool* ok = nullptr) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { if (ok) *ok = false; return ""; }
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    if (ok) *ok = true;
    return out;
}

static int mode_of(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (int)(st.st_mode & 07777) : -1;
}

static void sh(const std::string& cmd) { int rc = test_system(cmd); (void)rc; }

static std::string q(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

// The name a listing row carries, or "" when no row has it.
static const FileEntry* find_row(const std::vector<FileEntry>& rows, const char* name) {
    for (const FileEntry& e : rows)
        if (!strcmp(e.name, name)) return &e;
    return nullptr;
}

// ---------------------------------------------------------------------------
// protocol vocabulary
// ---------------------------------------------------------------------------

static void test_proto_vocabulary(void) {
    CHECK(net_proto_port(NET_FTP) == 21);
    CHECK(net_proto_port(NET_FTPS) == 21);
    CHECK(net_proto_port(NET_FTPS_IMPLICIT) == 990);
    CHECK(net_proto_port(NET_SFTP) == 22);

    CHECK(!strcmp(net_proto_key(NET_FTPS_IMPLICIT), "ftps-implicit"));

    // The key written in the sites file is the key read back, so a hand-edited
    // file says what the dialog shows.
    for (int p = 0; p < NET_PROTO_COUNT; p++) {
        NetProto back = NET_FTP;
        CHECK(net_proto_of(net_proto_key((NetProto)p), &back));
        CHECK(back == (NetProto)p);
    }
    NetProto junk = NET_FTP;
    CHECK(!net_proto_of("gopher", &junk));

    NetSite s;
    s.host = "example.com";
    s.proto = NET_SFTP;
    CHECK(net_site_port(s) == 22);
    s.port = 2222;
    CHECK(net_site_port(s) == 2222);
}

// ---------------------------------------------------------------------------
// what a user types in the quick-connect field
// ---------------------------------------------------------------------------

static void test_parse_url(void) {
    NetSite s;
    std::string err;

    CHECK(net_parse_url("example.com", &s, &err));
    CHECK(s.host == "example.com" && s.proto == NET_FTP && s.user == "anonymous");

    CHECK(net_parse_url("bob@example.com", &s, &err));
    CHECK(s.host == "example.com" && s.user == "bob");

    CHECK(net_parse_url("bob@example.com:2121/pub/incoming", &s, &err));
    CHECK(s.host == "example.com" && s.port == 2121 && s.path == "/pub/incoming");

    CHECK(net_parse_url("ftp://files.example.org/pub", &s, &err));
    CHECK(s.proto == NET_FTP && s.host == "files.example.org" && s.path == "/pub");

    CHECK(net_parse_url("ftps://files.example.org", &s, &err));
    CHECK(s.proto == NET_FTPS && s.port == 21);

    // Port 990 is the implicit-FTPS convention, and a URL that names it means
    // TLS from the first byte rather than AUTH TLS.
    CHECK(net_parse_url("ftps://files.example.org:990", &s, &err));
    CHECK(s.proto == NET_FTPS_IMPLICIT && s.port == 990);

    CHECK(net_parse_url("sftp://box.example/~/upload", &s, &err));
    CHECK(s.proto == NET_SFTP && s.port == 22 && s.path == "/~/upload");
    CHECK(net_parse_url("ssh://box.example", &s, &err));
    CHECK(s.proto == NET_SFTP);

    // A password typed into the field is used but never kept unless the caller
    // says so, and a percent sign in it survives the URL grammar.
    CHECK(net_parse_url("bob:pass%2Fword@example.com", &s, &err));
    CHECK(s.password == "pass/word" && !s.remember);

    // IPv6 needs brackets, because a colon is already the port separator.
    CHECK(net_parse_url("ftp://[::1]:2121/pub", &s, &err));
    CHECK(s.host == "::1" && s.port == 2121 && s.path == "/pub");

    CHECK(!net_parse_url("", &s, &err));
    CHECK(!net_parse_url("   ", &s, &err));
    CHECK(!net_parse_url("ftp://", &s, &err));
    CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// the virtual path a pane holds
// ---------------------------------------------------------------------------

static void test_virtual_paths(void) {
    NetSite s;
    s.proto = NET_FTP;
    s.host = "files.example.org";
    s.user = "bob";
    s.password = "secret";

    // The root is what a tab title, a history entry and the path bar show, so a
    // password must never be part of it.
    std::string root = net_site_root(s);
    CHECK(root == "ftp://bob@files.example.org");
    CHECK(root.find("secret") == std::string::npos);

    CHECK(net_virtual_path(root, "") == root + "/");
    CHECK(net_virtual_path(root, "/") == root + "/");
    CHECK(net_virtual_path(root, "/pub") == root + "/pub");
    CHECK(net_virtual_path(root, "pub") == root + "/pub");

    CHECK(net_is_remote(root.c_str()));
    CHECK(net_is_remote((root + "/pub/in").c_str()));
    CHECK(!net_is_remote("/home/adam/github"));
    CHECK(!net_is_remote(""));

    std::string rroot, rpath;
    CHECK(net_split_virtual((root + "/pub/incoming").c_str(), &rroot, &rpath));
    CHECK(rroot == root);
    CHECK(rpath == "/pub/incoming");
    CHECK(net_split_virtual((root + "/").c_str(), &rroot, &rpath));
    CHECK(rpath.empty() || rpath == "/");
    // The root a split produces has no password in it, so it is a lookup key
    // rather than a credential.
    CHECK(rroot.find("secret") == std::string::npos);
    CHECK(!net_split_virtual("/tmp", &rroot, &rpath));

    CHECK(net_path_parent("/pub/incoming") == "/pub");
    CHECK(net_path_parent("/pub") == "");
    CHECK(net_path_parent("/") == "");
    CHECK(net_path_parent("a/b") == "a");

    // Both spellings of the root have to be recognised, because a typed URL keeps
    // "/" and a walked-up path produces "". A pane that only tested one of them
    // would press ".." in /pub, land on the root, and then navigate to the root
    // again forever instead of leaving the site.
    CHECK(net_path_is_root(""));
    CHECK(net_path_is_root("/"));
    CHECK(!net_path_is_root("/pub"));
    CHECK(!net_path_is_root("/pub/incoming"));
    CHECK(net_path_is_root(net_path_parent("/pub")));

    // Joining a directory and a name. The naive version makes "//name" out of the
    // root, and a server that honours the empty component then looks for a
    // directory with no name in it.
    CHECK(net_join_remote("/pub", "a.txt") == "/pub/a.txt");
    CHECK(net_join_remote("/", "a.txt") == "/a.txt");
    CHECK(net_join_remote("", "a.txt") == "/a.txt");
    CHECK(net_join_remote("/pub/", "a.txt") == "/pub/a.txt");
    CHECK(net_join_remote("/sub", "a file.txt") == "/sub/a file.txt");
    CHECK(net_join_remote("/pub", "").find("//") == std::string::npos);
}

static void test_unescape(void) {
    CHECK(net_url_unescape("plain") == "plain");
    CHECK(net_url_unescape("a%20b") == "a b");
    CHECK(net_url_unescape("100%25") == "100%");
    // A percent that is not an escape is left alone rather than dropped.
    CHECK(net_url_unescape("100%") == "100%");
    CHECK(net_url_unescape("%zz") == "%zz");
}

// ---------------------------------------------------------------------------
// listing parsers
// ---------------------------------------------------------------------------

static void test_escape(void) {
    CHECK(net_url_escape("plain", true) == "plain");
    // The one that made a whole directory unreachable: curl refuses a URL with a
    // space in it before it has opened a socket, so the pane was left showing an
    // empty listing for a directory that had contents.
    CHECK(net_url_escape("IDA Pro 9.6.261001", true) == "IDA%20Pro%209.6.261001");
    // '/' separates the components of a path and stays; inside one name it is a
    // byte like any other.
    CHECK(net_url_escape("/sub/a file.txt", true) == "/sub/a%20file.txt");
    CHECK(net_url_escape("a/b", false) == "a%2Fb");
    // The characters a URL would read as something other than a name: a fragment,
    // a query, and the escape character itself.
    CHECK(net_url_escape("a#b", true) == "a%23b");
    CHECK(net_url_escape("q?x", true) == "q%3Fx");
    CHECK(net_url_escape("100%done", true) == "100%25done");
    // RFC 3986's unreserved set stays plain, so an ordinary name is untouched.
    CHECK(net_url_escape("a-b.c_d~e", true) == "a-b.c_d~e");
    // A name is bytes, and bytes need encoding.
    CHECK(net_url_escape("\xc3\xbcnicode", true) == "%C3%BCnicode");
    // The round trip is what the transport depends on: the pane holds the name as
    // the server spells it, curl decodes the URL, and the server is asked for the
    // same bytes the listing gave.
    CHECK(net_url_unescape(net_url_escape("IDA Pro 9.6.261001", true)) == "IDA Pro 9.6.261001");
    CHECK(net_url_unescape(net_url_escape("100%done", true)) == "100%done");
    CHECK(net_url_unescape(net_url_escape("a#b", true)) == "a#b");
}

static void test_mlsd_parser(void) {
    FileEntry e{};

    CHECK(net_parse_mlsd("type=file;size=1234;modify=20230131120000;unix.mode=0644;report.pdf", &e));
    CHECK(!strcmp(e.name, "report.pdf"));
    CHECK(e.size == 1234);
    CHECK(S_ISREG(e.mode));
    CHECK((e.mode & 0777) == 0644);
    CHECK(e.mtime != 0);

    CHECK(net_parse_mlsd("type=dir;modify=20230131120000;unix.mode=0755;incoming", &e));
    CHECK(!strcmp(e.name, "incoming") && S_ISDIR(e.mode));

    // The pane adds its own ".." row, and the server's idea of the current
    // directory would be a duplicate of what is already on screen.
    FileEntry dot{};
    CHECK(!net_parse_mlsd("type=cdir;/", &dot));
    CHECK(!net_parse_mlsd("type=pdir;/..", &dot));

    // Facts arrive in any order and in any case; the name is whatever follows
    // the first fact-less ';', so a name containing a ';' survives.
    CHECK(net_parse_mlsd("UNIX.MODE=600;Modify=20230131120000;Size=9;TYPE=File;a;b.txt", &e));
    CHECK(!strcmp(e.name, "a;b.txt"));
    CHECK(e.size == 9);
    CHECK((e.mode & 0777) == 0600);

    CHECK(net_parse_mlsd("type=file;size=1;name with spaces", &e));
    CHECK(!strcmp(e.name, "name with spaces"));

    // RFC 3659 writes an entry as "facts SP entry", and real servers put that
    // space there. A parser that takes the whole tail after the last ';' gives
    // every name on such a server a leading space, which then shows up in the path
    // the moment the row is opened or fetched. The fixture used to omit the space,
    // so the tests passed against a server nobody actually runs.
    CHECK(net_parse_mlsd("type=file;size=12;modify=20230131120000;unix.mode=0640; hello.txt", &e));
    CHECK(!strcmp(e.name, "hello.txt"));
    CHECK(e.size == 12);
    CHECK(net_parse_mlsd("type=dir;unix.mode=0755; sub dir", &e));
    CHECK(!strcmp(e.name, "sub dir") && S_ISDIR(e.mode));
    // Exactly one space is the separator, so a name that really does start with a
    // space arrives as two and keeps one.
    CHECK(net_parse_mlsd("type=file;size=1; leading.txt", &e));
    CHECK(!strcmp(e.name, "leading.txt"));
    CHECK(net_parse_mlsd("type=file;size=1;  two spaces.txt", &e));
    CHECK(!strcmp(e.name, " two spaces.txt"));

    // A name is bytes, and bytes can contain the '=' that separates a fact's key
    // from its value. Reading every segment with an '=' as a fact swallowed the
    // name and dropped the row, so the file was simply not on screen.
    CHECK(net_parse_mlsd("type=file;size=3;modify=20230131120000; a=b", &e));
    CHECK(!strcmp(e.name, "a=b"));
    CHECK(e.size == 3);
    CHECK(net_parse_mlsd("type=file;size=1; type=conf.txt", &e));
    CHECK(!strcmp(e.name, "type=conf.txt"));

    CHECK(!net_parse_mlsd("", &e));
    CHECK(!net_parse_mlsd("just some banner text", &e));
    CHECK(!net_parse_mlsd("size=12;no type fact", &e));

    // MLSD's modify fact is UTC and minute-precision is enough to check.
    time_t t = net_parse_mlsd_time("20230131120000");
    struct tm utc{};
    time_t probe = t;
    gmtime_r(&probe, &utc);
    CHECK(utc.tm_year == 123 && utc.tm_mon == 0 && utc.tm_mday == 31);
    CHECK(utc.tm_hour == 12 && utc.tm_min == 0);
}

static void test_list_parser(void) {
    FileEntry e{};

    // The unix dialect: eight fields counted from the front, the rest is name.
    CHECK(net_parse_list_line("-rw-r--r-- 1 bob staff 1234 Jan 31 12:00 report.pdf", &e));
    CHECK(!strcmp(e.name, "report.pdf"));
    CHECK(e.size == 1234);
    CHECK(S_ISREG(e.mode));
    CHECK((e.mode & 0777) == 0644);
    CHECK(e.mtime != 0);

    CHECK(net_parse_list_line("drwxr-xr-x 2 bob staff 4096 Jan 31  2022 incoming", &e));
    CHECK(S_ISDIR(e.mode) && !strcmp(e.name, "incoming"));

    // A name with spaces is one name, because the fields are counted from the
    // front rather than split from the back.
    CHECK(net_parse_list_line("-rw-r--r-- 1 bob staff 12 Jan 31 12:00 the long name.txt", &e));
    CHECK(!strcmp(e.name, "the long name.txt"));

    // A year rather than a clock time is the usual way to show something older
    // than six months.
    CHECK(net_parse_list_line("-rw-r--r-- 1 bob staff 12 Jan 31  1999 old.txt", &e));
    struct tm utc{};
    gmtime_r(&e.mtime, &utc);
    CHECK(utc.tm_year + 1900 == 1999);

    CHECK(net_parse_list_line("lrwxrwxrwx 1 bob staff 7 Jan 31 12:00 link", &e));
    CHECK(S_ISLNK(e.mode));

    // The DOS dialect some servers and some SFTP backends produce.
    CHECK(net_parse_list_line("01-31-23  12:00PM    1234 report.pdf", &e));
    CHECK(!strcmp(e.name, "report.pdf") && e.size == 1234 && S_ISREG(e.mode));
    CHECK(net_parse_list_line("01-31-23  12:00PM     <DIR>          incoming", &e));
    CHECK(S_ISDIR(e.mode) && !strcmp(e.name, "incoming"));

    CHECK(!net_parse_list_line("", &e));
    CHECK(!net_parse_list_line("total 8", &e));
    CHECK(!net_parse_list_line("Welcome to the server", &e));
}

static void test_mode_from_rwx(void) {
    CHECK((net_mode_from_rwx("-rw-r--r--") & 0777) == 0644);
    CHECK((net_mode_from_rwx("drwxr-xr-x") & 0777) == 0755);
    CHECK((net_mode_from_rwx("-rw-------") & 0777) == 0600);
    CHECK((net_mode_from_rwx("drwxrwxrwt") & 0777) == 0777);
    CHECK(S_ISDIR(net_mode_from_rwx("drwxr-xr-x")));
    CHECK(S_ISREG(net_mode_from_rwx("-rw-r--r--")));
    CHECK(S_ISLNK(net_mode_from_rwx("lrwxrwxrwx")));
    CHECK((net_mode_from_rwx("-rwsr-xr-x") & S_ISUID) != 0);
    CHECK((net_mode_from_rwx("drwxrwsr-t") & (S_ISGID | S_ISVTX)) == (S_ISGID | S_ISVTX));
    CHECK(net_mode_from_rwx("garbage") == 0);
}

// ---------------------------------------------------------------------------
// the sites file
// ---------------------------------------------------------------------------

static void test_sites_file(void) {
    // The file holds passwords, so it is pointed at a temporary HOME instead of
    // the real one, and what matters is checked there: the mode it is created
    // with, and whether a password is written at all.
    const std::string dir = test_mkdtemp("mc-remote-sites");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    auto test_home = std::make_unique<TestHome>(dir);

    std::vector<NetSite> sites;
    NetSite a;
    a.name = "Files";
    a.proto = NET_FTPS;
    a.host = "files.example.org";
    a.user = "bob";
    a.password = "hunter2";
    a.remember = true;
    a.path = "/pub";
    sites.push_back(a);

    NetSite b;
    b.name = "Archive";
    b.proto = NET_SFTP;
    b.host = "archive.example.org";
    b.port = 2222;
    b.user = "carol";
    b.password = "notsaved";
    b.remember = false;
    b.threads = 5;
    sites.push_back(b);

    NetSite c;
    c.name = "Legacy";
    c.proto = NET_FTPS_IMPLICIT;
    c.host = "old.example.org";
    c.verify_tls = false;
    c.passive = false;
    c.rar_order = true;
    sites.push_back(c);

    std::string err;
    CHECK(net_sites_save(sites, &err));

    std::string path = net_sites_path();
    CHECK(exists(path));
#ifndef _WIN32
    // The main config is world-readable; this file must not be.
    CHECK((mode_of(path) & 0777) == 0600);
#endif

    std::string body = read_file(path);
    CHECK(body.find("hunter2") != std::string::npos);
    CHECK(body.find("notsaved") == std::string::npos);
    CHECK(body.find("ftps-implicit") != std::string::npos);

    std::vector<NetSite> back;
    CHECK(net_sites_load(&back, &err));
    CHECK(back.size() == 3);
    if (back.size() == 3) {
        CHECK(back[0].name == "Files" && back[0].password == "hunter2" && back[0].remember);
        CHECK(back[0].proto == NET_FTPS && back[0].path == "/pub");
        CHECK(back[1].proto == NET_SFTP && back[1].port == 2222);
        CHECK(back[1].threads == 5);
        CHECK(back[0].threads == 2);          // the default, written and read back
        CHECK(back[1].password.empty());
        CHECK(back[2].proto == NET_FTPS_IMPLICIT);
        CHECK(back[2].verify_tls == false);
        CHECK(back[2].passive == false);
        CHECK(back[2].rar_order);
        CHECK(!back[0].rar_order);            // off unless a site asks for it
    }

    // A lookup by root URL is how a restart puts the password back in a pane.
    const NetSite* found = net_sites_find(back, net_site_root(a));
    CHECK(found && found->password == "hunter2");
    CHECK(net_sites_find(back, "ftp://nobody@example.net") == nullptr);

    // A file from a newer version still opens: unknown keys are skipped, not
    // fatal, so a downgrade does not lose the sites.
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fprintf(f, "[site]\nname = Future\nhost = future.example\nproto = ftp\n"
                   "quantum_mode = 1\nport = 2121\n");
        fclose(f);
    }
    back.clear();
    CHECK(net_sites_load(&back, &err));
    CHECK(back.size() == 1 && back[0].host == "future.example" && back[0].port == 2121);

    test_home.reset();
    sh("rm -rf " + q(dir));
}

// RAR volumes in the order an unpacker reads them. Both namings, both cases, and
// the names that only look like volumes.
static void test_rar_order(void) {
    std::string set;
    int idx = -1;
    CHECK(xfer_rar_volume("Movie.rar", &set, &idx) && set == "movie" && idx == 0);
    CHECK(xfer_rar_volume("movie.r00", &set, &idx) && set == "movie" && idx == 1);
    CHECK(xfer_rar_volume("movie.r99", &set, &idx) && idx == 100);
    CHECK(xfer_rar_volume("MOVIE.S00", &set, &idx) && set == "movie" && idx == 101);
    CHECK(xfer_rar_volume("movie.t05", &set, &idx) && idx == 206);
    CHECK(xfer_rar_volume("show.part1.rar", &set, &idx) && set == "show" && idx == 1);
    CHECK(xfer_rar_volume("show.part010.rar", &set, &idx) && set == "show" && idx == 10);
    CHECK(xfer_rar_volume("a.b.c.rar", &set, &idx) && set == "a.b.c" && idx == 0);
    CHECK(!xfer_rar_volume("movie.sfv", &set, &idx));
    CHECK(!xfer_rar_volume("movie.nfo", &set, &idx));
    CHECK(!xfer_rar_volume("movie.r0", &set, &idx));     // two digits, not one
    CHECK(!xfer_rar_volume("movie.q00", &set, &idx));    // before .r is not a volume
    CHECK(!xfer_rar_volume(".rar", &set, &idx));         // a name, not an extension
    CHECK(!xfer_rar_volume("movie.mp4", &set, &idx));

    // A listing in the server's order: alphabetical, which puts .rar after every
    // .rNN and .s00 before .r10's successors. Two folders, two sets in the first.
    auto item = [](const std::string& remote) {
        XferItem it;
        it.download = true;
        it.remote = remote;
        return it;
    };
    std::vector<XferItem> items = {
        item("/rel/a.r00"), item("/rel/a.r01"), item("/rel/a.rar"), item("/rel/a.s00"),
        item("/rel/a.sfv"), item("/rel/b.part2.rar"), item("/rel/b.part10.rar"),
        item("/rel/b.part1.rar"), item("/rel/a.nfo"),
        item("/rel/Sample/s.r00"), item("/rel/Sample/s.rar"),
    };
    xfer_order_rar(&items);
    std::vector<std::string> got;
    for (const XferItem& it : items) got.push_back(it.remote);
    std::vector<std::string> want = {
        "/rel/a.sfv", "/rel/a.nfo",                                   // the rest first
        "/rel/a.rar", "/rel/a.r00", "/rel/a.r01", "/rel/a.s00",       // set a in order
        "/rel/b.part1.rar", "/rel/b.part2.rar", "/rel/b.part10.rar", // set b in order
        "/rel/Sample/s.rar", "/rel/Sample/s.r00",                     // the folder after
    };
    CHECK(got == want);

    // An upload is ordered by the local names, which are the ones it has.
    std::vector<XferItem> up(2);
    up[0].download = false; up[0].local = "C:/x/v.r00"; up[0].remote = "/zz";
    up[1].download = false; up[1].local = "C:/x/v.rar"; up[1].remote = "/aa";
    xfer_order_rar(&up);
    CHECK(up[0].local == "C:/x/v.rar" && up[1].local == "C:/x/v.r00");
}

// FileZilla's sitemanager.xml, written the way FileZilla 3 writes it: sites at
// the top and inside nested folders, every protocol it numbers, and each way it
// keeps (or does not keep) a password.
static const char* kFileZillaXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- written by FileZilla -->
<FileZilla3 version="3.67.1" platform="windows">
    <Servers>
        <Server>
            <Host>ftp.example.org</Host>
            <Port>21</Port>
            <Protocol>0</Protocol>
            <Type>0</Type>
            <User>bob</User>
            <Pass encoding="base64">aHVudGVyMg==</Pass>
            <Logontype>1</Logontype>
            <PasvMode>MODE_DEFAULT</PasvMode>
            <EncodingType>Auto</EncodingType>
            <BypassProxy>0</BypassProxy>
            <Name>Plain &amp; simple</Name>
            <RemoteDir>1 0 3 pub 8 my files</RemoteDir>
            <SyncBrowsing>0</SyncBrowsing>
        </Server>
        <Folder expanded="1">Work
            <Server>
                <Host>sftp.example.org</Host>
                <Port>2222</Port>
                <Protocol>1</Protocol>
                <Type>0</Type>
                <User>carol</User>
                <Logontype>2</Logontype>
                <PasvMode>MODE_DEFAULT</PasvMode>
                <MaximumMultipleConnections>4</MaximumMultipleConnections>
                <Name>Deploy</Name>
            </Server>
            <Folder expanded="0">Old
                <Server>
                    <Host>legacy.example.org</Host>
                    <Port>990</Port>
                    <Protocol>3</Protocol>
                    <Logontype>1</Logontype>
                    <User>dave</User>
                    <Pass encoding="crypt" pubkey="0123abcd">c2VjcmV0</Pass>
                    <PasvMode>MODE_ACTIVE</PasvMode>
                    <Name>Gr&#252;&#223;e</Name>
                </Server>
            </Folder>
        </Folder>
        <Server>
            <Host>tls.example.org</Host>
            <Port>21</Port>
            <Protocol>4</Protocol>
            <Logontype>0</Logontype>
            <User>anonymous</User>
            <Name>Explicit</Name>
            <RemoteDir />
        </Server>
        <Server>
            <Host>bucket.s3.amazonaws.com</Host>
            <Port>443</Port>
            <Protocol>7</Protocol>
            <Logontype>1</Logontype>
            <Name>Not FTP</Name>
        </Server>
        <Server>
            <Host>keys.example.org</Host>
            <Port>22</Port>
            <Protocol>1</Protocol>
            <Logontype>5</Logontype>
            <User>erin</User>
            <Keyfile>C:\keys\erin.ppk</Keyfile>
            <Name>With a key</Name>
        </Server>
    </Servers>
</FileZilla3>
)";

static const NetImported* find_import(const std::vector<NetImported>& v, const char* name) {
    for (const NetImported& i : v)
        if (i.site.name == name) return &i;
    return nullptr;
}

static void test_filezilla_import(void) {
    std::vector<NetImported> sites;
    int skipped = -1;
    std::string err;
    CHECK(net_filezilla_parse(kFileZillaXml, &sites, &skipped, &err));
    CHECK(err.empty());
    CHECK(sites.size() == 5);
    CHECK(skipped == 1);                                // the S3 bucket
    CHECK(find_import(sites, "Not FTP") == nullptr);

    // Plain FTP: the base64 password comes across, remembered; the port that is
    // the protocol's own is left implicit; the path keeps its space.
    const NetImported* plain = find_import(sites, "Plain & simple");
    CHECK(plain != nullptr);
    if (plain) {
        CHECK(plain->site.proto == NET_FTP);
        CHECK(plain->site.host == "ftp.example.org");
        CHECK(plain->site.port == 0);
        CHECK(plain->site.user == "bob");
        CHECK(plain->site.password == "hunter2");
        CHECK(plain->site.remember);
        CHECK(plain->site.passive);
        CHECK(plain->site.path == "/pub/my files");
        CHECK(plain->note.empty());
        CHECK(plain->site.threads == 2);      // no limit of its own: the default
    }

    // In a folder, a password FileZilla asks for each time: no password, a note.
    const NetImported* deploy = find_import(sites, "Work/Deploy");
    CHECK(deploy != nullptr);
    if (deploy) {
        CHECK(deploy->site.proto == NET_SFTP);
        CHECK(deploy->site.port == 2222);
        CHECK(deploy->site.user == "carol");
        CHECK(deploy->site.password.empty() && !deploy->site.remember);
        CHECK(deploy->site.threads == 4);     // the site's own connection limit
        CHECK(!deploy->note.empty());
    }

    // Two folders deep, a numeric entity in the name, implicit FTPS on its own
    // port, active mode, and a password under the master password: not decoded.
    const NetImported* old = find_import(sites, "Work/Old/Grüße");
    CHECK(old != nullptr);
    if (old) {
        CHECK(old->site.proto == NET_FTPS_IMPLICIT);
        CHECK(old->site.port == 0);
        CHECK(!old->site.passive);
        CHECK(old->site.password.empty() && !old->site.remember);
        CHECK(old->note.find("master password") != std::string::npos);
    }

    // Anonymous: the user FileZilla shows is not a login to carry over.
    const NetImported* tls = find_import(sites, "Explicit");
    CHECK(tls != nullptr);
    if (tls) {
        CHECK(tls->site.proto == NET_FTPS);
        CHECK(tls->site.user.empty());
        CHECK(tls->site.path.empty());
        CHECK(tls->site.verify_tls);
    }

    const NetImported* key = find_import(sites, "With a key");
    CHECK(key != nullptr);
    if (key) {
        CHECK(key->site.proto == NET_SFTP && key->site.user == "erin");
        CHECK(key->note.find("key file") != std::string::npos);
    }

    // What is not a site manager says so instead of importing nothing silently.
    CHECK(!net_filezilla_parse("<FileZilla3><Servers><Server>", &sites, &skipped, &err));
    CHECK(!err.empty());
    CHECK(!net_filezilla_parse("<?xml version=\"1.0\"?><Other/>", &sites, &skipped, &err));
    CHECK(err.find("FileZilla") != std::string::npos);
    CHECK(net_filezilla_parse("<FileZilla3><Servers/></FileZilla3>", &sites, &skipped, &err));
    CHECK(sites.empty() && skipped == 0);

    // From a file: a byte order mark is not part of the XML, and a missing file
    // names the path that was looked at.
    const std::string dir = test_mkdtemp("mc-remote-fz");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    std::string path = dir + "/sitemanager.xml";
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fputs("\xEF\xBB\xBF", f);
        fputs(kFileZillaXml, f);
        fclose(f);
    }
    CHECK(net_filezilla_load(path, &sites, &skipped, &err));
    CHECK(sites.size() == 5);
    CHECK(!net_filezilla_load(dir + "/missing.xml", &sites, &skipped, &err));
    CHECK(err.find("missing.xml") != std::string::npos);

    // Where FileZilla keeps it: under the settings root this system gives it.
    {
        TestHome home(dir);
        std::string where = net_filezilla_path();
#ifdef _WIN32
        CHECK(where == dir + "/.config/FileZilla/sitemanager.xml");
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (!xdg || !*xdg) CHECK(where == dir + "/.config/filezilla/sitemanager.xml");
#endif
    }
    sh("rm -rf " + q(dir));
}

// ---------------------------------------------------------------------------
// the fixture
// ---------------------------------------------------------------------------

struct FtpFixture {
#ifdef _WIN32
    Proc proc;
#endif
    pid_t pid = -1;
    int port = 0;
    std::string root;
    std::string cert, key;

    bool start(const std::string& dir, bool want_tls, const char* rest_mode = "");
    void stop();
};

bool FtpFixture::start(const std::string& dir, bool want_tls, const char* rest_mode) {
    root = dir;
    if (want_tls) {
        // The certificate lives beside the root, never inside it: a key file
        // that showed up in a listing would confuse the checks below.
        cert = dir + "-tls/cert.pem";
        key = dir + "-tls/key.pem";
        // MSYS2_ARG_CONV_EXCL keeps Git for Windows' sh from reading /CN=localhost
        // as a path and rewriting it; it means nothing anywhere else.
        sh("mkdir -p " + q(dir + "-tls") +
           " && MSYS2_ARG_CONV_EXCL='*' openssl req -x509 -newkey rsa:2048 -keyout " + q(key) +
           " -out " + q(cert) + " -days 1 -nodes -subj /CN=localhost"
           " -addext subjectAltName=IP:127.0.0.1 >/dev/null 2>&1");
        if (!exists(cert) || !exists(key)) return false;
    }

#ifdef _WIN32
    {
        // No fork: the server runs through the app's own process runner, which joins
        // its stderr to stdout. It only writes there when FTP_SERVER_LOG asks it to.
        std::vector<std::string> argv = { "python", MC_FTP_SERVER, "--root", root,
                                          "--port", "0", "--user", kUser, "--password", kPass };
        if (want_tls) { argv.push_back("--cert"); argv.push_back(cert);
                        argv.push_back("--key"); argv.push_back(key); }
        if (rest_mode && *rest_mode) argv.push_back(rest_mode);
        std::string err;
        if (!proc_start(argv, nullptr, kNoEnv, &proc, &err)) return false;
        pid = 1;
        std::string line;
        char c;
        while (proc_read(&proc, &c, 1, 10000) == 1 && c != '\n') line.push_back(c);
        size_t at = line.find("\"port\":");
        if (at == std::string::npos) { stop(); return false; }
        port = (int)strtoll(line.c_str() + at + 7, nullptr, 10);
        if (port <= 0) { stop(); return false; }
        return true;
    }
#else
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t child = fork();
    if (child < 0) { close(fds[0]); close(fds[1]); return false; }
    if (child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        // FTP_SERVER_LOG=1 is the documented way to watch the protocol, and the
        // plan is right that without it a client bug looks exactly like a server
        // bug. That only holds if the log survives the run, so when it is asked
        // for, stderr goes to a file beside the root instead of to /dev/null.
        const char* want_log = getenv("FTP_SERVER_LOG");
        // One log per fixture: the plain and the TLS server share a root, and a
        // single file would have the second one erase the first exactly where the
        // TLS session is what needs reading. The REST-mode fixtures share the root
        // for the same reason, so they get their own file too.
        std::string log_path = root + "-server" +
                               (want_tls ? "-tls" : (rest_mode && *rest_mode ? rest_mode : "")) +
                               ".log";
        int logfd = (want_log && *want_log && strcmp(want_log, "0") != 0)
                        ? open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600)
                        : -1;
        if (logfd >= 0) { dup2(logfd, STDERR_FILENO); close(logfd); }
        else if (devnull >= 0) { dup2(devnull, STDERR_FILENO); }
        if (devnull >= 0) close(devnull);
        std::vector<std::string> args = { MC_FTP_SERVER, "--root", root,
                                          "--port", "0", "--user", kUser, "--password", kPass };
        if (want_tls) { args.push_back("--cert"); args.push_back(cert);
                        args.push_back("--key"); args.push_back(key); }
        if (rest_mode && *rest_mode) args.push_back(rest_mode);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("python3"));
        for (std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("python3", argv.data());
        _exit(127);
    }
    close(fds[1]);
    pid = child;

    // The server prints its port once it is listening, so that line is both the
    // port and the permission to connect.
    std::string line;
    struct pollfd pfd = { fds[0], POLLIN, 0 };
    while (true) {
        int pr = poll(&pfd, 1, 10000);
        if (pr <= 0) break;
        char c;
        ssize_t n = read(fds[0], &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        line.push_back(c);
    }
    close(fds[0]);

    size_t at = line.find("\"port\":");
    if (at == std::string::npos) { stop(); return false; }
    port = (int)strtoll(line.c_str() + at + 7, nullptr, 10);
    if (port <= 0) { stop(); return false; }
    return true;
#endif
}

void FtpFixture::stop() {
#ifdef _WIN32
    if (pid > 0) proc_finish(&proc, true);
#else
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
#endif
    pid = -1;
}

static NetSite fixture_site(const FtpFixture& fx) {
    NetSite s;
    s.name = "fixture";
    s.proto = fx.cert.empty() ? NET_FTP : NET_FTPS;
    s.host = "127.0.0.1";
    s.port = fx.port;
    s.user = kUser;
    s.password = kPass;
    s.remember = false;
    return s;
}

// A callback that counts, and aborts after `after` calls when that is not -1.
struct Prog {
    int calls = 0;
    long long bytes = 0;
    int after = -1;
};

static bool prog_cb(long long done, long long total, int done_files, int total_files, void* user) {
    Prog* p = (Prog*)user;
    p->calls++;
    p->bytes += done;
    (void)total; (void)done_files; (void)total_files;
    return p->after < 0 || p->calls <= p->after;
}

// ---------------------------------------------------------------------------
// transport, against the fixture
// ---------------------------------------------------------------------------

static void test_ftp_listing(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::vector<FileEntry> rows;

    CHECK(net_list(site, "/", &rows, &err));
    CHECK(find_row(rows, "hello.txt") != nullptr);
    CHECK(find_row(rows, "sub") != nullptr);
    // The pane adds its own ".." row, so the server's dot entries would be
    // duplicates on screen.
    CHECK(find_row(rows, ".") == nullptr);
    CHECK(find_row(rows, "..") == nullptr);

    const FileEntry* hello = find_row(rows, "hello.txt");
    if (hello) {
        CHECK(hello->size == 12);
        CHECK(S_ISREG(hello->mode));
#ifdef _WIN32
        // NTFS keeps no mode for the server to report, only the read-only bit:
        // Python makes every writable file rw-rw-rw-.
        CHECK((hello->mode & 0777) == 0666);
#else
        CHECK((hello->mode & 0777) == 0640);
#endif
        // A listing with no date would sort itself to the top of every date sort.
        CHECK(hello->mtime != 0);
        CHECK(llabs((long long)(time(nullptr) - hello->mtime)) < 3600);
    }
    const FileEntry* sub = find_row(rows, "sub");
    if (sub) CHECK(S_ISDIR(sub->mode));

    // A name with a space in it arrives whole, not as two rows.
    rows.clear();
    CHECK(net_list(site, "/sub", &rows, &err));
    CHECK(find_row(rows, "a file.txt") != nullptr);
    CHECK(rows.size() == 1);

    // A path that is not there fails with the server's own words.
    rows.clear();
    CHECK(!net_list(site, "/no-such-dir", &rows, &err));
    CHECK(!err.empty());

    FileEntry e{};
    CHECK(net_stat(site, "/hello.txt", &e, &err));
    CHECK(!strcmp(e.name, "hello.txt"));
    CHECK(e.size == 12);
    CHECK(S_ISREG(e.mode));

    CHECK(net_stat(site, "/sub", &e, &err));
    CHECK(S_ISDIR(e.mode));

    CHECK(!net_stat(site, "/gone.txt", &e, &err));
}

// A name the URL cannot carry is ordinary on a server. The path is escaped where
// the URL is built and nowhere else, so the pane keeps the name as the server
// spells it - which is what the path bar shows and what a transfer names back -
// and curl is handed a URL it will accept. Before that, curl answered "URL
// rejected: Malformed input to a URL function" for a directory named
// "IDA Pro 9.6.261001", and the pane showed an empty listing under a message
// about a filter nobody had set.
static void test_ftp_escaped_names(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::vector<FileEntry> rows;
    std::string out = fx.root + "-spaced";
    sh("mkdir -p " + q(out));

    // The row keeps the space, because everything downstream is built from it.
    CHECK(net_list(site, "/", &rows, &err));
    const FileEntry* spaced = find_row(rows, "IDA Pro 9.6.261001");
    CHECK(spaced != nullptr);
    if (spaced) CHECK(S_ISDIR(spaced->mode));

    // Walking in is the reported failure.
    rows.clear(); err.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001", &rows, &err));
    CHECK(find_row(rows, "inner.txt") != nullptr);

    FileEntry e{};
    CHECK(net_stat(site, "/IDA Pro 9.6.261001/inner.txt", &e, &err));
    CHECK(!strcmp(e.name, "inner.txt"));

    std::string down = out + "/got inner.txt";
    CHECK(net_download(site, "/IDA Pro 9.6.261001/inner.txt", down.c_str(), nullptr, nullptr, &err));
    CHECK(read_file(down) == "nested\n");
    // A download that fails leaves nothing behind, and the name it failed on had
    // a space in it.
    CHECK(!net_download(site, "/IDA Pro 9.6.261001/gone.txt", (out + "/gone.txt").c_str(),
                        nullptr, nullptr, &err));
    CHECK(!exists(out + "/gone.txt"));

    std::string up = out + "/put source.txt";
    write_file(up, "up\n");
    CHECK(net_upload(site, "/IDA Pro 9.6.261001/put source.txt", up.c_str(), nullptr, nullptr, &err));
    rows.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001", &rows, &err));
    CHECK(find_row(rows, "put source.txt") != nullptr);

    // A whole tree goes across in one call, and every component of every path
    // inside it needs the same encoding: the directory the server has to create
    // carries a space, and so does a file inside it.
    std::string tree = out + "/my tree";
    sh("rm -rf " + q(tree) + " && mkdir -p " + q(tree + "/deep dir") +
       " && printf 'one\\n' > " + q(tree + "/one two.txt") +
       " && printf 'two\\n' > " + q(tree + "/deep dir/two.txt"));
    CHECK(net_upload_tree(site, tree.c_str(), "/IDA Pro 9.6.261001/my tree", nullptr, nullptr, &err));
    rows.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001/my tree", &rows, &err));
    CHECK(find_row(rows, "one two.txt") != nullptr);
    CHECK(find_row(rows, "deep dir") != nullptr);
    rows.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001/my tree/deep dir", &rows, &err));
    CHECK(find_row(rows, "two.txt") != nullptr);

    std::string back = out + "/back";
    CHECK(net_download_tree(site, "/IDA Pro 9.6.261001/my tree", back.c_str(), nullptr, nullptr, &err));
    CHECK(read_file(back + "/one two.txt") == "one\n");
    CHECK(read_file(back + "/deep dir/two.txt") == "two\n");

    // MKD, RNFR/RNTO and DELE carry the name as a command argument rather than in
    // a URL, and the directory they are sent against is the one with the space in
    // its path.
    CHECK(net_mkdir(site, "/IDA Pro 9.6.261001/new sub", &err));
    rows.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001", &rows, &err));
    CHECK(find_row(rows, "new sub") != nullptr);
    CHECK(net_rename(site, "/IDA Pro 9.6.261001/put source.txt", "renamed one.txt", &err));
    rows.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001", &rows, &err));
    CHECK(find_row(rows, "renamed one.txt") != nullptr);
    CHECK(net_remove(site, "/IDA Pro 9.6.261001/renamed one.txt", false, &err));
    rows.clear();
    CHECK(net_list(site, "/IDA Pro 9.6.261001", &rows, &err));
    CHECK(find_row(rows, "renamed one.txt") == nullptr);

    // '#' ends a path where the name has not and '%' starts an escape; both are
    // ordinary characters in a name.
    rows.clear(); err.clear();
    CHECK(net_list(site, "/hash#and;percent%", &rows, &err));
    CHECK(find_row(rows, "p#1.txt") != nullptr);
    std::string odd = out + "/odd";
    CHECK(net_download(site, "/hash#and;percent%/p#1.txt", odd.c_str(), nullptr, nullptr, &err));
    CHECK(read_file(odd) == "odd\n");
}

static void test_ftp_transfer(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::string out = fx.root + "-local";
    sh("mkdir -p " + q(out));

    Prog prog;
    std::string local = out + "/got.txt";
    CHECK(net_download(site, "/hello.txt", local.c_str(), prog_cb, &prog, &err));
    CHECK(read_file(local) == "hello world\n");
    CHECK(prog.calls >= 0);

    // A download that fails must not leave a truncated file where the user will
    // open it.
    std::string bad = out + "/bad.txt";
    CHECK(!net_download(site, "/gone.txt", bad.c_str(), nullptr, nullptr, &err));
    CHECK(!exists(bad));

    std::string up = out + "/put.txt";
    write_file(up, "uploaded body\n");
    CHECK(net_upload(site, "/put.txt", up.c_str(), nullptr, nullptr, &err));
    FileEntry e{};
    CHECK(net_stat(site, "/put.txt", &e, &err));
    CHECK(e.size == 14);

    CHECK(!net_upload(site, "/x.txt", (out + "/nothing-here").c_str(), nullptr, nullptr, &err));

    // A directory goes up one MKD and one store per entry, and comes back the
    // same way, because FTP has no recursive transfer in either direction.
    std::string tree = out + "/tree";
    sh("rm -rf " + q(tree) + " && mkdir -p " + q(tree + "/deep") +
       " && printf 'one\\n' > " + q(tree + "/one.txt") +
       " && printf 'two\\n' > " + q(tree + "/deep/two.txt"));
    CHECK(net_upload_tree(site, tree.c_str(), "/tree", nullptr, nullptr, &err));
    std::vector<FileEntry> rows;
    CHECK(net_list(site, "/tree", &rows, &err));
    CHECK(find_row(rows, "one.txt") != nullptr);
    CHECK(find_row(rows, "deep") != nullptr);
    rows.clear();
    CHECK(net_list(site, "/tree/deep", &rows, &err));
    CHECK(find_row(rows, "two.txt") != nullptr);

    std::string back = out + "/back";
    sh("rm -rf " + q(back));
    CHECK(net_download_tree(site, "/tree", back.c_str(), nullptr, nullptr, &err));
    CHECK(read_file(back + "/one.txt") == "one\n");
    CHECK(read_file(back + "/deep/two.txt") == "two\n");

    // Abort: the callback returning false stops the walk, so the progress
    // dialog's Abort button means what it says.
    Prog stopper;
    stopper.after = 1;
    std::string partial = out + "/partial";
    sh("rm -rf " + q(partial));
    CHECK(!net_upload_tree(site, tree.c_str(), "/partial", prog_cb, &stopper, &err));
    rows.clear();
    if (net_list(site, "/partial", &rows, &err)) CHECK(rows.size() < 2);
    CHECK(stopper.calls >= 1);

    // Rename, mode, and the delete of a whole tree.
    CHECK(net_rename(site, "/put.txt", "renamed.txt", &err));
    CHECK(!net_stat(site, "/put.txt", &e, &err));
    CHECK(net_stat(site, "/renamed.txt", &e, &err));
    CHECK(!net_rename(site, "/renamed.txt", "no/where", &err));

    CHECK(net_chmod(site, "/renamed.txt", 0600, &err));
    CHECK(net_stat(site, "/renamed.txt", &e, &err));
#ifndef _WIN32
    // The SITE CHMOD reached the server either way; only a POSIX one can keep it.
    CHECK((e.mode & 0777) == 0600);
#endif

    CHECK(net_remove(site, "/renamed.txt", false, &err));
    CHECK(!net_stat(site, "/renamed.txt", &e, &err));

    CHECK(net_mkdir(site, "/made", &err));
    CHECK(net_stat(site, "/made", &e, &err) && S_ISDIR(e.mode));

    Prog del;
    CHECK(net_remove_tree(site, "/tree", prog_cb, &del, &err));
    CHECK(!net_stat(site, "/tree", &e, &err));
    CHECK(del.calls >= 2);   // the directory's entries, then the directory

    sh("rm -rf " + q(out));
}

// A quick view wants the beginning of a file, not the file. The cap has to hold
// against a file far larger than it, and a file smaller than the cap has to come
// back whole - that second case is what decides whether browsing a directory of
// small text files costs a fresh login per file or not.
static void test_ftp_prefix(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err, body;

    CHECK(net_download_prefix(site, "/hello.txt", &body, 4096, nullptr, nullptr, &err));
    CHECK(body == "hello world\n");

    // A cap exactly the size of the file: the transfer ends by itself, and the
    // answer is the same one.
    CHECK(net_download_prefix(site, "/hello.txt", &body, 12, nullptr, nullptr, &err));
    CHECK(body == "hello world\n");

    std::string big = fx.root + "/big.txt";
    sh("head -c 200000 /dev/zero | tr '\\0' 'z' > " + q(big));
    CHECK(net_download_prefix(site, "/big.txt", &body, 4096, nullptr, nullptr, &err));
    CHECK(body.size() == 4096);
    CHECK(body.find_first_not_of('z') == std::string::npos);
    remove(big.c_str());

    // A name the URL cannot carry still fetches: escaping is the URL's problem,
    // not the caller's.
    CHECK(net_download_prefix(site, "/IDA Pro 9.6.261001/inner.txt", &body, 4096,
                              nullptr, nullptr, &err));
    CHECK(body == "nested\n");

    CHECK(!net_download_prefix(site, "/gone.txt", &body, 4096, nullptr, nullptr, &err));
    CHECK(!err.empty());
    CHECK(body.empty());

    // The callback is how a caller stops a fetch nobody wants any more, because
    // the cursor moved on while the bytes were still on the wire.
    Prog stopper;
    stopper.after = 0;
    std::string huge = fx.root + "/huge.txt";
    sh("head -c 2000000 /dev/zero | tr '\\0' 'q' > " + q(huge));
    CHECK(!net_download_prefix(site, "/huge.txt", &body, 1000000, prog_cb, &stopper, &err));
    CHECK(err == "aborted");
    CHECK(body.empty());
    CHECK(stopper.calls >= 1);
    remove(huge.c_str());
}

// A background transfer runs on its own thread while the panes keep browsing the
// same site, and one curl handle cannot serve two threads at once - so the handle
// cache is per thread. That is asserted in the source, but the assertion is only
// worth anything if something exercises the overlap: three threads list and
// download from one site at the same time, and a handle shared between them
// answers one of them with another's bytes, or with nothing at all. Correct
// answers out of all three is the check.
// The sidecar a download writes before the transfer is known good. Two promises:
// what sits under the real name is the complete file, and a transfer that stopped
// part way leaves its partial where a resume can pick it up.
static void test_ftp_partial(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::string out = fx.root + "-partial";
    sh("mkdir -p " + q(out));

    std::string huge = fx.root + "/huge.txt";
    sh("head -c 2000000 /dev/zero | tr '\\0' 'q' > " + q(huge));

    // A good transfer leaves the real name and nothing else.
    std::string good = out + "/good.txt";
    long long got = -1;
    CHECK(net_download(site, "/huge.txt", good.c_str(), nullptr, nullptr, &err, 0, &got));
    CHECK(got == 2000000);
    CHECK(file_size(good) == 2000000);
    CHECK(!exists(good + ".part"));

    // An aborted transfer leaves nothing under the real name, and the sidecar holds
    // exactly the bytes this call wrote - which may be none, because an abort that
    // lands before the first chunk has nothing to keep. Asserting the equality
    // rather than a size is the honest form: where the abort lands on a 2 MB file
    // over a loopback socket is not something a test can pin down.
    Prog stopper;
    stopper.after = 2;
    std::string half = out + "/half.txt";
    got = -1;
    CHECK(!net_download(site, "/huge.txt", half.c_str(), prog_cb, &stopper, &err, 0, &got));
    CHECK(err == "aborted");
    CHECK(!exists(half));
    CHECK(net_part_size(half.c_str()) == (got > 0 ? got : 0));

    // Bytes that arrived are kept even when the transfer did not finish. The
    // failure here is the rename onto a directory, which is deterministic in a way
    // an abort's timing is not: the body is real and the name could not be put in
    // place, so the partial is what is left holding it.
    std::string blocked = out + "/blocked";
    sh("mkdir -p " + q(blocked));
    got = -1;
    CHECK(!net_download(site, "/hello.txt", blocked.c_str(), nullptr, nullptr, &err, 0, &got));
    CHECK(got == 12);
    CHECK(net_part_size(blocked.c_str()) == 12);
    sh("rm -rf " + q(blocked));

    // A transfer that never started keeps nothing: an empty sidecar under a name
    // the user will see is worse than no file, and it is the promise the old
    // remove(local) made.
    std::string none = out + "/none.txt";
    CHECK(!net_download(site, "/gone.txt", none.c_str(), nullptr, nullptr, &err));
    CHECK(!exists(none));
    CHECK(!exists(none + ".part"));
    CHECK(net_part_size(none.c_str()) == 0);

    // A restart clears the leftover before it starts, so "again" means from zero
    // and not on top of a half file.
    sh("printf 'stale' > " + q(half + ".part"));
    CHECK(net_download(site, "/hello.txt", half.c_str(), nullptr, nullptr, &err));
    CHECK(read_file(half) == "hello world\n");
    CHECK(!exists(half + ".part"));

    remove(huge.c_str());
    sh("rm -rf " + q(out));
}

// Resume, against a server that answers REST. The point of the feature: a transfer
// that stopped part way continues from the bytes it already moved, and the file
// that comes out is byte for byte the one a clean transfer would have produced.
static void test_ftp_resume(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::string out = fx.root + "-resume";
    sh("mkdir -p " + q(out));

    std::string big = fx.root + "/resume-src.bin";
    sh("head -c 200000 /dev/zero | tr '\\0' 'q' > " + q(big));
    std::string whole = read_file(big);
    CHECK(whole.size() == 200000);

    // 80 000 bytes are already on disk as a sidecar.
    std::string dst = out + "/big.bin";
    write_file(dst + ".part", whole.substr(0, 80000));
    CHECK(net_part_size(dst.c_str()) == 80000);

    long long got = -1;
    CHECK(net_download(site, "/resume-src.bin", dst.c_str(), nullptr, nullptr, &err,
                       net_part_size(dst.c_str()), &got));
    CHECK(got == 120000);                 // only what was left came over the wire
    CHECK(file_size(dst) == 200000);
    CHECK(read_file(dst) == whole);       // the resumed file is the file
    CHECK(!exists(dst + ".part"));

    // A tree resumes per entry: the file with a sidecar continues from it and the
    // ones without are fetched whole. The tree is built here because the transfer
    // test above takes /tree back off the server when it finishes.
    std::string up = out + "/src tree";
    sh("mkdir -p " + q(up + "/deep dir") +
       " && printf 'one\\n' > " + q(up + "/one two.txt") +
       " && printf 'two\\n' > " + q(up + "/deep dir/two.txt"));
    CHECK(net_upload_tree(site, up.c_str(), "/resume tree", nullptr, nullptr, &err));

    std::string back = out + "/back";
    sh("rm -rf " + q(back));
    CHECK(net_download_tree(site, "/resume tree", back.c_str(), nullptr, nullptr, &err));
    std::string one = read_file(back + "/one two.txt");
    sh("rm -f " + q(back + "/one two.txt"));
    write_file(back + "/one two.txt.part", one.substr(0, 2));
    CHECK(net_download_tree(site, "/resume tree", back.c_str(), nullptr, nullptr, &err, true));
    CHECK(read_file(back + "/one two.txt") == one);
    CHECK(read_file(back + "/deep dir/two.txt") == "two\n");
    CHECK(!exists(back + "/one two.txt.part"));
    CHECK(net_remove_tree(site, "/resume tree", nullptr, nullptr, &err));

    // An upload continues the file the server already has: curl reads the source
    // from the offset and the server writes from the same place. The half the
    // server has is written straight into its root, because that is where a
    // stopped upload left it.
    sh("head -c 100000 /dev/zero | tr '\\0' 'q' > " + q(fx.root + "/resumeme.bin"));
    std::string src = out + "/full.bin";
    write_file(src, whole);
    CHECK(net_upload(site, "/resumeme.bin", src.c_str(), nullptr, nullptr, &err, 100000));
    FileEntry e{};
    CHECK(net_stat(site, "/resumeme.bin", &e, &err));
    CHECK((long long)e.size == 200000);
    CHECK(read_file(fx.root + "/resumeme.bin") == whole);

    // A resume asked for past the end of the file on the server. There is nothing
    // to fetch from there, and the server's own answer is the reason the user sees.
    // The partial is kept - nothing is wrong with those bytes - and nothing lands at
    // the real name.
    write_file(fx.root + "/tiny.bin", "hello world\n");
    std::string tdst = out + "/tiny.txt";
    write_file(tdst + ".part", std::string(5000, 'x'));
    long long tgot = -1;
    CHECK(!net_download(site, "/tiny.bin", tdst.c_str(), nullptr, nullptr, &err, 5000, &tgot));
    CHECK(err.find("beyond file size") != std::string::npos);
    CHECK(tgot == 0);
    CHECK(!exists(tdst));
    CHECK(net_part_size(tdst.c_str()) == 5000);

    remove(big.c_str());
    sh("rm -rf " + q(out) + " " + q(fx.root + "/resumeme.bin") + " " + q(fx.root + "/tiny.bin"));
}

// The two ways a server stops a resume from the outside: one that says no to REST,
// and one that cannot report a size, which is the number curl needs before it will
// start at an offset. Both have to leave the partial exactly as it was and a reason
// on screen - a resume that fails must not cost the bytes already earned.
static void test_ftp_resume_refused(FtpFixture& fx, const char* mode) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::string out = fx.root + std::string(mode);
    sh("mkdir -p " + q(out));
    std::string big = fx.root + "/resume-src.bin";
    sh("head -c 200000 /dev/zero | tr '\\0' 'q' > " + q(big));

    std::string dst = out + "/big.bin";
    write_file(dst + ".part", std::string(80000, 'q'));
    long long got = -1;
    CHECK(!net_download(site, "/resume-src.bin", dst.c_str(), nullptr, nullptr, &err,
                        80000, &got));
    CHECK(!err.empty());
    CHECK(!exists(dst));   // nothing reached the real name either way
    // A refused resume leaves the partial exactly as it was: there is nothing wrong
    // with those bytes, the resume is what failed.
    CHECK(net_part_size(dst.c_str()) == 80000);

    remove(big.c_str());
    sh("rm -rf " + q(out));
}

static void test_ftp_concurrent(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    const int kThreads = 3;
    const int kRounds = 6;
    std::string out = fx.root + "-concurrent";
    sh("rm -rf " + q(out) + " && mkdir -p " + q(out));

    std::atomic<int> listed{0};
    std::atomic<int> got{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.push_back(std::thread([&, t] {
            // Each thread works from its own copy of the site: a pane hands a
            // background transfer a copy for the same reason, because the saved
            // sites can be edited while the thread is still running.
            NetSite mine = site;
            std::string local = out + "/dl" + std::to_string(t);
            for (int i = 0; i < kRounds; i++) {
                std::vector<FileEntry> rows;
                std::string err;
                if (net_list(site, "/", &rows, &err) && find_row(rows, "hello.txt"))
                    listed++;
                if (net_download(mine, "/hello.txt", local.c_str(), nullptr, nullptr, &err) &&
                    read_file(local) == "hello world\n")
                    got++;
            }
        }));
    }
    for (std::thread& th : threads) th.join();
    CHECK(listed.load() == kThreads * kRounds);
    CHECK(got.load() == kThreads * kRounds);
    sh("rm -rf " + q(out));
}

// The default is that a certificate has to check out. This is the one place the
// rule is proved: the same server, the same site, one flag apart.
static void test_ftp_tls(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::vector<FileEntry> rows;

    CHECK(site.proto == NET_FTPS);
    CHECK(!net_list(site, "/", &rows, &err));
    CHECK(!err.empty());

    site.verify_tls = false;
    CHECK(net_list(site, "/", &rows, &err));
    CHECK(find_row(rows, "hello.txt") != nullptr);
}

// The transfer queue: a tree planned into files and moved by several threads at
// once, each over its own connection. What has to hold is what held when one
// thread walked the tree - every byte arrives, folders (empty ones too) are made,
// a resume continues the .part - plus what the queue adds: the job knows its
// total before it starts, counts every byte once whichever thread moved it, and
// an abort leaves nothing pending.
static void test_ftp_queue(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    site.threads = 4;
    std::string err;

    // Twelve files in three folders, sizes all different so a byte counted twice
    // or not at all shows up in the total, and one empty folder.
    long long want_total = 0;
    for (int i = 0; i < 12; i++) {
        std::string dir = fx.root + "/qtree/d" + std::to_string(i % 3);
        sh("mkdir -p " + q(dir));
        long long size = 50000 + i * 7919;
        write_file(dir + "/f" + std::to_string(i) + ".bin", std::string((size_t)size, (char)('a' + i)));
        want_total += size;
    }
    sh("mkdir -p " + q(fx.root + "/qtree/empty"));

    const std::string out = fx.root + "-queue";
    sh("rm -rf " + q(out) + " && mkdir -p " + q(out));

    XferJob job;
    job.begin(1);
    std::vector<XferItem> items;
    CHECK(xfer_plan_download(site, "/qtree", out + "/qtree", &job, 0, false, &items, &err));
    CHECK(items.size() == 12);
    CHECK(job.total.load() == want_total);           // known before a byte moves
    CHECK(job.files_total.load() == 12);
    CHECK(exists(out + "/qtree/empty"));             // folders are made by the plan
    xfer_run(&job, site, site.threads, items);
    CHECK(job.pending == 0);
    CHECK(job.group_ok(0));
    CHECK(job.files_done.load() == 12);
    CHECK(job.done.load() == want_total);            // every byte once
    bool all_good = true;
    for (int i = 0; i < 12; i++) {
        std::string p = out + "/qtree/d" + std::to_string(i % 3) + "/f" + std::to_string(i) + ".bin";
        if (read_file(p) != std::string((size_t)(50000 + i * 7919), (char)('a' + i))) all_good = false;
    }
    CHECK(all_good);

    // The site allowed four, and twelve files are enough work for four: that many
    // threads were started, and they wait a moment for more before they end.
    std::vector<XferThreadStat> threads;
    int queued = -1;
    xfer_snapshot(&threads, &queued);
    CHECK(threads.size() == 4);
    CHECK(queued == 0);
    long long by_threads = 0;
    for (const XferThreadStat& s : threads) by_threads += s.bytes;
    CHECK(by_threads == want_total);                 // and between them moved it all

    // Up again, into a new folder, over three threads.
    XferJob up;
    up.begin(1);
    items.clear();
    CHECK(xfer_plan_upload(site, out + "/qtree", "/qtree up", &up, 0, false, &items, &err));
    CHECK(items.size() == 12);
    xfer_run(&up, site, 3, items);
    CHECK(up.group_ok(0));
    CHECK(up.done.load() == want_total);
    std::vector<FileEntry> rows;
    CHECK(net_list(site, "/qtree up", &rows, &err));
    CHECK(find_row(rows, "empty") != nullptr);       // the empty folder went up too
    CHECK(net_list(site, "/qtree up/d1", &rows, &err));
    CHECK(rows.size() == 4);
    const FileEntry* f4 = find_row(rows, "f4.bin");
    CHECK(f4 && (long long)f4->size == 50000 + 4 * 7919);

    // A resume continues the .part and counts what was already there, so the job
    // still ends at its total rather than short of it or past it.
    std::string again = out + "/again";
    sh("mkdir -p " + q(again));
    std::string part = again + "/f5.bin.part";
    long long size5 = 50000 + 5 * 7919;
    write_file(part, std::string((size_t)(size5 / 2), 'f'));
    XferJob res;
    res.begin(1);
    items.clear();
    CHECK(xfer_plan_download(site, "/qtree/d2/f5.bin", again + "/f5.bin", &res, 0, true, &items, &err));
    CHECK(items.size() == 1 && items[0].size == size5);
    xfer_run(&res, site, 2, items);
    CHECK(res.group_ok(0));
    CHECK(read_file(again + "/f5.bin") == std::string((size_t)size5, 'f'));
    CHECK(res.done.load() == size5);

    // An abort: the job's files that no thread took are dropped from the queue at
    // once, the job is not left waiting for them, and the group says it failed.
    XferJob stop;
    stop.begin(1);
    items.clear();
    CHECK(xfer_plan_download(site, "/qtree", out + "/stopped", &stop, 0, false, &items, &err));
    stop.abort = true;
    xfer_run(&stop, site, 4, items);
    CHECK(stop.pending == 0);
    CHECK(!stop.group_ok(0));
    CHECK(stop.first_err == "aborted");

    // Two groups in one job: a name whose file cannot be fetched fails alone.
    XferJob two;
    two.begin(2);
    items.clear();
    CHECK(xfer_plan_download(site, "/qtree/d0/f0.bin", out + "/two-a.bin", &two, 0, false, &items, &err));
    CHECK(xfer_plan_download(site, "/qtree/no such.bin", out + "/two-b.bin", &two, 1, false, &items, &err));
    xfer_run(&two, site, 2, items);
    CHECK(two.group_ok(0));
    CHECK(!two.group_ok(1));
    CHECK(!two.first_err.empty());

    // Quit: every thread stops and is joined, and the pools are gone.
    xfer_shutdown();
    xfer_snapshot(&threads, &queued);
    CHECK(threads.empty());

    net_remove_tree(site, "/qtree", nullptr, nullptr, &err);
    net_remove_tree(site, "/qtree up", nullptr, nullptr, &err);
    sh("rm -rf " + q(out));
}

// A wrong password is the server's answer, not a crash and not an empty listing
// that looks like an empty directory.
static void test_ftp_bad_login(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    site.password = "wrong";
    std::string err;
    std::vector<FileEntry> rows;
    CHECK(!net_list(site, "/", &rows, &err));
    CHECK(!err.empty());
}

// The connection log holds the conversation - the commands and the replies - and
// never the password, which is the one thing a log someone pastes into a bug
// report must not carry.
static void test_ftp_log(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    site.password = "s3cret-do-not-log";
    net_forget(nullptr);   // a fresh login, so USER and PASS are said again
    uint64_t before = net_log_since(0, nullptr);
    std::string err;
    std::vector<FileEntry> rows;
    net_list(site, "/", &rows, &err);

    std::vector<NetLogLine> lines;
    net_log_since(before, &lines);
    bool sent_user = false, sent_pass = false, got_reply = false, leaked = false;
    for (const NetLogLine& l : lines) {
        if (l.kind == NETLOG_SENT && !strncmp(l.text.c_str(), "USER ", 5)) sent_user = true;
        if (l.kind == NETLOG_SENT && l.text == "PASS ****") sent_pass = true;
        if (l.kind == NETLOG_RECV) got_reply = true;
        if (l.text.find("s3cret") != std::string::npos) leaked = true;
        CHECK(l.site == net_site_root(site));
    }
    CHECK(sent_user);
    CHECK(sent_pass);
    CHECK(got_reply);
    CHECK(!leaked);

    // A reader that has seen everything gets nothing more.
    std::vector<NetLogLine> none;
    uint64_t now = net_log_since(0, nullptr);
    net_log_since(now, &none);
    CHECK(none.empty());
    net_forget(nullptr);
}

// A listing asked to stop stops, says so, and leaves the next listing working:
// that is what lets a pane walk away from a server that is not answering.
static bool stop_now_cb(long long, long long, int, int, void* user) {
    (*(int*)user)++;
    return false;
}

static void test_ftp_list_cancel(FtpFixture& fx) {
    NetSite site = fixture_site(fx);
    std::string err;
    std::vector<FileEntry> rows;
    int asked = 0;
    CHECK(!net_list(site, "/", &rows, &err, stop_now_cb, &asked));
    CHECK(asked >= 1);
    CHECK(err == "cancelled");

    err.clear();
    CHECK(net_list(site, "/", &rows, &err));
    CHECK(find_row(rows, "hello.txt") != nullptr);
}

// A server that cannot be reached is asked once. The retry is for a cached
// connection the server dropped, and LIST is for a server that does not know
// MLSD; neither helps a port nothing listens on, and each used to add a full
// connect timeout to the wait.
static void test_unreachable_asks_once(void) {
    NetSite site;
    site.host = "127.0.0.1";
    site.port = 1;
    site.user = "someone";
    net_forget(nullptr);
    uint64_t before = net_log_since(0, nullptr);
    std::string err;
    std::vector<FileEntry> rows;
    CHECK(!net_list(site, "/", &rows, &err));
    CHECK(!err.empty());
    std::vector<NetLogLine> lines;
    net_log_since(before, &lines);
    int attempts = 0, errors = 0;
    for (const NetLogLine& l : lines) {
        if (l.kind == NETLOG_INFO && !strncmp(l.text.c_str(), "Listing ", 8)) attempts++;
        if (l.kind == NETLOG_ERROR) errors++;
    }
    CHECK(attempts == 1);
    CHECK(errors == 1);
}

// ---------------------------------------------------------------------------
// a build without libcurl
// ---------------------------------------------------------------------------

static void test_without_curl(void) {
    char what[256];
    net_what(what, sizeof(what));
    if (net_available()) {
        CHECK(strstr(what, "libcurl") != nullptr);
        return;
    }
    // The app still runs, and every transport call says why it cannot work.
    CHECK(what[0] != '\0');
    NetSite site;
    site.host = "example.com";
    std::string err;
    std::vector<FileEntry> rows;
    CHECK(!net_list(site, "/", &rows, &err));
    CHECK(err.find("no FTP support") != std::string::npos);
    FileEntry e{};
    CHECK(!net_stat(site, "/x", &e, &err));
    CHECK(!net_download(site, "/x", "/tmp/x", nullptr, nullptr, &err));
    CHECK(!net_upload(site, "/x", "/etc/hostname", nullptr, nullptr, &err));
    // The sidecar plumbing is not transport: a name and a stat, so it answers the
    // same way in a build that cannot fetch anything.
    CHECK(net_part_path("/tmp/x") == "/tmp/x.part");
    CHECK(net_part_size("/tmp/immc-no-such-file") == 0);
    CHECK(!net_mkdir(site, "/x", &err));
    CHECK(!net_remove(site, "/x", false, &err));
    CHECK(!net_rename(site, "/x", "y", &err));
    CHECK(!net_chmod(site, "/x", 0600, &err));
    CHECK(!net_remove_tree(site, "/x", nullptr, nullptr, &err));
    CHECK(!net_download_tree(site, "/x", "/tmp/x", nullptr, nullptr, &err));
    CHECK(!net_upload_tree(site, "/etc/hostname", "/x", nullptr, nullptr, &err));
}

// ---------------------------------------------------------------------------

static bool have_python(void) {
#ifdef _WIN32
    int rc = test_system("command -v python >/dev/null 2>&1");
#else
    int rc = system("command -v python3 >/dev/null 2>&1");
#endif
    return rc == 0;
}

int main(void) {
    test_proto_vocabulary();
    test_parse_url();
    test_virtual_paths();
    test_unescape();
    test_escape();
    test_mlsd_parser();
    test_list_parser();
    test_mode_from_rwx();
    test_sites_file();
    test_filezilla_import();
    test_rar_order();
    test_without_curl();

    if (!net_available()) {
        printf("libcurl is not in this build: the transport tests are skipped\n");
        printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    test_unreachable_asks_once();
    if (!have_python()) {
        printf("python3 is not installed: the transport tests are skipped\n");
        printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }

    const std::string base = test_mkdtemp("mc-remote-ftp");
    if (base.empty()) {
        printf("cannot make a temporary root: %s\n", strerror(errno));
        return 1;
    }
    std::string root = base + "/root";
    sh("mkdir -p " + q(root + "/sub") +
       " && mkdir -p " + q(root + "/IDA Pro 9.6.261001") +
       " && mkdir -p " + q(root + "/hash#and;percent%") +
       " && printf 'hello world\\n' > " + q(root + "/hello.txt") +
       " && printf 'nested\\n' > " + q(root + "/sub/a file.txt") +
       " && printf 'nested\\n' > " + q(root + "/IDA Pro 9.6.261001/inner.txt") +
       " && printf 'odd\\n' > " + q(root + "/hash#and;percent%/p#1.txt") +
       " && chmod 640 " + q(root + "/hello.txt"));

    FtpFixture fx;
    if (!fx.start(root, false)) {
        printf("the test ftp server did not start: the transport tests are skipped\n");
    } else {
        test_ftp_listing(fx);
        test_ftp_escaped_names(fx);
        test_ftp_bad_login(fx);
        test_ftp_log(fx);
        test_ftp_list_cancel(fx);
        test_ftp_transfer(fx);
        test_ftp_prefix(fx);
        test_ftp_partial(fx);
        test_ftp_resume(fx);
        test_ftp_concurrent(fx);
        test_ftp_queue(fx);
        fx.stop();
    }

    // A server that refuses REST, and one that cannot report a size, which is the
    // number curl needs before it will start at an offset at all. Both are real
    // servers, and neither may cost the bytes already earned.
    FtpFixture fx_nores;
    if (fx_nores.start(root, false, "--no-rest")) {
        test_ftp_resume_refused(fx_nores, "-nores");
        fx_nores.stop();
    } else {
        printf("the no-REST ftp server did not start: that check is skipped\n");
    }

    FtpFixture fx_nosize;
    if (fx_nosize.start(root, false, "--no-size")) {
        test_ftp_resume_refused(fx_nosize, "-nosize");
        fx_nosize.stop();
    } else {
        printf("the no-SIZE ftp server did not start: that check is skipped\n");
    }

    // TLS needs a certificate, and generating one needs openssl. Without it the
    // plain-FTP half above still ran.
    FtpFixture fx_tls;
    if (fx_tls.start(root, true)) {
        test_ftp_tls(fx_tls);
        fx_tls.stop();
        sh("rm -rf " + q(root + "-tls"));
    } else {
        printf("openssl is not usable: the TLS tests are skipped\n");
    }

    sh("rm -rf " + q(base));
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
