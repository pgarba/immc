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

#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
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
    return ARCH_NONE;
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

static bool path_has_tool(const char* name) {
    const char* path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p = path;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(':', i);
        if (j == std::string::npos) j = p.size();
        std::string cand = p.substr(i, j - i);
        if (cand.empty()) cand = ".";
        if (cand.back() != '/') cand += '/';
        cand += name;
        if (access(cand.c_str(), X_OK) == 0) return true;
        i = j + 1;
        if (j == p.size()) break;
    }
    return false;
}

bool arch_tool_available(ArchKind k) {
    static std::map<ArchKind, bool> cache;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;

    bool ok = false;
    switch (k) {
    case ARCH_ZIP:     ok = path_has_tool("unzip"); break;
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
    case ARCH_ZIP:     return path_has_tool("zip");
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ:  return path_has_tool("tar");
    case ARCH_7Z:      return path_has_tool("7z") || path_has_tool("7za");
    default:           return false;   // rar is read-only
    }
}

bool arch_can_extract(ArchKind k) { return arch_tool_available(k); }

// ---------------------------------------------------------------------------
// process runner
// ---------------------------------------------------------------------------

struct Proc {
    pid_t pid = -1;
    int fd = -1;
};

static bool proc_start(const std::vector<std::string>& argv, const char* cwd,
                       Proc* p, std::string* err) {
    int pfd[2];
    if (pipe(pfd) != 0) { *err = std::strerror(errno); return false; }
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        *err = std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);   // errors belong in the same stream
        close(pfd[0]); close(pfd[1]);
        if (cwd && *cwd && chdir(cwd) != 0) _exit(127);
        std::vector<char*> cs;
        for (const std::string& s : argv) cs.push_back(const_cast<char*>(s.c_str()));
        cs.push_back(nullptr);
        execvp(cs[0], cs.data());
        _exit(127);                    // exec failed: 127 is the shell's code for it
    }
    close(pfd[1]);
    p->pid = pid;
    p->fd = pfd[0];
    return true;
}

static int proc_finish(Proc* p, bool aborted) {
    if (p->pid < 0) return -1;
    if (aborted) {
        kill(p->pid, SIGTERM);
        // Give the tool a moment to clean up after itself, then stop insisting.
        for (int i = 0; i < 20; i++) {
            int st = 0;
            if (waitpid(p->pid, &st, WNOHANG) == p->pid) { p->pid = -1; break; }
            usleep(50000);
        }
        if (p->pid > 0) { kill(p->pid, SIGKILL); waitpid(p->pid, nullptr, 0); p->pid = -1; }
    } else {
        int st = 0;
        waitpid(p->pid, &st, 0);
        p->pid = -1;
        if (WIFEXITED(st))  return WEXITSTATUS(st);
        if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    }
    return -1;
}

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
// -1 if the process could not be started. `out` collects everything printed.
static int run_stream(const std::vector<std::string>& argv, const char* cwd,
                      Progress* pr, std::string* out, std::string* err) {
    Proc p;
    if (!proc_start(argv, cwd, &p, err)) return -1;

    std::string pending;   // partial line across reads
    char buf[4096];
    bool aborted = false;

    for (;;) {
        struct pollfd pf;
        pf.fd = p.fd;
        pf.events = POLLIN;
        pf.revents = 0;
        int r = poll(&pf, 1, 100);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            // Nothing to read: keep the dialog alive so a long single-file
            // operation does not look like a freeze.
            unsigned long long t = now_ms();
            if (t - pr->last_tick_ms >= 50) {
                pr->last_tick_ms = t;
                if (!pr->emit()) { aborted = true; break; }
            }
            continue;
        }
        ssize_t n = read(p.fd, buf, sizeof(buf));
        if (n > 0) {
            tail_append(out, buf, (size_t)n);
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
        } else if (n == 0) {
            break;                      // EOF: the tool is done
        } else {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
    }
    if (!pending.empty()) { feed(pr, pending); pending.clear(); }
    if (!aborted) { pr->done_bytes = pr->total_bytes; pr->done_files = pr->total_files; }
    close(p.fd);
    int st = proc_finish(&p, aborted);
    pr->aborted = aborted;
    if (aborted) *err = "aborted";
    return st;
}

// Run a tool and collect all of its output. Used for listings, which are fast
// enough not to need progress.
static int run_capture(const std::vector<std::string>& argv, const char* cwd,
                       std::string* out, std::string* err) {
    Progress pr;
    pr.mode = UNIT_ANY;
    return run_stream(argv, cwd, &pr, out, err);
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
        i = nl + 1;
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
static bool parse_tar_list(const std::string& out, std::vector<ArchEntry>* out_entries) {
    for (const std::string& line : split_lines(out)) {
        if (line.size() < 12) continue;
        char kind = line[0];
        if (!strchr("-dlpcbs", kind)) continue;              // header chatter
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
    case ARCH_ZIP: parsed = parse_unzip_list(out_text, out); break;
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
    case ARCH_ZIP: argv = { "unzip", "-l", a }; break;
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
    case ARCH_ZIP: pr.mode = UNIT_ANY; argv = { "unzip", "-o", a, "-d", d }; break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: pr.mode = UNIT_ANY; argv = { "tar", "-xvf", a, "-C", d }; break;
    case ARCH_7Z:
    case ARCH_RAR: pr.mode = UNIT_PERCENT; argv = { "7z", "x", a, "-o" + d, "-y" }; break;
    default: return false;
    }

    std::string out;
    int st = run_stream(argv, nullptr, &pr, &out, err);
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

bool arch_extract_members(const char* archive, const char* dest,
                          const std::vector<std::string>& raw_members,
                          arch_progress_fn cb, void* user, std::string* err) {
    ArchKind k = arch_kind_of(archive);
    if (k == ARCH_NONE || !arch_can_extract(k)) { *err = "cannot extract this archive"; return false; }
    if (raw_members.empty()) { *err = "nothing to extract"; return false; }

    std::vector<ArchEntry> members;
    std::string lerr;
    arch_list(archive, &members, &lerr);

    Progress pr;
    pr.cb = cb;
    pr.user = user;
    fill_totals(members, &pr.total_bytes, &pr.total_files);
    pr.sizes = size_order(members, &raw_members);
    pr.total_bytes = 0;
    pr.total_files = (int)pr.sizes.size();
    for (off_t s : pr.sizes) pr.total_bytes += (long long)s;

    std::string a(archive), d(dest);
    std::vector<std::string> argv;
    switch (k) {
    case ARCH_ZIP:
        // unzip takes the member list between the archive and "-d dest".
        pr.mode = UNIT_ANY;
        argv = { "unzip", "-o", a };
        argv.insert(argv.end(), raw_members.begin(), raw_members.end());
        argv.push_back("-d");
        argv.push_back(d);
        break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ:
        pr.mode = UNIT_ANY;
        argv = { "tar", "-xvf", a, "-C", d, "--" };
        argv.insert(argv.end(), raw_members.begin(), raw_members.end());
        break;
    case ARCH_7Z:
    case ARCH_RAR:
        pr.mode = UNIT_PERCENT;
        argv = { "7z", "x", a, "-o" + d, "-y", "--" };
        argv.insert(argv.end(), raw_members.begin(), raw_members.end());
        break;
    default: return false;
    }

    std::string out;
    int st = run_stream(argv, nullptr, &pr, &out, err);
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
    ArchKind k = arch_kind_of(archive);
    if (k == ARCH_NONE) { *err = "choose a name ending in .zip, .tar, .tar.gz, .tar.bz2, .tar.xz or .7z"; return false; }
    if (!arch_can_pack(k)) { *err = std::string(arch_kind_name(k)) + " cannot be written (read-only format)"; return false; }
    if (rel_sources.empty()) { *err = "nothing to pack"; return false; }

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
    switch (k) {
    case ARCH_ZIP: pr.mode = UNIT_MARKERED; pr.markers = { "adding:", "updating:", "freshening:" };
                   argv = { "zip", "-r", a }; break;
    case ARCH_TAR:
    case ARCH_TAR_GZ:
    case ARCH_TAR_BZ2:
    case ARCH_TAR_XZ: pr.mode = UNIT_ANY; argv = { "tar", "-cvf", a }; break;
    case ARCH_7Z: pr.mode = UNIT_PERCENT; argv = { "7z", "a", a }; break;
    default: return false;
    }
    argv.insert(argv.end(), rel_sources.begin(), rel_sources.end());

    std::string out;
    int st = run_stream(argv, cwd, &pr, &out, err);
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
    case ARCH_ZIP: pr.mode = UNIT_MARKERED; pr.markers = { "testing:" };
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
    int st = run_stream(argv, nullptr, &pr, &out, err);
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
    char tmpl[] = "/tmp/mc-clone-arch-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { *err = std::strerror(errno); return false; }
    snprintf(out_dir, dcap, "%s", dir);
    std::vector<std::string> one{ raw_member };
    if (!arch_extract_members(archive, dir, one, nullptr, nullptr, err)) {
        // Clean up the half-made directory before giving up.
        run_capture({ "rm", "-rf", std::string(dir) }, nullptr, err, err);
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
