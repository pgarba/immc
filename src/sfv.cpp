#include "sfv.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <algorithm>

// The reflected form of the CRC-32 polynomial, which is the form every .sfv writer
// means: the same table zlib builds, the same result `crc32(1)` prints, and the
// check value 0xCBF43926 for the string "123456789".
static unsigned long g_crc_table[256];
static bool g_crc_table_ready = false;

static void crc_table_build(void) {
    if (g_crc_table_ready) return;
    for (unsigned long i = 0; i < 256; i++) {
        unsigned long c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1UL) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_table_ready = true;
}

unsigned long sfv_crc32(const void* data, size_t n) {
    crc_table_build();
    const unsigned char* p = (const unsigned char*)data;
    unsigned long crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; i++)
        crc = g_crc_table[(crc ^ p[i]) & 0xFFUL] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

bool sfv_crc32_file(const char* path, unsigned long* out, std::string* err) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err) *err = std::string(path) + ": " + strerror(errno);
        return false;
    }
    crc_table_build();
    unsigned long crc = 0xFFFFFFFFUL;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < got; i++)
            crc = g_crc_table[(crc ^ buf[i]) & 0xFFUL] ^ (crc >> 8);
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) {
        if (err) *err = std::string(path) + ": read failed";
        return false;
    }
    if (out) *out = crc ^ 0xFFFFFFFFUL;
    return true;
}

void sfv_format_crc(unsigned long crc, char* out, size_t n) {
    snprintf(out, n, "%08lX", crc & 0xFFFFFFFFUL);
}

// Eight hex digits, and nothing else. A token that is eight hex digits is the
// checksum on a line, whichever side of the name it was written on; anything else
// is part of the name, which is how a file called `deadbeef` survives.
static bool is_crc_token(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s)
        if (!isxdigit((unsigned char)c)) return false;
    return true;
}

static unsigned long crc_of_token(const std::string& s) {
    return strtoul(s.c_str(), nullptr, 16);
}

// Trailing whitespace, then a trailing CR: an .sfv written on Windows and read on
// this machine has one on every line, and a name that ends in `\r` is a file that
// does not exist.
static void trim_line(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || isspace((unsigned char)s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    if (i) s.erase(0, i);
}

// The name is everything before the last run of whitespace and the checksum is that
// last token, because names with spaces in them are legal and common: `The Set
// (1998)/disc 1.bin 1A2B3C4D` is one entry, not a malformed one.
//
// Some writers put the checksum first. That is recognised only when the last token
// is not a checksum and the first one is, so a file genuinely named `12345678`
// still parses as a name.
static void parse_line(const std::string& line, SfvParse* out) {
    if (line.empty()) return;
    if (line[0] == ';' || line[0] == '#') return;   // a whole-line comment

    size_t end = line.size();
    size_t k = end;
    while (k > 0 && !isspace((unsigned char)line[k - 1])) k--;   // back over the last token
    std::string last = line.substr(k, end - k);
    std::string rest = line.substr(0, k);
    while (!rest.empty() && isspace((unsigned char)rest.back())) rest.pop_back();

    if (is_crc_token(last) && !rest.empty()) {
        SfvEntry e;
        e.name = rest;
        e.crc = crc_of_token(last);
        out->entries.push_back(e);
        return;
    }
    // The other way round: a leading checksum and a name that may itself contain
    // spaces, so the name is everything after the first run of whitespace.
    size_t sp = line.find_first_of(" \t");
    if (sp != std::string::npos && is_crc_token(line.substr(0, sp))) {
        std::string name = line.substr(sp + 1);
        while (!name.empty() && isspace((unsigned char)name.front())) name.erase(0, 1);
        if (!name.empty()) {
            SfvEntry e;
            e.name = name;
            e.crc = crc_of_token(line.substr(0, sp));
            e.crc_first = true;
            out->entries.push_back(e);
            out->crc_first++;
            return;
        }
    }
    out->malformed++;
}

// `folder\file.rar` is what a Windows writer produces and is not a name on this
// filesystem. The separator is translated rather than the entry dropped, because
// dropping it would report a file that is present and correct as missing.
static void translate_separators(std::string& name) {
    for (char& c : name)
        if (c == '\\') c = '/';
}

// An .sfv that lists itself is common, and verifying it against itself is not a
// check: the file would match whatever it contains. Both spellings are compared -
// the path it was opened by and the bare name - because writers differ.
static bool names_self(const std::string& name, const std::string& self) {
    if (name.empty() || self.empty()) return false;
    if (strcasecmp(name.c_str(), self.c_str()) == 0) return true;
    size_t slash = self.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? self : self.substr(slash + 1);
    size_t nslash = name.find_last_of('/');
    const std::string nbase = (nslash == std::string::npos) ? name : name.substr(nslash + 1);
    return strcasecmp(nbase.c_str(), base.c_str()) == 0;
}

bool sfv_parse(const char* path, const char* self, SfvParse* out, std::string* err) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err) *err = std::string(path) + ": " + strerror(errno);
        return false;
    }
    out->entries.clear();
    out->malformed = 0;
    out->self_named = 0;
    out->crc_first = 0;

    std::string self_name = self && *self ? self : (path ? path : "");
    char buf[4096];
    std::string carry;
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        // A line longer than the buffer arrives in pieces: the pieces are joined
        // rather than each one parsed as its own entry, which would turn one long
        // name into a name and a malformed line.
        if (line.empty()) break;
        if (line.back() != '\n') { carry += line; continue; }
        line = carry + line;
        carry.clear();
        trim_line(line);
        if (line.empty()) continue;
        parse_line(line, out);
    }
    if (!carry.empty()) { trim_line(carry); if (!carry.empty()) parse_line(carry, out); }
    fclose(f);

    for (size_t i = 0; i < out->entries.size();) {
        translate_separators(out->entries[i].name);
        if (out->entries[i].name.empty() || names_self(out->entries[i].name, self_name)) {
            out->self_named++;
            out->entries.erase(out->entries.begin() + (long)i);
        } else {
            i++;
        }
    }
    return true;
}

bool sfv_list(const char* dir, std::vector<std::string>* out) {
    DIR* d = opendir(dir);
    if (!d) return false;
    std::vector<std::string> names;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        const char* name = de->d_name;
        size_t n = strlen(name);
        if (n < 4) continue;
        if (strcasecmp(name + n - 4, ".sfv") != 0) continue;
        names.push_back(name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    if (out) *out = names;
    return !names.empty();
}

bool sfv_find(const char* dir, std::string* out) {
    std::vector<std::string> names;
    if (!sfv_list(dir, &names)) return false;
    std::string best = names[0];
    for (const std::string& n : names)
        if (n.size() < best.size()) best = n;
    if (out) *out = std::string(dir) + "/" + best;
    return true;
}
