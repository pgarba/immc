#ifndef SFV_H
#define SFV_H

#include <string>
#include <vector>
#include <stddef.h>

// .sfv verification: the text file that arrives beside a download and names every
// file in the set with its CRC-32.
//
// CRC-32 here is the reflected 32-bit polynomial 0xEDB88320 - the one zlib uses -
// computed with a 256-entry table built once on first use. The app does not link
// zlib today: only libcurl pulls it in, and the build configuration without libcurl
// has to keep working, so a checksum this small is not allowed to decide whether
// the app builds.
//
// What this module does not do is MD5. An .sfv carries CRC-32; a `.md5` file beside
// it is a different format with a different algorithm, and pretending otherwise
// would report a file as verified against a checksum this code never computed.

struct SfvEntry {
    std::string name;      // path relative to the .sfv's own directory, '/' separated
    unsigned long crc;     // as the file wrote it
    bool crc_first = false; // this line was written "CRC name" rather than "name CRC"
};

struct SfvParse {
    std::vector<SfvEntry> entries;
    int malformed = 0;   // lines that are neither a comment nor a name with a checksum
    int self_named = 0;  // lines naming the .sfv itself, which are dropped
    int crc_first = 0;   // lines the other way round, which are accepted
};

// Parse an .sfv. Comments (`;` and `#` at the start of a line), blank lines, CRLF
// endings, trailing spaces, names containing spaces, `folder\file` separators, and
// writers that put the checksum first are all handled. `self` is the .sfv's own
// path: an .sfv never verifies itself, and a file that lists itself is common
// enough that dropping it has to be a rule rather than a surprise.
bool sfv_parse(const char* path, const char* self, SfvParse* out, std::string* err);

// CRC-32 of a buffer, and of a file read in 64 KB chunks. The file form returns
// false with a reason when the file cannot be opened, which is a different answer
// from "the checksum does not match" and has to stay different.
unsigned long sfv_crc32(const void* data, size_t n);
bool sfv_crc32_file(const char* path, unsigned long* out, std::string* err);

// The eight hex digits an .sfv writes. Uppercase, because that is what the writers
// emit and what a user comparing two files by eye expects to see.
void sfv_format_crc(unsigned long crc, char* out, size_t n);

// Every .sfv in a directory, sorted by name. A caller that prefers the one named
// after what it transferred needs the list; `sfv_find` is the shortest name in it.
bool sfv_list(const char* dir, std::vector<std::string>* out);

// The .sfv that belongs to a directory, if it holds one: the shortest-named one,
// which is the one a set ships. Returns false when the directory has none or
// cannot be read.
bool sfv_find(const char* dir, std::string* out);

#endif // SFV_H
