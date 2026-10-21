#include "text.h"

#include <string.h>

// CP437's upper half. Index 0 is the byte 0x80.
//
// Generated from Python's cp437 codec rather than typed out of a table, because the
// two code pages people confuse are within a few rows of each other: CP437 has no `ã`
// at 0x84 - that is Latin-1 - and CP437's 0x84 is `ä`. A mistyped row in a transcode
// produces a wrong character that looks like a right one, which is the worst kind of
// bug in a feature whose whole job is showing characters.
//
// The rows that matter here are 0xB0 to 0xDF, checked against the anchors everyone
// remembers: 0xB3 is │, 0xC4 is ─, 0xDB is █, 0xDF is ▀.
static const unsigned short kCp437Upper[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static void utf8_push(std::string* out, unsigned int cp) {
    if (cp < 0x80) { out->push_back((char)cp); return; }
    if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
        return;
    }
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
}

bool text_is_utf8(const std::string& bytes) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char b = p[i];
        if (b < 0x80) { i++; continue; }

        int extra;
        unsigned int cp;
        if ((b & 0xE0) == 0xC0)      { extra = 1; cp = b & 0x1Fu; }
        else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0Fu; }
        else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07u; }
        else return false;   // a bare continuation byte, or 0xF8 and above

        // Cut off by the preview's byte cap. Half a character is not a character,
        // and saying so is what keeps a truncated UTF-8 file from being read as
        // something else.
        if (i + (size_t)extra >= n) return false;

        for (int k = 1; k <= extra; k++) {
            const unsigned char c = p[i + (size_t)k];
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        // Over-long encodings and the surrogate range are not well-formed UTF-8,
        // and a file that uses them is not a file to guess about.
        if (extra == 1 && cp < 0x80u) return false;
        if (extra == 2 && cp < 0x800u) return false;
        if (extra == 3 && cp < 0x10000u) return false;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
        if (cp > 0x10FFFFu) return false;

        i += (size_t)extra + 1;
    }
    return true;
}

bool text_byte_is_cp437_art(unsigned char b) {
    return b >= 0xB0 && b <= 0xDF;
}

void text_cp437_art_shape(const std::string& bytes, int* total, int* distinct, int* longest_run) {
    bool seen[256] = {};
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    int t = 0, run = 0, best = 0, diff = 0;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (!text_byte_is_cp437_art(p[i])) { run = 0; continue; }
        t++;
        run++;
        if (run > best) best = run;
        if (!seen[p[i]]) { seen[p[i]] = true; diff++; }
    }
    if (total) *total = t;
    if (distinct) *distinct = diff;
    if (longest_run) *longest_run = best;
}

bool text_looks_cp437_art(const std::string& bytes) {
    // UTF-8 wins outright. A file that is well-formed UTF-8 has an alphabet already
    // and there is nothing to guess.
    if (text_is_utf8(bytes)) return false;

    int total = 0, distinct = 0, run = 0;
    text_cp437_art_shape(bytes, &total, &distinct, &run);
    if (total < 4) return false;

    // A border is a run: `────`, `░░░░`, `████`. Prose is not - four accented letters
    // in a row do not happen in a Western European text file, which is the false
    // positive this whole rule exists to avoid. The distinctness half catches the
    // other shape ANSI art takes, a table where box characters frame ASCII words and
    // never sit next to each other.
    return run >= 4 || distinct >= 6;
}

TextCodepage text_decide_codepage(const std::string& bytes, TextCodepage requested) {
    switch (requested) {
        case TEXT_CODEPAGE_UTF8:  return TEXT_CODEPAGE_UTF8;
        case TEXT_CODEPAGE_CP437: return TEXT_CODEPAGE_CP437;
        default: break;
    }
    return text_looks_cp437_art(bytes) ? TEXT_CODEPAGE_CP437 : TEXT_CODEPAGE_UTF8;
}

std::string text_cp437_to_utf8(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 4);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    for (size_t i = 0; i < bytes.size(); i++) {
        if (p[i] < 0x80) { out.push_back((char)p[i]); continue; }
        utf8_push(&out, kCp437Upper[p[i] - 0x80]);
    }
    return out;
}

TextCodepage text_codepage_from_name(const char* name) {
    if (!name) return TEXT_CODEPAGE_AUTO;
    if (!strcmp(name, "utf8") || !strcmp(name, "utf-8") || !strcmp(name, "utf-8"))
        return TEXT_CODEPAGE_UTF8;
    if (!strcmp(name, "cp437") || !strcmp(name, "437"))
        return TEXT_CODEPAGE_CP437;
    return TEXT_CODEPAGE_AUTO;   // "auto", a typo, or an empty line: the default that looks
}

const char* text_codepage_name(TextCodepage page) {
    switch (page) {
        case TEXT_CODEPAGE_UTF8:  return "utf8";
        case TEXT_CODEPAGE_CP437: return "cp437";
        default:                  return "auto";
    }
}
