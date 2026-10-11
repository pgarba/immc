// Remote sites over libcurl. See remote.h for why this one backend links a
// library while archives shell out to CLI tools: a password in argv is readable
// through /proc, and a login plus a TLS handshake per directory listing makes
// browsing unusable. Everything here is plain C++ over the curl easy interface;
// the UI never touches a curl type.
//
// The build without libcurl is a first-class configuration, not a stub: the
// protocol vocabulary, the URL and virtual-path plumbing, the sites file and
// every listing parser still work, so the dialogs and the tests behave the same
// and only the transport says "this build has no FTP support".

#include "remote.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef MC_HAVE_CURL
#include <curl/curl.h>
#endif

// ---------------------------------------------------------------------------
// Protocol vocabulary
// ---------------------------------------------------------------------------

const char* net_proto_name(NetProto p) {
    switch (p) {
        case NET_FTP:           return "FTP";
        case NET_FTPS:          return "FTPS";
        case NET_FTPS_IMPLICIT: return "FTPS (implicit)";
        case NET_SFTP:          return "SFTP";
        default:                return "?";
    }
}

const char* net_proto_key(NetProto p) {
    switch (p) {
        case NET_FTP:           return "ftp";
        case NET_FTPS:          return "ftps";
        case NET_FTPS_IMPLICIT: return "ftps-implicit";
        case NET_SFTP:          return "sftp";
        default:                return "ftp";
    }
}

const char* net_proto_scheme(NetProto p) {
    switch (p) {
        case NET_SFTP:          return "sftp://";
        case NET_FTPS:          return "ftps://";
        case NET_FTPS_IMPLICIT: return "ftps://";
        default:                return "ftp://";
    }
}

int net_proto_port(NetProto p) {
    switch (p) {
        case NET_FTPS_IMPLICIT: return 990;
        case NET_SFTP:          return 22;
        case NET_FTPS:
        case NET_FTP:
        default:                return 21;
    }
}

bool net_proto_of(const char* name, NetProto* out) {
    if (!name) return false;
    if (!strcmp(name, "ftp"))            { *out = NET_FTP;           return true; }
    if (!strcmp(name, "ftps"))           { *out = NET_FTPS;          return true; }
    if (!strcmp(name, "ftps-implicit"))  { *out = NET_FTPS_IMPLICIT; return true; }
    if (!strcmp(name, "sftp"))           { *out = NET_SFTP;          return true; }
    if (!strcmp(name, "ssh"))            { *out = NET_SFTP;          return true; }
    return false;
}

int net_site_port(const NetSite& s) { return s.port > 0 ? s.port : net_proto_port(s.proto); }

// ---------------------------------------------------------------------------
// URLs and the virtual path a pane holds
// ---------------------------------------------------------------------------

std::string net_site_root(const NetSite& s) {
    std::string out = net_proto_scheme(s.proto);
    if (!s.user.empty()) out += s.user + "@";
    // An IPv6 literal needs brackets or the port separator becomes ambiguous.
    if (s.host.find(':') != std::string::npos) out += "[" + s.host + "]";
    else out += s.host;
    // A port that is the protocol's own default is left out. The root is a lookup
    // key as much as a label - it finds the saved site for a pane's path and keys
    // the cached handle - so "typed", "saved" and "defaulted" have to agree.
    if (s.port > 0 && s.port != net_proto_port(s.proto)) out += ":" + std::to_string(s.port);
    return out;
}

std::string net_virtual_path(const std::string& root, const std::string& rpath) {
    if (rpath.empty() || rpath == "/") return root + "/";
    if (rpath[0] != '/') return root + "/" + rpath;
    return root + rpath;
}

// Join a server directory and a name. The root arrives as "" or as "/" depending
// on which code produced it, and concatenating either with a "/" of your own
// gives "//name", which some servers read as a directory literally named "".
std::string net_join_remote(const std::string& dir, const std::string& name) {
    if (name.empty()) return dir;
    if (dir.empty() || dir == "/") return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

// Percent-decode one component of a URL. Only the three characters that can
// appear in what a user types are decoded; anything else is left as written, so
// a host name that legitimately contains a percent sign is not mangled.
std::string net_url_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], 0 };
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Percent-encode the bytes a URL path may not carry. A name on a server is
// bytes, and a space, a '#' or a '%' in one is ordinary. In a URL they are not:
// a space makes curl answer "URL rejected: Malformed input to a URL function"
// before it has opened a socket, and a '#' ends the path where the name has not.
// The pane that walked into "IDA Pro 9.6.261001" was left holding an empty
// listing and a message about a filter it had never set. curl decodes what it
// reads back, so the server is given the bytes it was asked for. Only the
// unreserved set of RFC 3986 stays plain, which keeps the encoding the one thing
// doing the work; '/' separates components when the string is a path.
std::string net_url_escape(const std::string& s, bool keep_slash) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' || c == '~';
        if (!keep && keep_slash && c == '/') keep = true;
        if (keep) { out += (char)c; continue; }
        out += '%';
        out += kHex[(c >> 4) & 0xF];
        out += kHex[c & 0xF];
    }
    return out;
}

// The scheme prefix of a path, if it has one.
static const struct { const char* scheme; NetProto proto; } kSchemes[] = {
    { "ftp://",  NET_FTP  },
    { "ftps://", NET_FTPS },
    { "sftp://", NET_SFTP },
    { "ssh://",  NET_SFTP },
};

static bool scheme_of(const char* path, NetProto* proto, const char** rest) {
    for (const auto& s : kSchemes) {
        size_t n = strlen(s.scheme);
        if (!strncasecmp(path, s.scheme, n)) { *proto = s.proto; *rest = path + n; return true; }
    }
    return false;
}

bool net_is_remote(const char* path) {
    if (!path) return false;
    NetProto p;
    const char* rest = nullptr;
    return scheme_of(path, &p, &rest) && *rest != '\0';
}

// Split "scheme://[user@]host[:port]" from the path that follows it. The root is
// everything up to and including the authority, the path is the rest with its
// leading '/' kept.
static bool split_authority(const char* after_scheme, std::string* authority,
                            std::string* rpath) {
    const char* p = after_scheme;
    bool in_brackets = false;
    for (; *p; p++) {
        if (*p == '[') in_brackets = true;
        else if (*p == ']') in_brackets = false;
        else if (*p == '/' && !in_brackets) break;
    }
    authority->assign(after_scheme, (size_t)(p - after_scheme));
    rpath->assign(p);
    while (rpath->size() > 1 && rpath->back() == '/') rpath->pop_back();   // trim, keep "/"
    return !authority->empty();
}

// "user:pw@host:21" -> "user@host:21". The root is what a title bar, a tab
// label and a path bar show, so a password never survives it.
static std::string strip_password_authority(const std::string& authority) {
    size_t at = authority.rfind('@');
    if (at == std::string::npos) return authority;
    std::string creds = authority.substr(0, at);
    size_t colon = creds.find(':');
    if (colon == std::string::npos) return authority;
    return creds.substr(0, colon) + authority.substr(at);
}

bool net_split_virtual(const char* path, std::string* root, std::string* rpath) {
    root->clear();
    rpath->clear();
    if (!path) return false;
    NetProto proto;
    const char* rest = nullptr;
    if (!scheme_of(path, &proto, &rest)) return false;
    std::string authority, rp;
    if (!split_authority(rest, &authority, &rp)) return false;
    // The root keeps the user name, because two users on one host are two
    // different sites, but never a password: that is what makes it safe to put
    // in a title bar, a tab label or a path bar.
    root->assign(net_proto_scheme(proto));
    root->append(strip_password_authority(authority));
    rpath->assign(rp);
    return true;
}

std::string net_path_parent(const std::string& rpath) {
    if (rpath.empty() || rpath == "/") return "";
    size_t slash = rpath.find_last_of('/');
    if (slash == std::string::npos) return "";
    // The server root is "", not "/", and that is the signal a caller reads as
    // "there is no parent on this server". Returning "/" here would send a pane
    // that pressed ".." in /pub back to /pub, forever.
    if (slash == 0) return "";
    return rpath.substr(0, slash);
}

bool net_path_is_root(const std::string& rpath) {
    return rpath.empty() || rpath == "/";
}

bool net_parse_url(const char* text, NetSite* out, std::string* err) {
    if (err) err->clear();
    if (!text) { if (err) *err = "nothing to connect to"; return false; }

    // Trim, and reject the whitespace a pasted URL brings with it: a space in a
    // host name is never a host name.
    std::string s = text;
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) { if (err) *err = "nothing to connect to"; return false; }
    s = s.substr(b, e - b + 1);

    NetSite site;
    const char* rest = nullptr;
    if (!scheme_of(s.c_str(), &site.proto, &rest)) {
        site.proto = NET_FTP;      // a bare host means plain FTP
        rest = s.c_str();
    }
    std::string authority, rpath;
    if (!split_authority(rest, &authority, &rpath)) {
        if (err) *err = "no host in \"" + s + "\"";
        return false;
    }

    // [user][:password]@ host [:port]
    std::string hostpart = authority;
    size_t at = hostpart.rfind('@');
    if (at != std::string::npos) {
        std::string creds = hostpart.substr(0, at);
        hostpart = hostpart.substr(at + 1);
        size_t colon = creds.find(':');
        if (colon == std::string::npos) {
            site.user = net_url_unescape(creds);
        } else {
            site.user = net_url_unescape(creds.substr(0, colon));
            site.password = net_url_unescape(creds.substr(colon + 1));
        }
    }
    if (hostpart.size() >= 2 && hostpart[0] == '[') {
        size_t close = hostpart.find(']');
        if (close == std::string::npos) { if (err) *err = "unbalanced [ in \"" + s + "\""; return false; }
        site.host = hostpart.substr(1, close - 1);
        if (close + 1 < hostpart.size()) {
            if (hostpart[close + 1] != ':') { if (err) *err = "expected :port in \"" + s + "\""; return false; }
            site.port = atoi(hostpart.c_str() + close + 2);
        }
    } else {
        size_t colon = hostpart.find(':');
        if (colon == std::string::npos) {
            site.host = hostpart;
        } else {
            site.host = hostpart.substr(0, colon);
            site.port = atoi(hostpart.c_str() + colon + 1);
        }
    }
    if (site.host.empty()) { if (err) *err = "no host in \"" + s + "\""; return false; }
    for (char c : site.host) {
        if (isspace((unsigned char)c)) { if (err) *err = "host names cannot contain spaces"; return false; }
    }
    if (site.port < 0 || site.port > 65535) site.port = 0;
    // Port 990 is the convention for implicit FTPS, and it is the only hint a
    // typed URL gives: "ftps://host:990" means TLS from the first byte.
    if (site.proto == NET_FTPS && site.port == 990) site.proto = NET_FTPS_IMPLICIT;
    // A URL that names no port still connects to one, and the record says which:
    // the dialog shows this number rather than a 0 for someone to interpret.
    // net_site_root() leaves a default port out of the URL, so this stays the
    // same root string either way.
    if (site.port == 0) site.port = net_proto_port(site.proto);
    // FTP has a guest account by convention, so a URL with no user means it.
    // SFTP has no such convention; there curl falls back to the local user name,
    // which is the only guess worth making, so the field is left empty.
    if (site.user.empty() && site.proto != NET_SFTP) site.user = "anonymous";
    site.path = rpath == "/" ? "" : rpath;

    *out = site;
    return true;
}

// ---------------------------------------------------------------------------
// The sites file
// ---------------------------------------------------------------------------

static std::string sites_dir() {
    const char* home = getenv("HOME");
    if (!home || !*home) return ".mc-clone";
    return std::string(home) + "/.config/mc-clone";
}

std::string net_sites_path() { return sites_dir() + "/ftp_sites"; }

static void ensure_sites_dir(void) {
    std::string dir = sites_dir();
    std::string parent = dir.substr(0, dir.find_last_of('/'));
    mkdir(parent.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
}

static char* sites_trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

bool net_sites_load(std::vector<NetSite>* out, std::string* err) {
    if (err) err->clear();
    out->clear();
    FILE* f = fopen(net_sites_path().c_str(), "r");
    if (!f) return true;              // no file yet is not an error

    NetSite site;
    bool open_block = false;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* s = sites_trim(line);
        if (*s == '\0' || *s == '#') continue;
        if (*s == '[') {
            if (open_block) out->push_back(site);
            site = NetSite();
            open_block = !strncmp(s, "[site]", 6);
            continue;
        }
        if (!open_block) continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = sites_trim(s);
        char* val = sites_trim(eq + 1);
        if      (!strcmp(key, "name"))     site.name = val;
        else if (!strcmp(key, "proto"))    net_proto_of(val, &site.proto);
        else if (!strcmp(key, "host"))     site.host = val;
        else if (!strcmp(key, "port"))     site.port = atoi(val);
        else if (!strcmp(key, "user"))     site.user = val;
        else if (!strcmp(key, "password")) site.password = val;
        else if (!strcmp(key, "remember")) site.remember = atoi(val) != 0;
        else if (!strcmp(key, "path"))     site.path = val;
        else if (!strcmp(key, "verify_tls")) site.verify_tls = atoi(val) != 0;
        else if (!strcmp(key, "passive"))    site.passive = atoi(val) != 0;
        // An unknown key is ignored rather than fatal: a newer version's file
        // still opens.
    }
    if (open_block) out->push_back(site);
    fclose(f);

    // A block with no host is noise, not a site; dropping it keeps the dialog's
    // row count honest.
    for (size_t i = 0; i < out->size();) {
        if (!(*out)[i].valid()) out->erase(out->begin() + (long)i);
        else i++;
    }
    return true;
}

bool net_sites_save(const std::vector<NetSite>& sites, std::string* err) {
    if (err) err->clear();
    ensure_sites_dir();
    std::string path = net_sites_path();

    // open() with the mode, not fopen(): the file may hold passwords, and a
    // window between creating it world-readable and tightening it is a window
    // another user can read it in. fchmod afterwards also fixes a file that
    // already existed with the wrong mode.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { if (err) *err = std::string("cannot write ") + path + ": " + strerror(errno); return false; }
    fchmod(fd, 0600);
    FILE* f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        if (err) *err = "cannot write " + path;
        return false;
    }

    fprintf(f,
        "# mc-clone remote sites\n"
        "#\n"
        "# One [site] block per site. proto is one of ftp, ftps, ftps-implicit,\n"
        "# sftp. port 0 means the protocol's own default. verify_tls 0 connects to a\n"
        "# server whose certificate cannot be checked, and the UI says so while it\n"
        "# is in effect.\n"
        "#\n"
        "# A password is stored in cleartext, and only for a site with remember = 1.\n"
        "# This file is created 0600 so only the owner can read it; the main config\n"
        "# is world-readable and never holds a password.\n");

    for (const NetSite& s : sites) {
        if (!s.valid()) continue;
        fprintf(f, "\n[site]\n");
        fprintf(f, "name = %s\n", s.name.c_str());
        fprintf(f, "proto = %s\n", net_proto_key(s.proto));
        fprintf(f, "host = %s\n", s.host.c_str());
        fprintf(f, "port = %d\n", s.port);
        fprintf(f, "user = %s\n", s.user.c_str());
        if (s.remember && !s.password.empty()) fprintf(f, "password = %s\n", s.password.c_str());
        fprintf(f, "remember = %d\n", s.remember ? 1 : 0);
        fprintf(f, "path = %s\n", s.path.c_str());
        fprintf(f, "verify_tls = %d\n", s.verify_tls ? 1 : 0);
        fprintf(f, "passive = %d\n", s.passive ? 1 : 0);
    }

    fclose(f);
    return true;
}

const NetSite* net_sites_find(const std::vector<NetSite>& sites, const std::string& root) {
    for (const NetSite& s : sites)
        if (net_site_root(s) == root) return &s;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Listing parsers
// ---------------------------------------------------------------------------

static const char* kMonths[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec" };

time_t net_parse_mlsd_time(const char* value) {
    // RFC 3659 writes `modify` as YYYYMMDDHHMMSS in UTC. Servers add separators
    // and sometimes drop the trailing fields, so the digits are collected and
    // read by width: 20230131120000, 2023-01-31 12:00:00 and 20230131 all land
    // on the same instant. Reading them with strtol() instead would swallow the
    // whole unseparated value as one number.
    if (!value) return 0;
    char num[16];
    int n = 0;
    for (const char* p = value; *p && n < 15; p++) {
        if (*p == '.') break;                        // fractional seconds
        if (isdigit((unsigned char)*p)) num[n++] = *p;
    }
    if (n < 8) return 0;
    num[n] = '\0';
    auto pair = [&](int at) { return (num[at] - '0') * 10 + (num[at + 1] - '0'); };
    int y  = (num[0] - '0') * 1000 + (num[1] - '0') * 100
           + (num[2] - '0') * 10 + (num[3] - '0');
    int mo = pair(4), d = pair(6), h = 0, mi = 0, s = 0;
    if (n >= 10) h  = pair(8);
    if (n >= 12) mi = pair(10);
    if (n >= 14) s  = pair(12);

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = 0;
    time_t t = timegm(&tm);
    return t == (time_t)-1 ? 0 : t;
}

mode_t net_mode_from_rwx(const char* rwx) {
    // Ten characters in ls order: rwx[0] is the type, rwx[1..9] are the nine
    // permission slots. The bits are what format_mode() prints back.
    static const char* kBits = "rwxrwxrwx";
    mode_t m = 0;
    for (int i = 0; i < 9; i++) {
        // A lowercase s or t in an x slot carries both facts: the execute bit is
        // set and the special bit is, which is why 's' and 'S' differ at all.
        if (rwx[i + 1] == kBits[i] ||
            (i == 2 && rwx[i + 1] == 's') ||
            (i == 5 && rwx[i + 1] == 's') ||
            (i == 8 && rwx[i + 1] == 't'))
            m |= (mode_t)(1 << (8 - i));
    }
    if (rwx[3] == 's' || rwx[3] == 'S') m |= S_ISUID;
    if (rwx[6] == 's' || rwx[6] == 'S') m |= S_ISGID;
    if (rwx[9] == 't' || rwx[9] == 'T') m |= S_ISVTX;
    switch (rwx[0]) {
        case '-': m |= S_IFREG; break;
        case 'd': m |= S_IFDIR; break;
        case 'l': m |= S_IFLNK; break;
        case 'c': m |= S_IFCHR; break;
        case 'b': m |= S_IFBLK; break;
        case 'p': m |= S_IFIFO; break;
        case 's': m |= S_IFSOCK; break;
        default:  break;
    }
    return m;
}

// A fact is "token=value". An entry name is bytes and can carry '=' itself -
// "a=b" is an ordinary file name - so a segment counts as a fact only when what
// comes before its '=' reads like a fact name: letters, digits, and the dots and
// underscores the registry uses. Without that, "type=file;size=4;modify=...; a=b"
// took its own name for a fact and the row disappeared from the pane.
static bool mlsd_fact_key(const std::string& seg, size_t eq) {
    if (!eq) return false;
    for (size_t i = 0; i < eq; i++) {
        char c = seg[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool net_parse_mlsd(const char* line, FileEntry* out) {
    // "type=file;size=1234;modify=20230131120000;name with spaces"
    // The facts are the segments that contain a '='. The first one that does not
    // is where the entry's name starts, and the name runs to the end of the line
    // including any ';' it contains, because a name is bytes and not a fact.
    if (!line || !*line) return false;
    const char* p = line;
    bool have_type = false;
    bool is_dir = false;
    bool skip = false;
    off_t size = 0;
    time_t mtime = 0;
    mode_t bits = 0;
    std::string name;

    while (*p) {
        const char* semi = strchr(p, ';');
        std::string seg(semi ? std::string(p, (size_t)(semi - p)) : std::string(p));
        size_t eq = seg.find('=');
        if (eq == std::string::npos || !mlsd_fact_key(seg, eq)) {   // not a fact: the name starts here
            // RFC 3659 separates the facts from the entry with one space, and real
            // servers send it: "type=file;size=12;modify=...; report.pdf". Taking
            // the rest of the line whole gave every name on a real server a leading
            // space, which then reappeared in the path handed back when a row was
            // opened or fetched. Exactly one space goes away, because a name that
            // genuinely begins with a space is sent as two.
            name.assign(*p == ' ' ? p + 1 : p);
            break;
        }
        if (!semi) break;                // a trailing fact: no name follows
        std::string key = seg.substr(0, eq);
        std::string val = seg.substr(eq + 1);
        for (size_t i = 0; i < key.size(); i++) key[i] = (char)tolower((unsigned char)key[i]);
        for (size_t i = 0; i < val.size(); i++) val[i] = (char)tolower((unsigned char)val[i]);
        if      (!key.compare("type")) {
            have_type = true;
            // MLST writes Type=File and MLSD writes type=file; the value's case
            // is a convention of the command, not a fact about the file.
            if      (val == "dir")  is_dir = true;
            else if (val == "file") is_dir = false;
            else                    skip = true;   // cdir, pdir: not rows to show
        } else if (!key.compare("size")) {
            size = (off_t)strtoll(val.c_str(), nullptr, 10);
        } else if (!key.compare("modify")) {
            mtime = net_parse_mlsd_time(val.c_str());
        } else if (!key.compare("unix.mode")) {
            bits = (mode_t)strtoul(val.c_str(), nullptr, 8);
        }
        p = semi + 1;
    }
    if (name.empty() || !have_type || skip) return false;
    // MLSD names are raw bytes, not percent-encoded, so they are taken as they
    // come; decoding them would corrupt a name that really does contain a '%'.
    if (name.size() >= sizeof(out->name)) name.resize(sizeof(out->name) - 1);
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name.c_str());
    out->mode = (bits & 07777) | (is_dir ? S_IFDIR : S_IFREG);
    out->size = is_dir ? 0 : size;
    out->mtime = mtime;
    return true;
}

// "Jan 01 12:00" or "Jan 01  2023" -> time_t. A listing that prints HH:MM means
// "within the last six months", so the current year is assumed and a date that
// has not happened yet belongs to last year.
static time_t parse_ls_date(const char* mon, int day, const char* rest) {
    int month = -1;
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(mon, kMonths[i], 3)) { month = i; break; }
    if (month < 0) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_mon = month;
    tm.tm_mday = day;
    const char* colon = strchr(rest, ':');
    if (colon) {
        time_t now = time(nullptr);
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        tm.tm_year = now_tm.tm_year;
        tm.tm_hour = atoi(rest);
        tm.tm_min = atoi(colon + 1);
    } else {
        tm.tm_year = atoi(rest) - 1900;
    }
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return 0;
    if (colon) {
        time_t now = time(nullptr);
        if (t > now) {                       // "Dec 31 23:59" in March
            tm.tm_year -= 1;
            t = mktime(&tm);
        }
    }
    return t;
}

bool net_parse_list_line(const char* line, FileEntry* out) {
    if (!line || !*line) return false;
    memset(out, 0, sizeof(*out));

    // unix dialect: "-rw-r--r-- 1 owner group 1234 Jan 01 12:00 name"
    if (strchr("-dlcbsptS", line[0]) && (isprint((unsigned char)line[1]) || line[1] == '?')) {
        char rwx[11];
        snprintf(rwx, sizeof(rwx), "%.*s", 10, line);
        // Fields: perms links owner group size mon day time-or-year, then the
        // name to the end of the line. Counting from the front is what keeps a
        // name containing spaces intact.
        const char* p = line + 10;
        std::string fields[7];
        for (int i = 0; i < 7; i++) {
            while (*p == ' ') p++;
            const char* start = p;
            while (*p && *p != ' ') p++;
            if (start == p) return false;
            fields[i].assign(start, (size_t)(p - start));
        }
        while (*p == ' ') p++;
        if (!*p) return false;
        int day = atoi(fields[5].c_str());
        time_t mtime = parse_ls_date(fields[4].c_str(), day, fields[6].c_str());
        mode_t m = net_mode_from_rwx(rwx);
        if (!(m & S_IFMT)) m |= S_IFREG;
        snprintf(out->name, sizeof(out->name), "%s", p);
        out->mode = m;
        out->size = S_ISDIR(m) ? 0 : (off_t)strtoll(fields[3].c_str(), nullptr, 10);
        out->mtime = mtime;
        return out->name[0] != '\0';
    }

    // DOS dialect: "12-25-02  09:11AM       1234 name" and "... 12:00PM  <DIR>  name"
    if (isdigit((unsigned char)line[0]) && line[2] == '-' && line[5] == '-') {
        int mm = atoi(line), dd = atoi(line + 3), yy = atoi(line + 6);
        const char* sp = strchr(line, ' ');
        while (sp && *sp == ' ') sp++;
        if (!sp) return false;
        const char* tstart = sp;
        const char* tend = tstart;
        while (*tend && *tend != ' ') tend++;
        std::string tstr(tstart, (size_t)(tend - tstart));
        // The size or the <DIR> marker, then the name to the end of the line.
        const char* p = tend;
        while (*p == ' ') p++;
        const char* fs = p;
        while (*p && *p != ' ') p++;
        std::string third(fs, (size_t)(p - fs));
        while (*p == ' ') p++;
        if (!*p) return false;

        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (yy < 70) yy += 2000; else yy += 1900;
        tm.tm_year = yy - 1900;
        tm.tm_mon = mm - 1;
        tm.tm_mday = dd;
        int hour = atoi(tstr.c_str());
        int minute = 0;
        size_t colon = tstr.find(':');
        if (colon != std::string::npos) minute = atoi(tstr.c_str() + colon + 1);
        if ((tstr.back() == 'P' || tstr.back() == 'p') && hour < 12) hour += 12;
        if ((tstr.back() == 'A' || tstr.back() == 'a') && hour == 12) hour = 0;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_isdst = -1;

        bool dir = third == "<DIR>";
        out->mode = dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        out->size = dir ? 0 : (off_t)strtoll(third.c_str(), nullptr, 10);
        out->mtime = mktime(&tm);
        snprintf(out->name, sizeof(out->name), "%s", p);
        return out->name[0] != '\0';
    }

    return false;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

#ifdef MC_HAVE_CURL

static bool g_curl_global_done = false;

struct CachedHandle {
    std::string key;      // the site root: scheme, user, host, port
    CURL* h;
};

static std::vector<CachedHandle>& handle_cache(void) {
    static std::vector<CachedHandle> c;
    return c;
}

bool net_available(void) { return true; }

void net_what(char* out, size_t n) {
    curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    const char* ssl = "no TLS";
    if (v && v->ssl_version) ssl = v->ssl_version;
    snprintf(out, n, "libcurl %s (%s)", v ? v->version : "?", ssl);
}

void net_forget(const NetSite* site) {
    std::string key = site ? net_site_root(*site) : std::string();
    std::vector<CachedHandle>& c = handle_cache();
    for (size_t i = 0; i < c.size();) {
        if (key.empty() || c[i].key == key) {
            curl_easy_cleanup(c[i].h);
            c.erase(c.begin() + (long)i);
        } else {
            i++;
        }
    }
}

void net_shutdown(void) {
    net_forget(nullptr);
    if (g_curl_global_done) {
        curl_global_cleanup();
        g_curl_global_done = false;
    }
}

// One handle per site, reused across navigations. That is the whole reason this
// backend links a library: the second listing into the same server does not log
// in again or redo the TLS handshake.
static CURL* handle_for(const NetSite& site) {
    if (!g_curl_global_done) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curl_global_done = true;
    }
    std::string key = net_site_root(site);
    std::vector<CachedHandle>& c = handle_cache();
    for (CachedHandle& e : c)
        if (e.key == key) return e.h;
    CURL* h = curl_easy_init();
    if (!h) return nullptr;
    c.push_back({ key, h });
    return h;
}

// Everything that depends on the site, set from scratch: curl_easy_reset()
// between transfers means an option from the previous listing cannot leak into
// this one.
static bool setup_handle(CURL* h, const NetSite& site, const std::string& url,
                         char* errbuf, size_t errbuf_size) {
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_USERNAME, site.user.empty() ? "anonymous" : site.user.c_str());
    curl_easy_setopt(h, CURLOPT_PASSWORD, site.password.c_str());

    if (site.proto == NET_SFTP) {
        curl_easy_setopt(h, CURLOPT_USE_SSL, (long)CURLUSESSL_NONE);
        // libssh2 has no server certificate to check; the host key is accepted
        // on first use and remembered by the caller, not here.
    } else if (site.proto == NET_FTP) {
        curl_easy_setopt(h, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
        curl_easy_setopt(h, CURLOPT_FTPSSLAUTH, (long)CURLFTPAUTH_TLS);
    } else {
        curl_easy_setopt(h, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
        curl_easy_setopt(h, CURLOPT_FTPSSLAUTH,
                         site.proto == NET_FTPS_IMPLICIT ? (long)CURLFTPAUTH_SSL
                                                         : (long)CURLFTPAUTH_TLS);
    }
    // Verification is on unless the site says otherwise, and when it is off the
    // UI has to say so out loud: a mode that degrades silently is the one thing
    // not to get wrong here.
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, site.verify_tls ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, site.verify_tls ? 2L : 0L);

    curl_easy_setopt(h, CURLOPT_FTPPORT, site.passive ? (const char*)NULL : "-");
    curl_easy_setopt(h, CURLOPT_FTP_USE_EPSV, 1L);
    // Deliberately not here: CURLOPT_FTP_CREATE_MISSING_DIRS makes curl answer a
    // failed CWD by sending MKD, so a listing of a path that does not exist would
    // create it on the server. Only a store wants a missing path built. See
    // net_upload(), the one transfer that sets it.

    // A dead server has to end rather than hang: the connect timeout covers the
    // handshake and the low-speed guard covers a connection that goes quiet
    // mid-transfer, which is also what makes the progress dialog's Abort land
    // on a stalled transfer.
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    if (errbuf && errbuf_size) {
        errbuf[0] = '\0';
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    }
    return true;
}

// curl's own message when it has one: the server's reply is far more useful
// than "Could not connect to server". The URL never carries a password here,
// because it is built from a site root, so nothing has to be redacted.
static std::string curl_error(CURLcode rc, const char* errbuf) {
    if (errbuf && errbuf[0]) return errbuf;
    return curl_easy_strerror(rc);
}

// Progress plumbing for the transfers. `aborted` records that the transfer
// stopped because the user pressed Abort in the progress dialog, which is a
// different outcome from a failure and must not be retried.
struct PerformCtx {
    fs_progress_fn cb;
    void* user;
    bool aborted;
    bool upload;
};

static int xferinfo_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
    PerformCtx* c = (PerformCtx*)clientp;
    if (!c->cb) return 0;
    long long total = c->upload ? (long long)ultotal : (long long)dltotal;
    long long done  = c->upload ? (long long)ulnow   : (long long)dlnow;
    // fs_progress_fn returns false to abort; curl reads non-zero the same way.
    if (!c->cb(done, total, done > 0 ? 1 : 0, 1, c->user)) { c->aborted = true; return 1; }
    return 0;
}

struct LineSink {
    std::string text;
};

static size_t collect_lines_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    LineSink* s = (LineSink*)user;
    s->text.append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t discard_cb(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

// The name of an entry, and the directory holding it.
static void split_remote(const std::string& rpath, std::string* parent, std::string* base) {
    size_t slash = rpath.find_last_of('/');
    if (slash == std::string::npos) { *parent = "/"; *base = rpath; return; }
    base->assign(rpath.substr(slash + 1));
    parent->assign(slash == 0 ? "/" : rpath.substr(0, slash));
}

// The URL a transfer goes to: the pane's virtual path with one thing changed,
// the scheme curl has to see. libcurl reads an `ftps://` prefix as *implicit*
// TLS - it starts the handshake before it has said a single FTP command, which
// against a server waiting for cleartext is the "wrong version number" handshake
// failure. So explicit FTPS travels as `ftp://` with CURLOPT_USE_SSL set to ALL,
// and only NET_FTPS_IMPLICIT gets the `ftps://` prefix. The site's root string
// keeps the scheme the user typed: that is what the path bar shows, and it is
// what tells a saved explicit-FTPS site apart from a plain one.
static std::string curl_url(const NetSite& site, const std::string& rpath) {
    std::string shown = net_proto_scheme(site.proto);
    std::string wanted = site.proto == NET_FTPS ? "ftp://" : shown;
    std::string root = net_site_root(site);
    if (wanted != shown && root.compare(0, shown.size(), shown) == 0)
        root.replace(0, shown.size(), wanted);
    // The path is escaped here and nowhere else. The pane's virtual path keeps
    // the name as the server spells it, because that is what the path bar shows,
    // what a tab and a history entry carry, and what a transfer names back; only
    // the URL has to speak URL.
    return net_virtual_path(root, net_url_escape(rpath, true));
}

// The URL of a directory on the server, and the reason it has to end in '/'.
// Without the slash curl sends SIZE <last component> to decide whether the
// target is a file or a directory: a directory answers 550, and curl then
// reports "The file does not exist" without ever sending the command that was
// actually wanted. That is a confusing failure, because the path is correct.
static std::string dir_url(const NetSite& site, const std::string& rpath) {
    std::string url = curl_url(site, rpath);
    if (url.empty() || url.back() != '/') url += '/';
    return url;
}

// A listing, in either dialect. MLSD is machine-readable and carries the mode,
// so it is tried first; a server without it answers an error, and the same URL
// is asked again with LIST.
// A unix LIST names "." and "..". They are not entries of the directory, and a
// caller that shows a listing adds its own ".." row, so they are dropped here
// rather than at every use site.
static void drop_dot_entries(std::vector<FileEntry>* rows) {
    for (size_t i = 0; i < rows->size();) {
        if (strcmp(rows->at(i).name, ".") == 0 || strcmp(rows->at(i).name, "..") == 0)
            rows->erase(rows->begin() + i);
        else
            i++;
    }
}

static bool listing(const NetSite& site, const std::string& rpath, const char* cmd,
                    std::vector<FileEntry>* out, std::string* err) {
    std::string url = dir_url(site, rpath);
    char errbuf[CURL_ERROR_SIZE];
    LineSink sink;

    CURL* h = handle_for(site);
    if (!h) { if (err) *err = "out of memory"; return false; }
    setup_handle(h, site, url, errbuf, sizeof(errbuf));
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, cmd);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect_lines_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        net_forget(&site);
        h = handle_for(site);
        if (!h) { if (err) *err = "out of memory"; return false; }
        setup_handle(h, site, url, errbuf, sizeof(errbuf));
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, cmd);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect_lines_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
        rc = curl_easy_perform(h);
    }
    if (rc != CURLE_OK) {
        if (err) *err = curl_error(rc, errbuf);
        return false;
    }

    // Split the response into lines and parse each. A line that does not parse
    // is skipped rather than failing the listing: servers add banners, and a
    // pane with most of its rows is better than a pane with an error.
    size_t pos = 0;
    while (pos < sink.text.size()) {
        size_t nl = sink.text.find('\n', pos);
        std::string line = sink.text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                        : nl - pos);
        pos = (nl == std::string::npos) ? sink.text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        FileEntry fe;
        bool ok = false;
        if (!strcmp(cmd, "MLSD")) ok = net_parse_mlsd(line.c_str(), &fe);
        else                      ok = net_parse_list_line(line.c_str(), &fe);
        if (ok) out->push_back(fe);
    }
    drop_dot_entries(out);
    return true;
}

bool net_list(const NetSite& site, const std::string& rpath,
              std::vector<FileEntry>* out, std::string* err) {
    out->clear();
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    // MLSD first, and a LIST whenever MLSD produced nothing: an empty answer is
    // either a genuinely empty directory or a server that ignored the command,
    // and only a LIST can tell them apart.
    if (site.proto != NET_SFTP) {
        std::string mlsd_err;
        if (listing(site, rpath, "MLSD", out, &mlsd_err) && !out->empty()) return true;
    }
    std::string list_err;
    if (listing(site, rpath, "LIST", out, &list_err)) return true;
    if (err) *err = list_err;
    return false;
}

bool net_stat(const NetSite& site, const std::string& rpath, FileEntry* out, std::string* err) {
    memset(out, 0, sizeof(*out));
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    // One listing of the parent, and the row whose name matches. There is no
    // cheaper way and no reason to look for one: curl ignores
    // CURLOPT_CUSTOMREQUEST "MLST" on a file URL, sending SIZE and then RETR
    // instead, so the fact line never arrives. Over SFTP it was never on the
    // table. The same answer is what makes a batch of names cheap - see the
    // caller that lists the destination once instead of calling this per name.
    std::string parent, base;
    split_remote(rpath, &parent, &base);
    if (base.empty()) { if (err) *err = "no name to look up"; return false; }
    std::vector<FileEntry> rows;
    if (!net_list(site, parent, &rows, err)) return false;
    for (const FileEntry& fe : rows)
        if (!strcmp(fe.name, base.c_str())) { *out = fe; return true; }
    // The directory listed, so this is a missing entry and not a failed request:
    // the caller's "already deleted?" answer has to be distinguishable from a
    // server that refused the listing.
    if (err) *err = base + " is not on the server";
    return false;
}

struct FileSink {
    FILE* f;
};

static size_t write_file_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    FileSink* s = (FileSink*)user;
    return fwrite(ptr, size, nmemb, s->f);
}

static size_t read_file_cb(char* dest, size_t size, size_t nmemb, void* user) {
    FileSink* s = (FileSink*)user;
    return fread(dest, size, nmemb, s->f);
}

bool net_download(const NetSite& site, const std::string& rpath, const char* local,
                  fs_progress_fn cb, void* user, std::string* err) {
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    FILE* f = fopen(local, "wb");
    if (!f) { if (err) *err = std::string("cannot write ") + local + ": " + strerror(errno); return false; }
    std::string url = curl_url(site, rpath);
    char errbuf[CURL_ERROR_SIZE];
    PerformCtx pctx = { cb, user, false, false };
    CURL* h = handle_for(site);
    bool ok = false;
    if (!h) { fclose(f); if (err) *err = "out of memory"; return false; }
    setup_handle(h, site, url, errbuf, sizeof(errbuf));
    FileSink sink = { f };
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) net_forget(&site);
    ok = (rc == CURLE_OK);
    if (!ok && err) *err = pctx.aborted ? std::string("aborted") : curl_error(rc, errbuf);
    fclose(f);
    if (!ok) remove(local);   // a partial file is worse than no file
    return ok;
}

bool net_upload(const NetSite& site, const std::string& rpath, const char* local,
                fs_progress_fn cb, void* user, std::string* err) {
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    FILE* f = fopen(local, "rb");
    if (!f) { if (err) *err = std::string("cannot read ") + local + ": " + strerror(errno); return false; }
    struct stat st;
    long long size = -1;
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) size = (long long)st.st_size;

    std::string url = curl_url(site, rpath);
    char errbuf[CURL_ERROR_SIZE];
    PerformCtx pctx = { cb, user, false, true };
    CURL* h = handle_for(site);
    if (!h) { fclose(f); if (err) *err = "out of memory"; return false; }
    setup_handle(h, site, url, errbuf, sizeof(errbuf));
    // The one place a missing path is worth building: a store into a folder the
    // server does not have yet is an ordinary upload of a tree, not a mistake.
    curl_easy_setopt(h, CURLOPT_FTP_CREATE_MISSING_DIRS, (long)CURLFTP_CREATE_DIR_RETRY);
    curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
    FileSink sink = { f };
    curl_easy_setopt(h, CURLOPT_READFUNCTION, read_file_cb);
    curl_easy_setopt(h, CURLOPT_READDATA, &sink);
    if (size >= 0) curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) net_forget(&site);
    fclose(f);
    if (rc != CURLE_OK) {
        if (err) *err = pctx.aborted ? std::string("aborted") : curl_error(rc, errbuf);
        return false;
    }
    return true;
}

// Mutations are FTP commands sent after the server has changed into the entry's
// parent directory, with the name relative to it. That is what real servers
// accept; assuming an absolute path in MKD/DELE works on some and not on others.
// The URL itself is a directory, so curl would list it: the listing is thrown
// away, which costs one round trip and keeps the command sequence predictable.
//
// The CWD that pins the directory is sent by us, not left to curl. curl decides
// whether the URL needs a CWD from its own idea of where the connection is, and
// that idea is per-easy-handle: the handle is reset between transfers while the
// cached connection keeps whatever directory the last one left it in, so curl
// can be sure it is already in the right place when the server is not. The log
// shows it plainly - RNFR put.txt answered 550 with the session sitting in
// /partial. An explicit absolute CWD at the head of the quote list runs after
// anything curl does and settles it.
static bool quote_cmds(const NetSite& site, const std::string& parent,
                       const std::vector<std::string>& cmds, std::string* err) {
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    std::string url = dir_url(site, parent);
    std::string abs = (!parent.empty() && parent[0] == '/') ? parent : "/" + parent;
    char errbuf[CURL_ERROR_SIZE];
    CURL* h = handle_for(site);
    if (!h) { if (err) *err = "out of memory"; return false; }
    setup_handle(h, site, url, errbuf, sizeof(errbuf));
    struct curl_slist* q = nullptr;
    q = curl_slist_append(q, ("CWD " + abs).c_str());
    for (const std::string& c : cmds) q = curl_slist_append(q, c.c_str());
    curl_easy_setopt(h, CURLOPT_QUOTE, q);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, discard_cb);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) net_forget(&site);
    curl_slist_free_all(q);
    if (rc != CURLE_OK) { if (err) *err = curl_error(rc, errbuf); return false; }
    return true;
}

bool net_mkdir(const NetSite& site, const std::string& rpath, std::string* err) {
    std::string parent, base;
    split_remote(rpath, &parent, &base);
    if (base.empty()) { if (err) *err = "no name to create"; return false; }
    // Over SSH libcurl translates the same MKD quote command into a mkdir; it
    // is not an FTP-only trick, so there is only one path here.
    return quote_cmds(site, parent, { "MKD " + base }, err);
}

bool net_remove(const NetSite& site, const std::string& rpath, bool is_dir, std::string* err) {
    std::string parent, base;
    split_remote(rpath, &parent, &base);
    if (base.empty()) { if (err) *err = "no name to delete"; return false; }
    return quote_cmds(site, parent, { (is_dir ? "RMD " : "DELE ") + base }, err);
}

// FTP has no recursive delete, so a directory goes only after everything inside
// it: list, take out the deepest entries first, then the directory itself.
bool net_remove_tree(const NetSite& site, const std::string& rpath,
                     fs_progress_fn cb, void* user, std::string* err) {
    FileEntry me;
    std::string stat_err;
    bool is_dir = net_stat(site, rpath, &me, &stat_err) && S_ISDIR(me.mode);
    if (!is_dir) return net_remove(site, rpath, false, err);

    std::vector<FileEntry> rows;
    if (!net_list(site, rpath, &rows, err)) return false;
    for (const FileEntry& e : rows) {
        std::string child = rpath.empty() ? e.name : rpath + "/" + e.name;
        if (S_ISDIR(e.mode)) {
            if (!net_remove_tree(site, child, cb, user, err)) return false;
        } else if (!net_remove(site, child, false, err)) {
            return false;
        }
        // The count is what a user watches here: a directory has no byte total,
        // and this is the only progress a remote delete can report.
        if (cb && !cb(0, 0, 0, 0, user)) { if (err) *err = "aborted"; return false; }
    }
    return net_remove(site, rpath, true, err);
}

// Every local directory a download needs, one component at a time. A fetch of a
// remote tree arrives before any of it exists locally.
static bool make_local_dirs(const char* path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

// FTP has no recursive retrieve either: a directory is one mkdir and one fetch
// per entry, taken from a listing.
bool net_download_tree(const NetSite& site, const std::string& rpath, const char* local,
                       fs_progress_fn cb, void* user, std::string* err) {
    FileEntry me;
    std::string stat_err;
    if (!net_stat(site, rpath, &me, &stat_err) || !S_ISDIR(me.mode))
        return net_download(site, rpath, local, cb, user, err);

    if (!make_local_dirs(local)) {
        if (err) *err = std::string(local) + ": " + strerror(errno);
        return false;
    }
    std::vector<FileEntry> rows;
    if (!net_list(site, rpath, &rows, err)) return false;
    for (const FileEntry& e : rows) {
        std::string child = rpath.empty() ? e.name : rpath + "/" + e.name;
        char child_local[4300];
        snprintf(child_local, sizeof(child_local), "%s/%s", local, e.name);
        if (!net_download_tree(site, child, child_local, cb, user, err)) return false;
    }
    return true;
}

// The mirror image: a local folder goes up as MKD plus one store per entry. An
// existing remote directory is kept rather than failed on, because uploading
// over a folder that is already there is an overwrite, not an error.
bool net_upload_tree(const NetSite& site, const char* local, const std::string& rpath,
                     fs_progress_fn cb, void* user, std::string* err) {
    struct stat st;
    if (stat(local, &st) != 0) {
        if (err) *err = std::string(local) + ": " + strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) return net_upload(site, rpath, local, cb, user, err);

    FileEntry me;
    std::string stat_err;
    if (!net_stat(site, rpath, &me, &stat_err) || !S_ISDIR(me.mode)) {
        if (!net_mkdir(site, rpath, err)) return false;
    }
    DIR* d = opendir(local);
    if (!d) {
        if (err) *err = std::string(local) + ": " + strerror(errno);
        return false;
    }
    bool ok = true;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        std::string child_local = std::string(local) + "/" + de->d_name;
        std::string child_remote = rpath.empty() ? de->d_name : rpath + "/" + de->d_name;
        if (!net_upload_tree(site, child_local.c_str(), child_remote, cb, user, err)) { ok = false; break; }
        if (cb && !cb(0, 0, 0, 0, user)) { ok = false; if (err) *err = "aborted"; break; }
    }
    closedir(d);
    return ok;
}

bool net_rename(const NetSite& site, const std::string& rpath, const char* new_name,
                std::string* err) {
    std::string parent, base;
    split_remote(rpath, &parent, &base);
    if (base.empty() || !new_name || !*new_name) { if (err) *err = "nothing to rename"; return false; }
    if (strchr(new_name, '/')) { if (err) *err = "a new name cannot contain /"; return false; }
    // RNFR and RNTO have to arrive as a pair, which one QUOTE list guarantees.
    return quote_cmds(site, parent, { "RNFR " + base, "RNTO " + std::string(new_name) }, err);
}

bool net_chmod(const NetSite& site, const std::string& rpath, int mode, std::string* err) {
    std::string parent, base;
    split_remote(rpath, &parent, &base);
    if (base.empty()) { if (err) *err = "no name to change"; return false; }
    char buf[64];
    snprintf(buf, sizeof(buf), "SITE CHMOD %03o %s", mode & 07777, base.c_str());
    // Not every server implements SITE CHMOD. The server's own reply is what
    // the caller shows, rather than a generic failure that hides the reason.
    return quote_cmds(site, parent, { buf }, err);
}

#else  // ---------------------------------------------------------------------
// A build without libcurl. The vocabulary, the sites file and the parsers above
// still work, so the dialogs and the tests behave the same and only the
// transport says no. This is the same shape as a build without FreeType.
// ---------------------------------------------------------------------------

bool net_available(void) { return false; }

void net_what(char* out, size_t n) {
    snprintf(out, n, "no FTP support in this build");
}

void net_forget(const NetSite*) {}
void net_shutdown(void) {}

static const char* const kNoCurl = "this build has no FTP support: install libcurl and rebuild";

static bool no_curl(std::string* err) {
    if (err) *err = kNoCurl;
    return false;
}

bool net_list(const NetSite&, const std::string&, std::vector<FileEntry>* out, std::string* err) {
    out->clear();
    return no_curl(err);
}
bool net_stat(const NetSite&, const std::string&, FileEntry*, std::string* err) { return no_curl(err); }
bool net_download(const NetSite&, const std::string&, const char*, fs_progress_fn, void*, std::string* err) { return no_curl(err); }
bool net_upload(const NetSite&, const std::string&, const char*, fs_progress_fn, void*, std::string* err) { return no_curl(err); }
bool net_mkdir(const NetSite&, const std::string&, std::string* err) { return no_curl(err); }
bool net_remove(const NetSite&, const std::string&, bool, std::string* err) { return no_curl(err); }
bool net_remove_tree(const NetSite&, const std::string&, fs_progress_fn, void*, std::string* err) { return no_curl(err); }
bool net_download_tree(const NetSite&, const std::string&, const char*, fs_progress_fn, void*, std::string* err) { return no_curl(err); }
bool net_upload_tree(const NetSite&, const char*, const std::string&, fs_progress_fn, void*, std::string* err) { return no_curl(err); }
bool net_rename(const NetSite&, const std::string&, const char*, std::string* err) { return no_curl(err); }
bool net_chmod(const NetSite&, const std::string&, int, std::string* err) { return no_curl(err); }

#endif // MC_HAVE_CURL
