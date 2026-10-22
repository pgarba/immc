// Archive support without an archive library: every operation drives a
// command-line tool (zip/unzip, tar, 7z) through a pipe, so the app keeps the
// same dependency shape it already has for `gio trash` and `xdg-open`.
//
// Two things make the shell-out worth doing properly rather than with popen():
// member names are passed as argv, so a name with a space, a quote or a glob
// character cannot change what the tool does; and holding the pipe open lets
// the per-member output lines drive the existing progress dialog instead of
// blocking the UI until the tool exits.

#include "archive.h"
#include "proc.h"
#include "platform.h"

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

// ---------------------------------------------------------------------------
// kinds
// ---------------------------------------------------------------------------

struct KindSuffix { const char* suffix; ArchKind kind; };
// Longest first: ".tar.gz" has to win over ".gz".
static const KindSuffix KINDS[] = {
    { ".tar.gz",  ARCH_TAR_GZ  },
    { ".tar.bz2", ARCH_TAR_BZ2 },
    { ".tar.xz",  ARCH_TAR_XZ  },
    { ".tbz2",    ARCH_TAR_BZ2 },
    { ".tgz",     ARCH_TAR_GZ  },
    { ".txz",     ARCH_TAR_XZ  },
    { ".tar",     ARCH_TAR     },
    { ".taz",     ARCH_TAR     },
    { ".zip",     ARCH_ZIP     },
    { ".7z",      ARCH_7Z      },
    { ".rar",     ARCH_RAR     },
    { ".r00",     ARCH_RAR     },
};

const char* arch_kind_name(ArchKind k) {
    switch (k) {
    case ARCH_ZIP:     return "zip";
    case ARCH_TAR:     return "tar";
    case ARCH_TAR_GZ:  return "tar.gz";
    case ARCH_TAR_BZ2: return "tar.bz2";
    case ARCH_TAR_XZ:  return "tar.xz";
    case ARCH_7Z:      return "7z";
    case ARCH_RAR:     return "rar";
    default:           return "archive";
    }
}

static std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

ArchKind arch_kind_of(const char* path) {
    if (!path) return ARCH_NONE;
    std::string p = lower(path);
    for (const KindSuffix& k : KINDS) {
        size_t n = strlen(k.suffix);
        if (p.size() >= n && p.compare(p.size() - n, n, k.suffix) == 0) return k.kind;
    }
    // A split archive names its parts after the whole: "movies.7z.001". Those
    // digits are a part number, not a format, so drop them and ask again. This
    // is what lets the browser and the extractor read back a volume set the
    // pack dialog just made.
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= p.size()) return ARCH_NONE;
    for (size_t i = dot + 1; i < p.size(); i++)
        if (!isdigit((unsigned char)p[i])) return ARCH_NONE;
    return arch_kind_of(p.substr(0, dot).c_str());
}

bool arch_is_archive(const char* path) { return arch_kind_of(path) != ARCH_NONE; }

// The tool that handles a kind. rar has no writer, and 7z carries a bundled
// Rar/Rar5 decoder, so rar needs no parser of its own.
static const char* tool_for(ArchKind k) {
    switch (k) {
    case ARCH_ZIP:
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ:  return nullptr;   // handled per-op: zip/unzip/tar
    case ARCH_7Z:
    case ARCH_RAR:     return "7z";
    default:           return nullptr;
    }
}

#ifdef _WIN32
static const char kPathSep = ';';
#else
static const char kPathSep = ':';
#endif

static bool path_has_tool(const char* name) {
    const char* path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p = path;
#ifdef _WIN32
    // A program on Windows is found by its name plus one of the extensions the shell
    // runs: "7z" is 7z.exe.
    const char* pathext = getenv("PATHEXT");
    std::vector<std::string> exts;
    std::string pe = (pathext && *pathext) ? pathext : ".COM;.EXE;.BAT;.CMD";
    for (size_t a = 0; a <= pe.size();) {
        size_t b = pe.find(';', a);
        if (b == std::string::npos) b = pe.size();
        if (b > a) exts.push_back(pe.substr(a, b - a));
        a = b + 1;
    }
#else
    const std::vector<std::string> exts = { "" };
#endif
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(kPathSep, i);
        if (j == std::string::npos) j = p.size();
        std::string cand = p.substr(i, j - i);
        if (cand.empty()) cand = ".";
        if (cand.back() != '/' && cand.back() != '\\') cand += '/';
        cand += name;
        for (const std::string& e : exts)
            if (access((cand + e).c_str(), X_OK) == 0) return true;
        i = j + 1;
        if (j == p.size()) break;
    }
    return false;
}

// Windows has no zip or unzip, but every Windows since 10 has bsdtar in System32, and
// libarchive under it reads and writes zip. So there a zip is listed, unpacked, packed
// and tested by tar, with tar's command lines and tar's listing - the same tool on
// every machine, rather than whichever unzip happened to be on PATH.
#ifdef _WIN32
static const bool kZipByTar = true;
#else
static const bool kZipByTar = false;
#endif

bool arch_tool_available(ArchKind k) {
    static std::map<ArchKind, bool> cache;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;

    bool ok = false;
    switch (k) {
    case ARCH_ZIP:     ok = path_has_tool(kZipByTar ? "tar" : "unzip"); break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ:  ok = path_has_tool("tar"); break;
    case ARCH_7Z:
    case ARCH_RAR:     ok = path_has_tool("7z") || path_has_tool("7za"); break;
    default:           ok = false; break;
    }
    cache[k] = ok;
    return ok;
}

bool arch_can_pack(ArchKind k) {
    switch (k) {
    case ARCH_ZIP:     return path_has_tool(kZipByTar ? "tar" : "zip");
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ:  return path_has_tool("tar");
    case ARCH_7Z:      return path_has_tool("7z") || path_has_tool("7za");
    default:           return false;   // rar is read-only
    }
}

bool arch_can_extract(ArchKind k) { return arch_tool_available(k); }

const char* arch_kind_suffix(ArchKind k) {
    switch (k) {
    case ARCH_ZIP:     return ".zip";
    case ARCH_TAR:     return ".tar";
    case ARCH_TAR_GZ:  return ".tar.gz";
    case ARCH_TAR_BZ2: return ".tar.bz2";
    case ARCH_TAR_XZ:  return ".tar.xz";
    case ARCH_7Z:      return ".7z";
    default:           return "";
    }
}

// What a pack dialog should offer, in the order it reads naturally: the two
// formats that can carry encryption and options first, then the tar family.
// RAR is deliberately absent - there is no writer for it on a normal system.
const std::vector<ArchKind>& arch_pack_kinds() {
    static const std::vector<ArchKind> k = { ARCH_ZIP, ARCH_7Z, ARCH_TAR_GZ,
                                             ARCH_TAR_XZ, ARCH_TAR_BZ2, ARCH_TAR };
    return k;
}

ArchPackOpts arch_pack_opts_default(ArchKind k) {
    ArchPackOpts o;
    o.kind = k;
    o.level = -1;
    o.password.clear();
    o.volume_bytes = 0;
    return o;
}

bool arch_pack_supported(const ArchPackOpts& opts, std::string* why) {
    const bool is_tar = opts.kind == ARCH_TAR || opts.kind == ARCH_TAR_GZ ||
                        opts.kind == ARCH_TAR_BZ2 || opts.kind == ARCH_TAR_XZ;
    if (!arch_can_pack(opts.kind)) {
        *why = std::string(arch_kind_name(opts.kind)) + " cannot be written (read-only format)";
        return false;
    }
    if (opts.level < -1 || opts.level > 9) {
        *why = "compression level must be between 0 and 9";
        return false;
    }
    if (opts.level >= 0 && opts.kind == ARCH_TAR) {
        *why = "a plain .tar is not compressed, so a level means nothing";
        return false;
    }
    if (!opts.password.empty() && opts.kind == ARCH_ZIP && kZipByTar) {
        *why = "zip is written by tar here, which cannot encrypt; choose 7z for a password";
        return false;
    } else if (!opts.password.empty() && !is_tar) {
        // fine for zip/7z
    } else if (!opts.password.empty() && is_tar) {
        *why = "tar cannot encrypt; choose zip or 7z for a password";
        return false;
    }
    if (opts.volume_bytes > 0 && opts.kind != ARCH_7Z) {
        *why = "only 7z writes volumes the tools can read back; "
               "a split .zip comes out unreadable by unzip";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// process runner: Proc, proc_start and proc_finish live in src/proc.cpp, which
// the console uses too. What is local here is only how a tool's output is read.
// ---------------------------------------------------------------------------

// Keep the tail of a tool's output for the status line.
static void tail_append(std::string* tail, const char* s, size_t n) {
    tail->append(s, n);
    if (tail->size() > 2000) tail->erase(0, tail->size() - 2000);
}

// Collapse a tool's output to the one line that explains a failure.
static std::string last_meaningful_line(const std::string& out) {
    size_t end = out.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return {};
    std::string s = out.substr(0, end + 1);
    size_t nl = s.rfind('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(nl + 1);
    size_t a = line.find_first_not_of(" \t");
    if (a != std::string::npos) line.erase(0, a);
    size_t b = line.find_last_not_of(" \t");
    if (b != std::string::npos) line.resize(b + 1);
    return line;
}

// ---------------------------------------------------------------------------
// progress accounting
// ---------------------------------------------------------------------------

// How a tool's output maps onto "one unit of work".
enum UnitMode {
    UNIT_ANY,       // every non-empty line is one member (tar -xvf, zip -r)
    UNIT_MARKERED,  // only lines carrying one of these markers count
    UNIT_PERCENT,   // the tool reports a percentage instead of members (7z)
};

struct Progress {
    arch_progress_fn cb = nullptr;
    void* user = nullptr;
    long long total_bytes = 0;
    long long done_bytes = 0;
    int total_files = 0;
    int done_files = 0;
    UnitMode mode = UNIT_ANY;
    std::vector<const char*> markers;
    int last_percent = -1;
    std::vector<off_t> sizes;      // member sizes in listing order
    size_t size_cursor = 0;
    bool aborted = false;
    unsigned long long last_tick_ms = 0;

    bool emit() {
        if (!cb) return true;
        return cb(done_bytes, total_bytes, done_files, total_files, user);
    }
};

// Monotonic milliseconds, for the idle-tick budget. Deliberately not SDL's
// clock: this runs in code the headless tests link without an SDL context.
static unsigned long long now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
}

// Extract the last "NN%" from a chunk of 7z progress output.
static int percent_in(const std::string& s) {
    int best = -1;
    size_t i = 0;
    while ((i = s.find('%', i)) != std::string::npos) {
        int j = (int)i - 1;
        while (j >= 0 && isdigit((unsigned char)s[j])) j--;
        if (j + 1 < (int)i) {
            int v = atoi(s.c_str() + j + 1);
            if (v >= 0 && v <= 100 && v > best) best = v;
        }
        i++;
    }
    return best;
}

static void feed(Progress* pr, const std::string& line) {
    if (line.empty()) return;
    switch (pr->mode) {
    case UNIT_PERCENT: {
        int p = percent_in(line);
        if (p > pr->last_percent) {
            pr->last_percent = p;
            pr->done_bytes = pr->total_bytes * p / 100;
            pr->done_files = pr->total_files * p / 100;
        }
        break;
    }
    case UNIT_ANY:
    case UNIT_MARKERED: {
        bool unit = pr->markers.empty();
        for (const char* m : pr->markers) {
            if (line.find(m) != std::string::npos) { unit = true; break; }
        }
        if (!unit) break;
        pr->done_files++;
        // The tools emit members in listing order, so the byte total can track
        // real sizes without trying to parse a name out of the line.
        if (pr->size_cursor < pr->sizes.size()) {
            pr->done_bytes += (long long)pr->sizes[pr->size_cursor];
            pr->size_cursor++;
        } else if (pr->total_files > 0) {
            pr->done_bytes = pr->total_bytes * pr->done_files / pr->total_files;
        }
        break;
    }
    }
}

// Run a tool, streaming its output through `pr`. Returns the exit status, or
// -1 if the process could not be started. `out` collects the tail of what was
// printed - enough for the line that explains a failure - or, with keep_all, every
// byte of it, which is what a listing needs.
static int run_stream(const std::vector<std::string>& argv, const char* cwd,
                      const std::vector<std::string>& env,
                      Progress* pr, std::string* out, std::string* err,
                      bool keep_all = false) {
    Proc p;
#ifdef _WIN32
    // 7-Zip writes names it cannot put in the console's code page as '_' unless told
    // to write UTF-8; the switch goes straight after the command letter, before any
    // "--" that ends the switches.
    std::vector<std::string> args = argv;
    if (args.size() >= 2 && args[0] == "7z") args.insert(args.begin() + 2, "-sccUTF-8");
    if (!proc_start(args, cwd, env, &p, err)) return -1;
#else
    if (!proc_start(argv, cwd, env, &p, err)) return -1;
#endif

    std::string pending;   // partial line across reads
    char buf[4096];
    bool aborted = false;

    for (;;) {
        long n = proc_read(&p, buf, sizeof(buf), 100);
        if (n == -1) {
            // Nothing to read: keep the dialog alive so a long single-file
            // operation does not look like a freeze.
            unsigned long long t = now_ms();
            if (t - pr->last_tick_ms >= 50) {
                pr->last_tick_ms = t;
                if (!pr->emit()) { aborted = true; break; }
            }
            continue;
        }
        if (n <= 0) break;              // EOF: the tool is done (or the pipe failed)
        if (keep_all) out->append(buf, (size_t)n);
        else tail_append(out, buf, (size_t)n);
        pending.append(buf, (size_t)n);
        // Tools separate progress updates with '\r' as well as '\n'.
        size_t nl;
        while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            size_t a = line.find_first_not_of(" \t");
            if (a != std::string::npos) line.erase(0, a);
            feed(pr, line);
        }
        if (!pr->emit()) { aborted = true; break; }
    }
    if (!pending.empty()) { feed(pr, pending); pending.clear(); }
    if (!aborted) { pr->done_bytes = pr->total_bytes; pr->done_files = pr->total_files; }
    int st = proc_finish(&p, aborted);
    pr->aborted = aborted;
    if (aborted) *err = "aborted";
#ifdef _WIN32
    // Windows' tar prints member names in the ANSI code page. The names handed back
    // to it go through its wide command line, which it turns back into that page.
    if (argv[0] == "tar") *out = plat_from_ansi(*out);
#endif
    return st;
}

// Run a tool and collect all of its output. Used for listings, which are fast
// enough not to need progress.
static int run_capture(const std::vector<std::string>& argv, const char* cwd,
                       std::string* out, std::string* err) {
    Progress pr;
    pr.mode = UNIT_ANY;
    return run_stream(argv, cwd, kNoEnv, &pr, out, err, true);
}

// ---------------------------------------------------------------------------
// listing parsers
// ---------------------------------------------------------------------------

static time_t parse_date_time(const char* date, const char* time_s) {
    struct tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(date, "%d-%d-%d", &y, &mo, &d) != 3) {
        // unzip on some builds prints MM-DD-YY
        if (sscanf(date, "%d-%d-%d", &mo, &d, &y) != 3) return 0;
        if (y < 100) y += 1900;
    }
    if (time_s && *time_s) sscanf(time_s, "%d:%d:%d", &h, &mi, &s);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t < 0 ? 0 : t;
}

static void set_name(ArchEntry* e, const std::string& raw) {
    snprintf(e->raw, sizeof(e->raw), "%s", raw.c_str());
    std::string rel = raw;
    if (rel.size() >= 2 && rel[0] == '.' && rel[1] == '/') rel = rel.substr(2);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();   // dirs end with '/'
    snprintf(e->rel, sizeof(e->rel), "%s", rel.c_str());
}

// Split a tool's output into lines. Progress updates arrive with '\r', so both
// separators end a line.
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t nl = s.find_first_of("\r\n", i);
        if (nl == std::string::npos) { out.push_back(s.substr(i)); break; }
        out.push_back(s.substr(i, nl - i));
        // "\r\n" is one line end, not a line end and an empty line: tools on Windows
        // end every line that way, and an empty line means something to the 7z reader.
        i = nl + ((s[nl] == '\r' && nl + 1 < s.size() && s[nl + 1] == '\n') ? 2 : 1);
    }
    return out;
}

// unzip -l:
//   Archive:  t.zip
//     Length      Date    Time    Name
//   ---------  ---------- -----   ----
//         6  2026-10-09 08:06   a file.txt
//         0  2026-10-09 08:06   sub/
static bool parse_unzip_list(const std::string& out, std::vector<ArchEntry>* out_entries) {
    bool in_rows = false;
    for (const std::string& raw_line : split_lines(out)) {
        std::string line = raw_line;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line.erase(0, a);
        if (!in_rows) {
            if (line.rfind("---------", 0) == 0) in_rows = true;
            continue;
        }
        if (line.rfind("---------", 0) == 0) break;          // the totals rule
        long long size = 0;
        char date[64] = "", tme[64] = "";
        int consumed = 0;
        if (sscanf(line.c_str(), "%lld %63s %63s %n", &size, date, tme, &consumed) < 3) continue;
        std::string name = line.substr(consumed);
        size_t b = name.find_last_not_of(" \t");
        if (b != std::string::npos) name.resize(b + 1);
        if (name.empty()) continue;
        ArchEntry e{};
        bool dir = !name.empty() && name.back() == '/';
        e.mode = (dir ? S_IFDIR : S_IFREG) | (dir ? 0755 : 0644);
        e.size = (off_t)size;
        e.mtime = parse_date_time(date, tme);
        set_name(&e, name);
        out_entries->push_back(e);
    }
    return !out_entries->empty();
}

// tar -tvf:
//   drwxr-xr-x adam/adam         0 2026-10-09 08:06 sub/
//   -rw-r--r-- adam/adam         6 2026-10-09 08:07 a file.txt
// bsdtar -tv, which is the tar on Windows and on the BSDs, writes ls -l instead:
//   -rw-r--r--  0 adam   staff       6 Oct  9 08:06 a file.txt
//   drwxr-xr-x  0 adam   staff       0 Oct  9  2025 sub/
// The time is a year for a date that is not recent. Returns false when the line is
// not in that shape, so the GNU reading gets its turn.
static bool parse_bsdtar_line(const std::string& line, ArchEntry* e) {
    std::vector<std::pair<size_t, std::string>> w;   // where each word starts, and the word
    size_t i = 0;
    while (i < line.size() && w.size() < 8) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
        if (i > start) w.push_back({ start, line.substr(start, i - start) });
    }
    if (w.size() < 8) return false;
    if (w[1].second.find_first_not_of("0123456789") != std::string::npos) return false;
    static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int mon = -1;
    for (int m = 0; m < 12; m++) if (w[5].second == months[m]) mon = m;
    if (mon < 0) return false;
    while (i < line.size() && line[i] == ' ') i++;   // exactly the separator, not the name's own
    if (i >= line.size()) return false;

    struct tm tm{};
    time_t now = time(nullptr);
    struct tm now_tm{};
    localtime_r(&now, &now_tm);
    tm.tm_mon = mon;
    tm.tm_mday = atoi(w[6].second.c_str());
    tm.tm_isdst = -1;
    const std::string& when = w[7].second;
    if (when.find(':') != std::string::npos) {
        // A recent date, and ls leaves the year out: it is this one, or last year's
        // when this year's would be in the future.
        tm.tm_year = now_tm.tm_year;
        tm.tm_hour = atoi(when.c_str());
        tm.tm_min = atoi(when.c_str() + when.find(':') + 1);
        struct tm probe = tm;
        if (mktime(&probe) > now + 86400) tm.tm_year--;
    } else {
        tm.tm_year = atoi(when.c_str()) - 1900;
    }
    time_t t = mktime(&tm);

    const std::string& mode_s = w[0].second;
    const bool dir = mode_s[0] == 'd';
    mode_t perm = 0;
    static const char* bits = "rwxrwxrwx";
    for (int b = 0; b < 9 && b + 1 < (int)mode_s.size(); b++)
        if (mode_s[b + 1] == bits[b]) perm |= (mode_t)(0400 >> b);
    *e = ArchEntry{};
    e->mode = (dir ? S_IFDIR : S_IFREG) | perm;
    e->size = (off_t)atoll(w[4].second.c_str());
    e->mtime = t < 0 ? 0 : t;
    std::string name = line.substr(i);
    // A link prints its target after the name; the name is what is in the archive.
    if (mode_s[0] == 'l') {
        size_t arrow = name.find(" -> ");
        if (arrow != std::string::npos) name.resize(arrow);
    }
    set_name(e, name);
    return true;
}

static bool parse_tar_list(const std::string& out, std::vector<ArchEntry>* out_entries) {
    for (const std::string& line : split_lines(out)) {
        if (line.size() < 12) continue;
        char kind = line[0];
        if (!strchr("-dlpcbs", kind)) continue;              // header chatter
        ArchEntry bsd;
        if (parse_bsdtar_line(line, &bsd)) {
            if (bsd.rel[0]) out_entries->push_back(bsd);
            continue;
        }
        std::istringstream ss(line);
        std::string mode_s, owner, size_s, date, fifth;
        ss >> mode_s >> owner >> size_s >> date >> fifth;
        if (size_s.empty() || date.empty()) continue;
        std::string time_s = fifth;
        // A member name can start right after the time; a far-future or very
        // old entry prints a year where the time goes.
        bool fifth_is_time = fifth.find(':') != std::string::npos;
        std::string rest;
        size_t pos = line.find(date);
        if (pos == std::string::npos) continue;
        pos += date.size();
        std::string after = line.substr(pos);
        size_t a = after.find_first_not_of(" \t");
        if (a != std::string::npos) after.erase(0, a);
        if (fifth_is_time) {
            // drop the time field, the name is what is left
            if (after.size() >= fifth.size() && after.compare(0, fifth.size(), fifth) == 0)
                after.erase(0, fifth.size());
            size_t b = after.find_first_not_of(" \t");
            if (b != std::string::npos) after.erase(0, b);
        } else {
            time_s.clear();
        }
        ArchEntry e{};
        bool dir = kind == 'd';
        mode_t perm = 0;
        static const char* bits = "rwxrwxrwx";
        for (int i = 0; i < 9 && i + 1 < (int)mode_s.size(); i++)
            if (mode_s[i + 1] == bits[i]) perm |= (mode_t)(0400 >> i);
        e.mode = (dir ? S_IFDIR : S_IFREG) | perm;
        e.size = (off_t)atoll(size_s.c_str());
        e.mtime = parse_date_time(date.c_str(), time_s.c_str());
        set_name(&e, after);
        if (e.rel[0]) out_entries->push_back(e);
    }
    return !out_entries->empty();
}

// 7z l -slt: blocks of "Key = value" after a "----------" rule.
//   Path = sub/b.log
//   Size = 2
//   Modified = 2026-10-09 08:06:49.4156983
//   Attributes = A -rw-r--r--
static bool parse_7z_list(const std::string& out, std::vector<ArchEntry>* out_entries) {
    bool in_rows = false;
    bool have = false;
    std::string path, size_s, modified, attrs;

    auto flush = [&]() {
        if (!have || path.empty()) { path.clear(); have = false; return; }
        ArchEntry e{};
        bool dir = !attrs.empty() && attrs[0] == 'D';
        e.mode = (dir ? S_IFDIR : S_IFREG) | (dir ? 0755 : 0644);
        e.size = (off_t)atoll(size_s.c_str());
        char d[64] = "", t[64] = "";
        sscanf(modified.c_str(), "%63s %63s", d, t);
        e.mtime = parse_date_time(d, t);
        set_name(&e, path);
#ifdef _WIN32
        // 7-Zip on Windows prints its own separator. The raw name stays the tool's
        // spelling, which is what it is handed back as; the row uses '/'.
        for (char* c = e.rel; *c; c++) if (*c == '\\') *c = '/';
#endif
        if (e.rel[0]) out_entries->push_back(e);
        path.clear(); size_s.clear(); modified.clear(); attrs.clear();
        have = false;
    };

    for (const std::string& line : split_lines(out)) {
        if (line.rfind("----------", 0) == 0) { in_rows = true; continue; }
        if (!in_rows) continue;
        if (line.empty()) { flush(); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        size_t a = key.find_first_not_of(" \t");
        if (a != std::string::npos) key.erase(0, a);
        size_t b = key.find_last_not_of(" \t");
        if (b != std::string::npos) key.resize(b + 1);
        size_t c = val.find_first_not_of(" \t");
        if (c != std::string::npos) val.erase(0, c);
        size_t d = val.find_last_not_of(" \t");
        if (d != std::string::npos) val.resize(d + 1);
        if (key == "Path") { flush(); path = val; have = true; }
        else if (key == "Size") size_s = val;
        else if (key == "Modified") modified = val;
        else if (key == "Attributes") attrs = val;
    }
    flush();
    return !out_entries->empty();
}

// A tool that lists files but never folders would leave the pane without any
// way to descend, so every intermediate component becomes a directory row.
static void synthesize_dirs(std::vector<ArchEntry>* entries) {
    std::set<std::string> seen;
    for (const ArchEntry& e : *entries) seen.insert(e.rel);
    std::vector<ArchEntry> added;
    for (const ArchEntry& e : *entries) {
        std::string p = e.rel;
        for (;;) {
            size_t slash = p.find_last_of('/');
            if (slash == std::string::npos) break;
            p = p.substr(0, slash);
            if (p.empty()) break;
            if (seen.insert(p).second) {
                ArchEntry d{};
                d.mode = S_IFDIR | 0755;
                d.size = 0;
                d.mtime = e.mtime;
                set_name(&d, p);
                added.push_back(d);
            }
        }
    }
    for (const ArchEntry& d : added) entries->push_back(d);
}

static bool arch_list_run(const std::vector<std::string>& argv, ArchKind k,
                          std::vector<ArchEntry>* out, std::string* err) {
    std::string out_text;
    int st = run_capture(argv, nullptr, &out_text, err);
    if (st == 127) { *err = std::string(tool_for(k) ? tool_for(k) : argv[0]) + " is not installed"; return false; }
    if (st != 0 && out_text.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "listing failed (exit %d)", st);
        *err = buf;
        return false;
    }
    bool parsed = false;
    switch (k) {
    case ARCH_ZIP: parsed = kZipByTar ? parse_tar_list(out_text, out)
                                      : parse_unzip_list(out_text, out); break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: parsed = parse_tar_list(out_text, out); break;
    case ARCH_7Z:
    case ARCH_RAR: parsed = parse_7z_list(out_text, out); break;
    default: break;
    }
    if (!parsed) {
        std::string why = last_meaningful_line(out_text);
        *err = why.empty() ? "unrecognized listing output" : why;
        return false;
    }
    synthesize_dirs(out);
    std::sort(out->begin(), out->end(), [](const ArchEntry& a, const ArchEntry& b) {
        return strcmp(a.rel, b.rel) < 0;
    });
    return true;
}

bool arch_list(const char* archive, std::vector<ArchEntry>* out, std::string* err) {
    out->clear();
    ArchKind k = arch_kind_of(archive);
    if (k == ARCH_NONE) { *err = "not an archive"; return false; }
    if (!arch_tool_available(k)) { *err = std::string("no tool installed for ") + arch_kind_name(k); return false; }

    std::string a(archive);
    std::vector<std::string> argv;
    switch (k) {
    case ARCH_ZIP: argv = kZipByTar ? std::vector<std::string>{ "tar", "-tvf", a }
                                    : std::vector<std::string>{ "unzip", "-l", a }; break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: argv = { "tar", "-tvf", a }; break;
    case ARCH_7Z:
    case ARCH_RAR: argv = { "7z", "l", "-slt", a }; break;
    default: *err = "not an archive"; return false;
    }
    return arch_list_run(argv, k, out, err);
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------

static void fill_totals(const std::vector<ArchEntry>& members, long long* bytes, int* files) {
    long long b = 0;
    int f = 0;
    for (const ArchEntry& m : members) {
        if (S_ISDIR(m.mode)) continue;
        b += (long long)m.size;
        f++;
    }
    *bytes = b;
    *files = f;
}

// Members to report progress against: the whole listing, or just the ones
// being extracted.
static std::vector<off_t> size_order(const std::vector<ArchEntry>& members,
                                     const std::vector<std::string>* wanted) {
    std::vector<off_t> out;
    for (const ArchEntry& m : members) {
        if (S_ISDIR(m.mode)) continue;
        if (wanted) {
            bool hit = false;
            for (const std::string& w : *wanted) if (w == m.raw) { hit = true; break; }
            if (!hit) continue;
        }
        out.push_back(m.size);
    }
    return out;
}

bool arch_extract(const char* archive, const char* dest,
                  arch_progress_fn cb, void* user, std::string* err) {
    ArchKind k = arch_kind_of(archive);
    if (k == ARCH_NONE || !arch_can_extract(k)) { *err = "cannot extract this archive"; return false; }

    std::vector<ArchEntry> members;
    std::string lerr;
    arch_list(archive, &members, &lerr);      // totals only; failure is not fatal

    Progress pr;
    pr.cb = cb;
    pr.user = user;
    fill_totals(members, &pr.total_bytes, &pr.total_files);
    pr.sizes = size_order(members, nullptr);

    std::string a(archive), d(dest);
    std::vector<std::string> argv;
    switch (k) {
    case ARCH_ZIP: pr.mode = UNIT_ANY;
                   argv = kZipByTar ? std::vector<std::string>{ "tar", "-xvf", a, "-C", d }
                                    : std::vector<std::string>{ "unzip", "-o", a, "-d", d };
                   break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: pr.mode = UNIT_ANY; argv = { "tar", "-xvf", a, "-C", d }; break;
    case ARCH_7Z:
    case ARCH_RAR: pr.mode = UNIT_PERCENT; argv = { "7z", "x", a, "-o" + d, "-y" }; break;
    default: return false;
    }

    std::string out;
    int st = run_stream(argv, nullptr, kNoEnv, &pr, &out, err);
    if (pr.aborted) return false;
    if (st == 127) { *err = "the archive tool is not installed"; return false; }
    if (st != 0) {
        std::string why = last_meaningful_line(out);
        char buf[80];
        snprintf(buf, sizeof(buf), "extraction failed (exit %d)", st);
        *err = why.empty() ? buf : why;
        return false;
    }
    return true;
}

// A member name as tar should match it. GNU tar matches the names it is given
// literally; bsdtar - the tar on Windows, which CreateProcess finds in System32 before
// anything on PATH - reads every one as a pattern, so "photo [1].jpg" would be a
// character class. A backslash makes each of its pattern characters itself again.
static std::string tar_member(const std::string& raw) {
#ifdef _WIN32
    std::string out;
    for (char c : raw) {
        if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') out += '\\';
        out += c;
    }
    return out;
#else
    return raw;
#endif
}

bool arch_extract_members(const char* archive, const char* dest,
                          const std::vector<std::string>& raw_members,
                          arch_progress_fn cb, void* user, std::string* err) {
    ArchKind k = arch_kind_of(archive);
    if (k == ARCH_NONE || !arch_can_extract(k)) { *err = "cannot extract this archive"; return false; }
    if (raw_members.empty()) { *err = "nothing to extract"; return false; }

    std::vector<ArchEntry> members;
    std::string lerr;
    arch_list(archive, &members, &lerr);

    // A directory row in the pane stands for a whole subtree, but the tools do
    // not agree about what naming that directory means: unzip matches names
    // literally and unpacks an empty directory, while tar recurses and then
    // reports "not found" for any child named alongside it. So the list handed
    // to the tool is the subtree's leaves: its files, plus any directory under
    // it that holds no files. Every tool writes those, creating the parent
    // directories on the way, and the per-file progress counts what is actually
    // written instead of one row per folder.
    std::vector<std::string> names;
    std::set<std::string> seen;
    auto push = [&](const std::string& raw) {
        if (seen.insert(raw).second) names.push_back(raw);
    };
    for (const std::string& want : raw_members) {
        const ArchEntry* hit = nullptr;
        for (const ArchEntry& m : members)
            if (want == m.raw) { hit = &m; break; }
        if (!hit || !S_ISDIR(hit->mode)) { push(want); continue; }
        std::string pre = std::string(hit->rel) + "/";
        size_t added = 0;
        for (const ArchEntry& m : members) {
            if (strncmp(m.rel, pre.c_str(), pre.size()) != 0) continue;
            if (!S_ISDIR(m.mode)) { push(m.raw); added++; continue; }
            std::string under = std::string(m.rel) + "/";
            bool has_child = false;
            for (const ArchEntry& c : members)
                if (strcmp(c.rel, m.rel) != 0 &&
                    strncmp(c.rel, under.c_str(), under.size()) == 0) { has_child = true; break; }
            if (!has_child) { push(m.raw); added++; }   // an empty folder still has to appear
        }
        if (added == 0) push(hit->raw);                 // the folder itself is empty
    }

    Progress pr;
    pr.cb = cb;
    pr.user = user;
    fill_totals(members, &pr.total_bytes, &pr.total_files);
    pr.sizes = size_order(members, &names);
    pr.total_bytes = 0;
    pr.total_files = (int)pr.sizes.size();
    for (off_t s : pr.sizes) pr.total_bytes += (long long)s;

    std::string a(archive), d(dest);
    std::vector<std::string> argv;
    switch (k) {
    case ARCH_ZIP:
        pr.mode = UNIT_ANY;
        if (kZipByTar) {
            argv = { "tar", "-xvf", a, "-C", d, "--" };
            for (const std::string& n : names) argv.push_back(tar_member(n));
            break;
        }
        // unzip takes the member list between the archive and "-d dest".
        argv = { "unzip", "-o", a };
        argv.insert(argv.end(), names.begin(), names.end());
        argv.push_back("-d");
        argv.push_back(d);
        break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ:
        pr.mode = UNIT_ANY;
        argv = { "tar", "-xvf", a, "-C", d, "--" };
        for (const std::string& n : names) argv.push_back(tar_member(n));
        break;
    case ARCH_7Z:
    case ARCH_RAR:
        pr.mode = UNIT_PERCENT;
        argv = { "7z", "x", a, "-o" + d, "-y", "--" };
        argv.insert(argv.end(), names.begin(), names.end());
        break;
    default: return false;
    }

    std::string out;
    int st = run_stream(argv, nullptr, kNoEnv, &pr, &out, err);
    if (pr.aborted) return false;
    if (st != 0) {
        std::string why = last_meaningful_line(out);
        char buf[80];
        snprintf(buf, sizeof(buf), "extraction failed (exit %d)", st);
        *err = why.empty() ? buf : why;
        return false;
    }
    return true;
}

bool arch_pack(const char* archive, const char* cwd,
               const std::vector<std::string>& rel_sources,
               arch_progress_fn cb, void* user, std::string* err) {
    return arch_pack_opts(archive, cwd, rel_sources,
                          arch_pack_opts_default(arch_kind_of(archive)),
                          cb, user, err);
}

bool arch_pack_opts(const char* archive, const char* cwd,
                    const std::vector<std::string>& rel_sources,
                    const ArchPackOpts& opts,
                    arch_progress_fn cb, void* user, std::string* err) {
    std::string why;
    if (!arch_pack_supported(opts, &why)) { *err = why; return false; }
    if (arch_kind_of(archive) != opts.kind) {
        *err = std::string("a ") + arch_kind_name(opts.kind) +
               " archive must be named with " + arch_kind_suffix(opts.kind);
        return false;
    }
    if (rel_sources.empty()) { *err = "nothing to pack"; return false; }
    const ArchKind k = opts.kind;

    // Rough totals so the bar has a denominator: stat the sources recursively.
    long long bytes = 0;
    int files = 0;
    for (const std::string& s : rel_sources) {
        std::string full = std::string(cwd) + "/" + s;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;
        if (S_ISREG(st.st_mode)) { bytes += st.st_size; files++; continue; }
        if (!S_ISDIR(st.st_mode)) continue;
        files++;
        std::vector<std::string> stack{ full };
        while (!stack.empty()) {
            std::string dir = stack.back();
            stack.pop_back();
            DIR* d = opendir(dir.c_str());
            if (!d) continue;
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                std::string child = dir + "/" + de->d_name;
                struct stat cs;
                if (lstat(child.c_str(), &cs) != 0) continue;
                if (S_ISREG(cs.st_mode)) { bytes += cs.st_size; files++; }
                else if (S_ISDIR(cs.st_mode)) { files++; stack.push_back(child); }
            }
            closedir(d);
        }
    }

    Progress pr;
    pr.cb = cb;
    pr.user = user;
    pr.total_bytes = bytes;
    pr.total_files = files;

    std::string a(archive);
    std::vector<std::string> argv;
    std::vector<std::string> env;
    switch (k) {
    case ARCH_ZIP: if (kZipByTar) {
                       // -a: the format comes from the name, and libarchive takes the
                       // level as one of its own options rather than a flag.
                       pr.mode = UNIT_ANY;
                       argv = { "tar", "-a", "-cvf", a };
                       if (opts.level >= 0) {
                           argv.push_back("--options");
                           argv.push_back(opts.level == 0 ? std::string("zip:compression=store")
                                          : "zip:compression-level=" + std::to_string(opts.level));
                       }
                       break;
                   }
                   pr.mode = UNIT_MARKERED; pr.markers = { "adding:", "updating:", "freshening:" };
                   argv = { "zip", "-r", a };
                   // zip's own interactive prompt (-e) needs a terminal, which a
                   // GUI process does not have, so the password goes on the
                   // command line. That makes it visible in the process list for
                   // the length of the pack; the dialog says so.
                   if (opts.level >= 0) argv.push_back("-" + std::to_string(opts.level));
                   if (!opts.password.empty()) { argv.push_back("-P"); argv.push_back(opts.password); }
                   break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: {
        pr.mode = UNIT_ANY;
        // -a: compress the way the name says. Without it a .tar.gz is written as a
        // plain tar that only happens to be called .gz.
        argv = { "tar", "-a", "-cvf", a };
        if (opts.level >= 0) {
#ifdef _WIN32
            // bsdtar compresses in-process, so the level is a libarchive option.
            const char* filter = k == ARCH_TAR_GZ ? "gzip" : k == ARCH_TAR_BZ2 ? "bzip2" : "xz";
            argv.push_back("--options");
            argv.push_back(std::string(filter) + ":compression-level=" + std::to_string(opts.level));
#else
            // tar hands the stream to gzip/bzip2/xz; those read their level from the
            // environment, which keeps it out of the argument vector.
            const char* var = k == ARCH_TAR_GZ ? "GZIP" : k == ARCH_TAR_BZ2 ? "BZIP2" : "XZ_OPT";
            env.push_back(std::string(var) + "=-" + std::to_string(opts.level));
#endif
        }
        break;
    }
    case ARCH_7Z:  pr.mode = UNIT_PERCENT;
                   argv = { "7z", "a", a };
                   if (opts.level >= 0) argv.push_back("-mx" + std::to_string(opts.level));
                   if (!opts.password.empty()) argv.push_back("-p" + opts.password);
                   if (opts.volume_bytes > 0) argv.push_back("-v" + std::to_string(opts.volume_bytes));
                   break;
    default: return false;
    }
    argv.insert(argv.end(), rel_sources.begin(), rel_sources.end());

    std::string out;
    int st = run_stream(argv, cwd, env, &pr, &out, err);
    if (pr.aborted) return false;
    if (st == 127) { *err = "the archive tool is not installed"; return false; }
    if (st != 0) {
        std::string why = last_meaningful_line(out);
        char buf[80];
        snprintf(buf, sizeof(buf), "packing failed (exit %d)", st);
        *err = why.empty() ? buf : why;
        return false;
    }
    return true;
}

bool arch_test(const char* archive, arch_progress_fn cb, void* user, std::string* err) {
    ArchKind k = arch_kind_of(archive);
    if (k == ARCH_NONE || !arch_tool_available(k)) { *err = "cannot test this archive"; return false; }

    std::vector<ArchEntry> members;
    std::string lerr;
    arch_list(archive, &members, &lerr);

    Progress pr;
    pr.cb = cb;
    pr.user = user;
    fill_totals(members, &pr.total_bytes, &pr.total_files);
    pr.sizes = size_order(members, nullptr);

    std::string a(archive);
    std::vector<std::string> argv;
    switch (k) {
    case ARCH_ZIP: if (kZipByTar) { pr.mode = UNIT_ANY; argv = { "tar", "-tvf", a }; break; }
                   pr.mode = UNIT_MARKERED; pr.markers = { "testing:" };
                   argv = { "unzip", "-t", a }; break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: pr.mode = UNIT_ANY; argv = { "tar", "-tvf", a }; break;
    case ARCH_7Z:
    case ARCH_RAR: pr.mode = UNIT_PERCENT; argv = { "7z", "t", a, "-y" }; break;
    default: return false;
    }

    std::string out;
    int st = run_stream(argv, nullptr, kNoEnv, &pr, &out, err);
    if (pr.aborted) return false;
    if (st != 0) {
        std::string why = last_meaningful_line(out);
        char buf[80];
        snprintf(buf, sizeof(buf), "integrity test failed (exit %d)", st);
        *err = why.empty() ? buf : why;
        return false;
    }
    return true;
}

bool arch_extract_temp(const char* archive, const char* raw_member,
                       char* out_path, size_t cap,
                       char* out_dir, size_t dcap, std::string* err) {
    std::string tmpl_s = plat_temp_dir() + "/mc-clone-arch-XXXXXX";
    std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end());
    tmpl.push_back('\0');
    const char* dir = mkdtemp(tmpl.data());
    if (!dir) { *err = std::strerror(errno); return false; }
    snprintf(out_dir, dcap, "%s", dir);
    std::vector<std::string> one{ raw_member };
    if (!arch_extract_members(archive, dir, one, nullptr, nullptr, err)) {
        // Clean up the half-made directory before giving up.
#ifdef _WIN32
        std::string win_dir = dir;
        for (char& c : win_dir) if (c == '/') c = '\\';
        run_capture({ "cmd", "/d", "/c", "rd /s /q \"" + win_dir + "\"" }, nullptr, err, err);
#else
        run_capture({ "rm", "-rf", std::string(dir) }, nullptr, err, err);
#endif
        out_dir[0] = '\0';
        return false;
    }
    snprintf(out_path, cap, "%s/%s", dir, raw_member);
    if (access(out_path, R_OK) != 0) {
        *err = "the extracted member is missing";
        return false;
    }
    return true;
}

bool arch_split_virtual(const char* path, char* archive, size_t acap,
                        char* prefix, size_t pcap) {
    if (!path || !*path) return false;
    std::string p(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();

    size_t i = 0;
    while (i < p.size()) {
        size_t slash = p.find('/', i);
        size_t end = (slash == std::string::npos) ? p.size() : slash;
        std::string head = p.substr(0, end);
        struct stat st;
        // Only a regular file with an archive extension opens as an archive.
        if (stat(head.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            arch_kind_of(head.c_str()) != ARCH_NONE) {
            snprintf(archive, acap, "%s", head.c_str());
            std::string rest = (end == p.size()) ? std::string() : p.substr(end + 1);
            snprintf(prefix, pcap, "%s", rest.c_str());
            return true;
        }
        if (end == p.size()) break;
        i = end + 1;
    }
    return false;
}

const ArchEntry* arch_find(const std::vector<ArchEntry>& listing, const char* rel) {
    if (!rel) return nullptr;
    for (const ArchEntry& e : listing) if (!strcmp(e.rel, rel)) return &e;
    return nullptr;
}
