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
#include "platform.h"

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
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <mutex>   // call_once: curl_global_init is not thread safe, and a
                   // background transfer calls it from its own thread

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

static std::string sites_dir() { return plat_config_dir(); }

std::string net_sites_path() { return sites_dir() + "/ftp_sites"; }

static void ensure_sites_dir(void) { plat_make_config_dir(); }

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
        else if (!strcmp(key, "rar_order"))  site.rar_order = atoi(val) != 0;
        else if (!strcmp(key, "threads")) {
            int n = atoi(val);
            site.threads = n < 1 ? 1 : (n > 10 ? 10 : n);   // xfer.h's limits
        }
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
#ifndef _WIN32
    // On Windows the file inherits the ACL of %APPDATA%, which is the user's own, and
    // there are no group or world bits to take away.
    fchmod(fd, 0600);
#endif
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
        "# is in effect. threads is how many files a copy moves at once (1-10).\n"
        "# rar_order 1 sends RAR sets volume by volume (.rar .r00 .r01 ... .s00).\n"
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
        fprintf(f, "threads = %d\n", s.threads);
        fprintf(f, "rar_order = %d\n", s.rar_order ? 1 : 0);
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
// Importing FileZilla's site manager
// ---------------------------------------------------------------------------
//
// sitemanager.xml is small and FileZilla writes it the same way every time, so
// a reader for the XML it actually contains - elements, attributes, text, the
// five named entities and numeric ones - is enough, and it keeps an XML library
// out of the build for one dialog. What it does not understand it reports as a
// malformed file rather than guessing at.

namespace {

struct XNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;               // this element's own text, not its children's
    std::vector<XNode> children;

    const XNode* child(const char* n) const {
        for (const XNode& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
    std::string attr(const char* n) const {
        for (const auto& a : attrs)
            if (a.first == n) return a.second;
        return "";
    }
};

void utf8_append(std::string* out, unsigned long cp) {
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::string xml_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if      (ent == "amp")  out.push_back('&');
        else if (ent == "lt")   out.push_back('<');
        else if (ent == "gt")   out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() > 1 && ent[0] == '#') {
            bool hex = ent[1] == 'x' || ent[1] == 'X';
            utf8_append(&out, strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
        } else {
            out.append(s, i, semi - i + 1);   // not ours to interpret: keep it as written
        }
        i = semi;
    }
    return out;
}

bool xml_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string xml_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && xml_space(s[a])) a++;
    while (b > a && xml_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool xml_parse(const std::string& s, XNode* root, std::string* err) {
    // Only the innermost open element ever gains children, so the pointers to
    // its ancestors on this stack stay valid while it grows.
    std::vector<XNode*> stack{ root };
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        if (s[i] != '<') {
            size_t j = s.find('<', i);
            if (j == std::string::npos) j = n;
            stack.back()->text += xml_unescape(s.substr(i, j - i));
            i = j;
            continue;
        }
        if (!s.compare(i, 4, "<!--")) {
            size_t j = s.find("-->", i + 4);
            if (j == std::string::npos) { *err = "an unterminated comment"; return false; }
            i = j + 3;
            continue;
        }
        if (!s.compare(i, 9, "<![CDATA[")) {
            size_t j = s.find("]]>", i + 9);
            if (j == std::string::npos) { *err = "an unterminated CDATA section"; return false; }
            stack.back()->text += s.substr(i + 9, j - i - 9);
            i = j + 3;
            continue;
        }
        if (!s.compare(i, 2, "<?") || !s.compare(i, 2, "<!")) {
            size_t j = s.find('>', i);
            if (j == std::string::npos) { *err = "an unterminated declaration"; return false; }
            i = j + 1;
            continue;
        }
        // The end of the tag is the first '>' outside a quoted attribute value.
        size_t j = i + 1;
        char quote = 0;
        for (; j < n; j++) {
            if (quote) { if (s[j] == quote) quote = 0; }
            else if (s[j] == '"' || s[j] == '\'') quote = s[j];
            else if (s[j] == '>') break;
        }
        if (j >= n) { *err = "an unterminated tag"; return false; }
        std::string tag = s.substr(i + 1, j - i - 1);
        i = j + 1;

        if (!tag.empty() && tag[0] == '/') {
            std::string name = xml_trim(tag.substr(1));
            if (stack.size() < 2 || stack.back()->name != name) {
                *err = "</" + name + "> closes an element that is not open";
                return false;
            }
            stack.pop_back();
            continue;
        }
        bool self_closing = !tag.empty() && tag.back() == '/';
        if (self_closing) tag.pop_back();

        XNode node;
        size_t k = 0;
        while (k < tag.size() && !xml_space(tag[k])) k++;
        node.name = tag.substr(0, k);
        if (node.name.empty()) { *err = "a tag with no name"; return false; }
        while (k < tag.size()) {
            while (k < tag.size() && xml_space(tag[k])) k++;
            if (k >= tag.size()) break;
            size_t key_start = k;
            while (k < tag.size() && tag[k] != '=' && !xml_space(tag[k])) k++;
            std::string key = tag.substr(key_start, k - key_start);
            while (k < tag.size() && xml_space(tag[k])) k++;
            if (k >= tag.size() || tag[k] != '=') { *err = "attribute " + key + " has no value"; return false; }
            k++;
            while (k < tag.size() && xml_space(tag[k])) k++;
            if (k >= tag.size() || (tag[k] != '"' && tag[k] != '\'')) {
                *err = "attribute " + key + " is not quoted";
                return false;
            }
            char q = tag[k++];
            size_t end = tag.find(q, k);
            if (end == std::string::npos) { *err = "attribute " + key + " is not closed"; return false; }
            node.attrs.emplace_back(key, xml_unescape(tag.substr(k, end - k)));
            k = end + 1;
        }
        stack.back()->children.push_back(std::move(node));
        if (!self_closing) stack.push_back(&stack.back()->children.back());
    }
    if (stack.size() != 1) { *err = "<" + stack.back()->name + "> is never closed"; return false; }
    return true;
}

std::string base64_decode(const std::string& in) {
    std::string out;
    unsigned int acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if      (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;                  // padding and line breaks
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// FileZilla's own spelling of a server path: "<type> <prefix length> [prefix]"
// and then each segment as "<length> <segment>", so a segment may hold spaces.
// "1 0 4 home 3 bob" is /home/bob. The server type only matters to FileZilla's
// display, and every server this program reaches takes '/'.
std::string fz_remote_dir(const std::string& s) {
    const char* p = s.c_str();
    char* e;
    strtol(p, &e, 10);                  // server type
    if (e == p) return "";
    p = e;
    long prefix_len = strtol(p, &e, 10);
    if (e == p || prefix_len < 0) return "";
    p = e;
    if (prefix_len > 0) {
        if (*p == ' ') p++;
        if ((long)strlen(p) < prefix_len) return "";
        p += prefix_len;
    }
    std::string out;
    while (*p) {
        long len = strtol(p, &e, 10);
        if (e == p || *e != ' ' || len <= 0) break;
        p = e + 1;
        if ((long)strlen(p) < len) break;
        out += "/" + std::string(p, (size_t)len);
        p += len;
    }
    return out;
}

// One <Server> element. False when it is in a protocol this program cannot open.
bool fz_server(const XNode& x, const std::string& folder, NetImported* out) {
    auto field = [&](const char* name) {
        const XNode* c = x.child(name);
        return c ? xml_trim(c->text) : std::string();
    };
    NetSite& site = out->site;
    // FileZilla's numbering: 0 FTP that upgrades when it can, 1 SFTP, 3 implicit
    // FTPS, 4 explicit FTPS, 6 plain FTP only. The rest - HTTP, S3, WebDAV,
    // cloud drives - are not FTP.
    std::string proto = field("Protocol");
    switch (proto.empty() ? 0 : atoi(proto.c_str())) {
    case 0: case 6: site.proto = NET_FTP; break;
    case 1:         site.proto = NET_SFTP; break;
    case 3:         site.proto = NET_FTPS_IMPLICIT; break;
    case 4:         site.proto = NET_FTPS; break;
    default:        return false;
    }
    site.host = field("Host");
    int port = atoi(field("Port").c_str());
    // The protocol's own port is left implicit, as the dialog does, so the
    // imported site's URL reads the way a typed one would.
    site.port = port == net_proto_port(site.proto) ? 0 : port;

    // 0 anonymous, 1 normal, 2 ask for the password, 3 interactive, 4 account,
    // 5 key file. Only 1 and 4 keep a password in the file.
    int logon = atoi(field("Logontype").c_str());
    if (logon != 0) site.user = field("User");
    const XNode* pass = x.child("Pass");
    if ((logon == 1 || logon == 4) && pass) {
        std::string enc = pass->attr("encoding");
        if (enc == "base64") {
            site.password = base64_decode(pass->text);
        } else if (enc.empty()) {
            site.password = pass->text;     // older versions wrote it as it is
        } else {
            out->note = "the password is locked by FileZilla's master password";
        }
        site.remember = !site.password.empty();
    } else if (logon == 2 || logon == 3) {
        out->note = "FileZilla asks for this password each time";
    } else if (logon == 5) {
        out->note = "the key file is not imported";
    }
    site.passive = field("PasvMode") != "MODE_ACTIVE";
    // The site's own "limit number of simultaneous connections"; 0 means FileZilla's
    // global setting, which is not in this file, so the default stays.
    int conns = atoi(field("MaximumMultipleConnections").c_str());
    if (conns > 0) site.threads = conns < 10 ? conns : 10;
    site.path = fz_remote_dir(field("RemoteDir"));

    // The name is <Name>; versions before it put the name in the element's text.
    std::string name = field("Name");
    if (name.empty()) name = xml_trim(x.text);
    site.name = folder.empty() ? name : folder + "/" + name;
    return true;
}

void fz_walk(const XNode& x, const std::string& folder, std::vector<NetImported>* out,
             int* skipped) {
    for (const XNode& c : x.children) {
        if (c.name == "Folder") {
            std::string name = xml_trim(c.text);
            fz_walk(c, folder.empty() ? name : folder + "/" + name, out, skipped);
        } else if (c.name == "Server") {
            NetImported imp;
            if (!fz_server(c, folder, &imp)) { (*skipped)++; continue; }
            if (imp.site.valid()) out->push_back(imp);
        }
    }
}

} // namespace

std::string net_filezilla_path() {
    std::string home = plat_home_dir();
#ifdef _WIN32
    const char* a = getenv("APPDATA");
    std::string base = (a && *a) ? std::string(a) : home + "/AppData/Roaming";
    for (char& c : base) if (c == '\\') c = '/';
    return base + "/FileZilla/sitemanager.xml";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string path = std::string((xdg && *xdg) ? xdg : (home + "/.config").c_str()) +
                       "/filezilla/sitemanager.xml";
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        std::string old = home + "/.filezilla/sitemanager.xml";
        if (stat(old.c_str(), &st) == 0) return old;
    }
    return path;
#endif
}

bool net_filezilla_parse(const std::string& xml, std::vector<NetImported>* out,
                         int* skipped, std::string* err) {
    out->clear();
    *skipped = 0;
    if (err) err->clear();
    std::string why;
    XNode doc;
    if (!xml_parse(xml, &doc, &why)) {
        if (err) *err = "not a readable FileZilla file: " + why;
        return false;
    }
    const XNode* fz = doc.child("FileZilla3");
    const XNode* servers = fz ? fz->child("Servers") : nullptr;
    if (!servers) {
        if (err) *err = "not a FileZilla site manager file";
        return false;
    }
    fz_walk(*servers, "", out, skipped);
    return true;
}

bool net_filezilla_load(const std::string& path, std::vector<NetImported>* out,
                        int* skipped, std::string* err) {
    out->clear();
    *skipped = 0;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "no FileZilla site manager at " + path;
        return false;
    }
    std::string xml;
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) xml.append(buf, got);
    fclose(f);
    if (!xml.compare(0, 3, "\xEF\xBB\xBF")) xml.erase(0, 3);
    if (!net_filezilla_parse(xml, out, skipped, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    return true;
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
// Connection log
// ---------------------------------------------------------------------------

// One ring for the process, behind one mutex: lines arrive from the UI thread,
// the listing worker and every transfer thread, and the window reads them once
// a frame. A copy per frame would be wasteful, so a reader asks only for the
// lines past the last seq it saw.
static const size_t kNetLogMax = 4000;
static std::mutex g_log_mu;
static std::deque<NetLogLine> g_log;
static uint64_t g_log_seq = 0;

static int log_thread_number(void) {
    static std::atomic<int> next{1};
    static thread_local int mine = 0;
    if (!mine) mine = next++;
    return mine;
}

void net_log_add(const std::string& site, NetLogKind kind, const std::string& text) {
    NetLogLine l;
    l.when = time(nullptr);
    l.thread = log_thread_number();
    l.kind = kind;
    l.site = site;
    l.text = text;
    std::lock_guard<std::mutex> lk(g_log_mu);
    l.seq = ++g_log_seq;
    g_log.push_back(std::move(l));
    while (g_log.size() > kNetLogMax) g_log.pop_front();
}

uint64_t net_log_since(uint64_t after_seq, std::vector<NetLogLine>* out) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (out && !g_log.empty() && g_log.back().seq > after_seq) {
        // The ring is in seq order, so the first wanted line is found by its distance
        // from the back rather than by walking the whole ring.
        size_t newer = (size_t)(g_log.back().seq - after_seq);
        size_t start = newer >= g_log.size() ? 0 : g_log.size() - newer;
        for (size_t i = start; i < g_log.size(); i++) out->push_back(g_log[i]);
    }
    return g_log_seq;
}

void net_log_clear() {
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_log.clear();
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// The name a download writes before the transfer is known good. An aborted or
// failed transfer leaves it behind on purpose - it is what a resume continues
// from - and the real name is only ever reached by a rename. A crash therefore
// leaves a file that says it is a partial instead of a truncated file wearing the
// real name, which is the case a user cannot tell from looking at the pane.
std::string net_part_path(const char* local) { return std::string(local) + ".part"; }

long long net_part_size(const char* local) {
    struct stat st;
    std::string p = net_part_path(local);
    if (stat(p.c_str(), &st) != 0) return 0;
    return (long long)st.st_size;
}

#ifdef MC_HAVE_CURL

// curl_global_init is not thread safe and must happen once for the process. A
// background transfer runs on its own thread and reaches this on its first
// request, so the lazy flag it used to guard with is a race between that thread
// and whichever listing started first.
static std::once_flag g_curl_global_once;

// Every handle sets CURLOPT_NOSIGNAL, as threads need, and that also turns off
// curl's own guard against SIGPIPE. An older libcurl over OpenSSL (7.81, Ubuntu
// 22.04's) then writes to a socket the server already closed and the process
// dies of the signal. Ignoring it makes that write fail with EPIPE instead,
// which curl reports like any other dropped connection. The children the app
// starts put it back to the default before they exec.
static void curl_global_once(void) {
    std::call_once(g_curl_global_once, [] {
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

struct CachedHandle {
    std::string key;      // the site root: scheme, user, host, port
    CURL* h;
};

// A handle belongs to the thread that opened it: one CURL* cannot be used by two
// threads at once, and that is exactly the collision a background transfer
// creates, because the transfer runs on the site the pane behind it is listing.
// So the cache is per thread. The worker gets its own connection - it loses the
// login reuse the cache exists for, which is the price of not sharing, and it
// still reuses that connection across the files of one tree. The destructor is
// what returns a worker's handle to curl when its thread ends; nothing on the UI
// thread can reach it, and that is deliberate: net_forget() on the site list
// must not clean up a handle a transfer is still writing through.
struct HandleCache {
    std::vector<CachedHandle> entries;
    ~HandleCache() { for (CachedHandle& e : entries) curl_easy_cleanup(e.h); }
};

static HandleCache& handle_cache(void) {
    static thread_local HandleCache c;
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
    HandleCache& c = handle_cache();
    for (size_t i = 0; i < c.entries.size();) {
        if (key.empty() || c.entries[i].key == key) {
            curl_easy_cleanup(c.entries[i].h);
            c.entries.erase(c.entries.begin() + (long)i);
        } else {
            i++;
        }
    }
}

void net_forget_root(const char* root) {
    if (!root || !*root) return;   // an empty root is not a site, and "every site" is not what was asked
    HandleCache& c = handle_cache();
    for (size_t i = 0; i < c.entries.size();) {
        if (c.entries[i].key == root) {
            curl_easy_cleanup(c.entries[i].h);
            c.entries.erase(c.entries.begin() + (long)i);
        } else {
            i++;
        }
    }
}

// Only safe once every transfer thread has been joined: this closes the
// connections and then the library they belong to.
void net_shutdown(void) {
    net_forget(nullptr);
    curl_global_once();
    curl_global_cleanup();
}

// One handle per site per thread, reused across navigations. That is the whole
// reason this backend links a library: the second listing into the same server
// does not log in again or redo the TLS handshake.
// `reused` says whether the handle came from the cache, and so may be holding a
// connection the server has since dropped.
static CURL* handle_for(const NetSite& site, bool* reused = nullptr) {
    curl_global_once();
    std::string key = net_site_root(site);
    HandleCache& c = handle_cache();
    if (reused) *reused = false;
    for (CachedHandle& e : c.entries)
        if (e.key == key) { if (reused) *reused = true; return e.h; }
    CURL* h = curl_easy_init();
    if (!h) return nullptr;
    c.entries.push_back({ key, h });
    return h;
}

// The site the calling thread's handle is working for, for the log lines its
// debug callback writes. The callback runs inside curl_easy_perform() on the same
// thread, so a thread_local is all the context it needs.
static thread_local std::string t_log_site;

// curl's verbose output, filtered into the connection log. Only the control
// channel and curl's narration go in - the data and the TLS records would bury
// the one line that says where the wait is. A chunk can hold several lines.
static int log_debug_cb(CURL*, curl_infotype type, char* data, size_t size, void*) {
    NetLogKind kind;
    switch (type) {
        case CURLINFO_TEXT:       kind = NETLOG_INFO; break;
        case CURLINFO_HEADER_OUT: kind = NETLOG_SENT; break;
        case CURLINFO_HEADER_IN:  kind = NETLOG_RECV; break;
        default: return 0;
    }
    size_t pos = 0;
    while (pos < size) {
        size_t end = pos;
        while (end < size && data[end] != '\n') end++;
        std::string line(data + pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (kind == NETLOG_SENT && !strncasecmp(line.c_str(), "PASS ", 5)) line = "PASS ****";
        net_log_add(t_log_site, kind, line);
    }
    return 0;
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
    t_log_site = net_site_root(site);
    curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, log_debug_cb);
    curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
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
    std::string e = (errbuf && errbuf[0]) ? errbuf : curl_easy_strerror(rc);
    net_log_add(t_log_site, NETLOG_ERROR, e);
    return e;
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
                    std::vector<FileEntry>* out, std::string* err,
                    fs_progress_fn cb, void* user, bool* give_up) {
    std::string url = dir_url(site, rpath);
    char errbuf[CURL_ERROR_SIZE];
    LineSink sink;
    PerformCtx pctx = { cb, user, false, false };

    // Set up twice when the first attempt fails, so it is one function.
    auto prepare = [&](CURL* h) {
        setup_handle(h, site, url, errbuf, sizeof(errbuf));
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, cmd);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect_lines_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
        if (cb) {
            curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
            curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pctx);
            curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        }
    };

    bool reused = false;
    CURL* h = handle_for(site, &reused);
    if (!h) { if (err) *err = "out of memory"; return false; }
    prepare(h);
    net_log_add(net_site_root(site), NETLOG_INFO,
                std::string("Listing ") + (rpath.empty() ? "/" : rpath) + " with " + cmd);

    CURLcode rc = curl_easy_perform(h);
    // A cached connection the server has since dropped fails on first use, so a
    // failure on a reused handle gets one retry on a fresh one - unless the caller
    // asked to stop. A fresh connection that failed is not retried: against a
    // host that does not answer, that only doubles the wait.
    if (rc != CURLE_OK && !pctx.aborted && reused) {
        net_log_add(net_site_root(site), NETLOG_INFO,
                    std::string("Failed (") + curl_easy_strerror(rc) + "), retrying on a new connection");
        net_forget(&site);
        h = handle_for(site);
        if (!h) { if (err) *err = "out of memory"; return false; }
        sink.text.clear();
        prepare(h);
        rc = curl_easy_perform(h);
    }
    if (pctx.aborted) {
        if (give_up) *give_up = true;
        // Half a conversation is not something to reuse: the next listing starts
        // on a fresh connection.
        net_forget(&site);
        net_log_add(net_site_root(site), NETLOG_INFO, "Cancelled");
        if (err) *err = "cancelled";
        return false;
    }
    if (rc != CURLE_OK) {
        // A failure to reach the server or to log in is not something a different
        // listing command can fix, so the caller does not try one.
        switch (rc) {
            case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_CONNECT:
            case CURLE_OPERATION_TIMEDOUT:   case CURLE_LOGIN_DENIED:
            case CURLE_SSL_CONNECT_ERROR:    case CURLE_PEER_FAILED_VERIFICATION:
            case CURLE_USE_SSL_FAILED:
                if (give_up) *give_up = true;
                // Nothing on this handle is worth keeping, and a handle whose
                // handshake failed can carry that into the next request: the
                // next listing starts from a fresh one.
                net_forget(&site);
                break;
            default:
                break;
        }
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
              std::vector<FileEntry>* out, std::string* err,
              fs_progress_fn cb, void* user) {
    out->clear();
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    bool give_up = false;
    // MLSD first, and a LIST whenever MLSD produced nothing: an empty answer is
    // either a genuinely empty directory or a server that ignored the command,
    // and only a LIST can tell them apart. A server that could not be reached at
    // all gets no second command.
    if (site.proto != NET_SFTP) {
        std::string mlsd_err;
        if (listing(site, rpath, "MLSD", out, &mlsd_err, cb, user, &give_up) && !out->empty())
            return true;
        if (give_up) { if (err) *err = mlsd_err; return false; }
    }
    std::string list_err;
    if (listing(site, rpath, "LIST", out, &list_err, cb, user, &give_up)) return true;
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
    long long wrote;   // bytes this call actually stored, whatever else happened
};

static size_t write_file_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    FileSink* s = (FileSink*)user;
    size_t n = fwrite(ptr, size, nmemb, s->f);
    s->wrote += (long long)(n * size);   // fwrite counts items, not bytes
    return n;
}

static size_t read_file_cb(char* dest, size_t size, size_t nmemb, void* user) {
    FileSink* s = (FileSink*)user;
    return fread(dest, size, nmemb, s->f);
}

bool net_download(const NetSite& site, const std::string& rpath, const char* local,
                  fs_progress_fn cb, void* user, std::string* err,
                  long long resume_from, long long* got) {
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    std::string tmp = net_part_path(local);
    // A restart starts the sidecar empty and a resume continues the one that is
    // already there. Clearing it here is what makes "restart" mean starting over
    // rather than downloading on top of a half file.
    if (resume_from <= 0) remove(tmp.c_str());
    FILE* f = fopen(tmp.c_str(), resume_from > 0 ? "ab" : "wb");
    if (!f) { if (err) *err = std::string("cannot write ") + tmp + ": " + strerror(errno); return false; }
    std::string url = curl_url(site, rpath);
    char errbuf[CURL_ERROR_SIZE];
    PerformCtx pctx = { cb, user, false, false };
    CURL* h = handle_for(site);
    bool ok = false;
    if (!h) { fclose(f); if (err) *err = "out of memory"; return false; }
    setup_handle(h, site, url, errbuf, sizeof(errbuf));
    FileSink sink = { f, 0 };
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    // The REST that a resume is: curl sends it before the RETR and the server
    // answers with the starting point instead of the whole file.
    if (resume_from > 0)
        curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) net_forget(&site);
    ok = (rc == CURLE_OK);
    fclose(f);
    if (got) *got = sink.wrote;

    // What the sidecar actually ended up being: the only number that can tell a
    // resumed transfer from a restarted one.
    long long final_size = 0;
    struct stat st;
    if (stat(tmp.c_str(), &st) == 0) final_size = (long long)st.st_size;

    // No length check here, which is a finding and not an omission. curl asks the
    // server for the size, subtracts the offset it put in REST, and stops reading at
    // that many bytes: an honest resume lands on the remote size by construction. A
    // server that refuses REST, or that is asked to resume past its end, says so and
    // curl hands the reason back ("Offset (5000) was beyond file size (12)"). The
    // one shape a length check would catch is a remote file that changed size between
    // the listing and the transfer - and there the file curl produced is complete, so
    // throwing it away would be the wrong answer. Whether the bytes are the *right*
    // bytes is a checksum question, which is what the SFV check is for.
    if (!ok) {
        // Nothing arrived, so there is nothing to keep. This is the same promise
        // the old `remove(local)` made: a failed transfer does not leave a file
        // where the user will open it.
        if (final_size == 0) remove(tmp.c_str());
        if (err) *err = pctx.aborted ? std::string("aborted") : curl_error(rc, errbuf);
        return false;
    }
    // The real name is reached by rename, atomic on the same filesystem: what sits
    // under it is either the complete file or it is not there.
    if (rename(tmp.c_str(), local) != 0) {
        if (err) *err = std::string("cannot put ") + local + " in place: " + strerror(errno);
        return false;
    }
    return true;
}

struct PrefixSink {
    std::string* out;
    size_t max;
    bool full;
};

// Returning anything other than the chunk's length is how a write callback tells
// curl to abandon a transfer, which is the only way to stop an FTP retrieve in
// the middle: the protocol has no range that ends early.
static size_t write_prefix_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    PrefixSink* s = (PrefixSink*)user;
    size_t got = size * nmemb;
    size_t room = s->max - s->out->size();
    if (got > room) got = room;
    s->out->append(ptr, got);
    if (s->out->size() >= s->max) { s->full = true; return 0; }
    return got;
}

bool net_download_prefix(const NetSite& site, const std::string& rpath, std::string* out,
                         size_t max_bytes, fs_progress_fn cb, void* user, std::string* err) {
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    out->clear();
    if (!max_bytes) return true;
    std::string url = curl_url(site, rpath);
    char errbuf[CURL_ERROR_SIZE];
    PerformCtx pctx = { cb, user, false, false };
    CURL* h = handle_for(site);
    if (!h) { if (err) *err = "out of memory"; return false; }
    setup_handle(h, site, url, errbuf, sizeof errbuf);
    PrefixSink sink = { out, max_bytes, false };
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_prefix_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(h);
    // A transfer abandoned on purpose still leaves the control connection inside
    // a retrieve, so the handle goes the same way it does after a real failure.
    if (rc != CURLE_OK) net_forget(&site);
    if (rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && sink.full)) return true;
    if (err) *err = pctx.aborted ? std::string("aborted") : curl_error(rc, errbuf);
    out->clear();
    return false;
}

bool net_upload(const NetSite& site, const std::string& rpath, const char* local,
                fs_progress_fn cb, void* user, std::string* err,
                long long resume_from) {
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    FILE* f = fopen(local, "rb");
    if (!f) { if (err) *err = std::string("cannot read ") + local + ": " + strerror(errno); return false; }
    struct stat st;
    long long size = -1;
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) size = (long long)st.st_size;
    // A resume over FTP is an append: the offset is where in the local source
    // libcurl starts reading and the server continues the file it already has. So
    // the size the server is told is what is left to send, not the whole file.
    if (resume_from > 0 && size >= 0 && resume_from > size) resume_from = 0;
    // curl resumes an FTP upload with APPE rather than REST + STOR. With our own
    // read callback there is no FILE* for curl to seek, so the remainder starts
    // here, and INFILESIZE below is the remainder too.
    if (resume_from > 0) fseeko(f, (off_t)resume_from, SEEK_SET);

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
    FileSink sink = { f, 0 };
    curl_easy_setopt(h, CURLOPT_READFUNCTION, read_file_cb);
    curl_easy_setopt(h, CURLOPT_READDATA, &sink);
    // curl wants the size of the local source, not the size of the remainder: it
    // subtracts the resume offset itself. Telling it the remainder and the offset
    // is what makes a resume upload nothing and call that a success.
    if (size >= 0)
        curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
    if (resume_from > 0)
        curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
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
    // The check an upload resume does need. A resume over FTP is an APPE, so the
    // only way the append can go wrong is by not landing, and the way to see that is
    // to ask the server how big the file is now: it has to be the size of the local
    // source. This is not a formality - a resume that told curl the size of the
    // remainder made curl transfer nothing, report success, and leave the server
    // holding half a file.
    if (resume_from > 0) {
        FileEntry me;
        std::string serr;
        if (net_stat(site, rpath, &me, &serr) && (long long)me.size != size) {
            if (err) *err = "the server did not store the whole file - start again";
            return false;
        }
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
// A component that is already a directory is fine however mkdir refused it:
// Windows answers "access denied", not "exists", for a drive like D: or a folder
// the user may not write to, and those are on the way to most destinations.
static bool make_local_dir(const char* path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    int saved = errno;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    errno = saved;
    return false;
}

static bool make_local_dirs(const char* path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!make_local_dir(buf)) return false;
        *p = '/';
    }
    return make_local_dir(buf);
}

// FTP has no recursive retrieve either: a directory is one mkdir and one fetch
// per entry, taken from a listing.
bool net_download_tree(const NetSite& site, const std::string& rpath, const char* local,
                       fs_progress_fn cb, void* user, std::string* err, bool resume) {
    FileEntry me;
    std::string stat_err;
    if (!net_stat(site, rpath, &me, &stat_err) || !S_ISDIR(me.mode))
        return net_download(site, rpath, local, cb, user, err,
                            resume ? net_part_size(local) : 0, nullptr);

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
        // Each entry resumes from its own sidecar: a tree that died on the fourth
        // file continues the fourth and re-runs the ones after it. The listing
        // already says which entries are directories, so the walk does not go and
        // ask the server the same question per entry.
        if (S_ISDIR(e.mode)) {
            if (!net_download_tree(site, child, child_local, cb, user, err, resume)) return false;
        } else {
            long long from = resume ? net_part_size(child_local) : 0;
            if (!net_download(site, child, child_local, cb, user, err, from, nullptr)) return false;
        }
    }
    return true;
}

// The mirror image: a local folder goes up as MKD plus one store per entry. An
// existing remote directory is kept rather than failed on, because uploading
// over a folder that is already there is an overwrite, not an error.
bool net_upload_tree(const NetSite& site, const char* local, const std::string& rpath,
                     fs_progress_fn cb, void* user, std::string* err, bool resume) {
    struct stat st;
    if (stat(local, &st) != 0) {
        if (err) *err = std::string(local) + ": " + strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        // A resumed store continues the file the server already has, so the offset
        // is the size the server says that file has. Without a number there is
        // nothing to continue from and the store starts over.
        long long from = 0;
        if (resume) {
            FileEntry there;
            std::string serr;
            if (net_stat(site, rpath, &there, &serr) && S_ISREG(there.mode))
                from = (long long)there.size;
        }
        return net_upload(site, rpath, local, cb, user, err, from);
    }

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
        if (!net_upload_tree(site, child_local.c_str(), child_remote, cb, user, err, resume)) { ok = false; break; }
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
void net_forget_root(const char*) {}
void net_shutdown(void) {}

static const char* const kNoCurl = "this build has no FTP support: install libcurl and rebuild";

static bool no_curl(std::string* err) {
    if (err) *err = kNoCurl;
    return false;
}

bool net_list(const NetSite&, const std::string&, std::vector<FileEntry>* out, std::string* err,
              fs_progress_fn, void*) {
    out->clear();
    return no_curl(err);
}
bool net_stat(const NetSite&, const std::string&, FileEntry*, std::string* err) { return no_curl(err); }
bool net_download(const NetSite&, const std::string&, const char*, fs_progress_fn, void*, std::string* err,
                  long long, long long*) { return no_curl(err); }
bool net_upload(const NetSite&, const std::string&, const char*, fs_progress_fn, void*, std::string* err,
                long long) { return no_curl(err); }
bool net_download_prefix(const NetSite&, const std::string&, std::string* out, size_t, fs_progress_fn, void*, std::string* err) {
    out->clear();
    return no_curl(err);
}
bool net_mkdir(const NetSite&, const std::string&, std::string* err) { return no_curl(err); }
bool net_remove(const NetSite&, const std::string&, bool, std::string* err) { return no_curl(err); }
bool net_remove_tree(const NetSite&, const std::string&, fs_progress_fn, void*, std::string* err) { return no_curl(err); }
bool net_download_tree(const NetSite&, const std::string&, const char*, fs_progress_fn, void*, std::string* err, bool) { return no_curl(err); }
bool net_upload_tree(const NetSite&, const char*, const std::string&, fs_progress_fn, void*, std::string* err, bool) { return no_curl(err); }
bool net_rename(const NetSite&, const std::string&, const char*, std::string* err) { return no_curl(err); }
bool net_chmod(const NetSite&, const std::string&, int, std::string* err) { return no_curl(err); }

#endif // MC_HAVE_CURL
