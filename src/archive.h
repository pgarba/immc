#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <sys/types.h>
#include <time.h>
#include <string>
#include <vector>

// Archives we can drive through a command-line tool. The app links no archive
// library: every operation shells out to zip/unzip, tar or 7z, which keeps the
// dependency surface where the trash and viewer helpers already put it
// (gio trash, xdg-open).
enum ArchKind {
    ARCH_NONE = 0,
    ARCH_ZIP,      // zip / unzip
    ARCH_TAR,      // tar
    ARCH_TAR_GZ,   // tar.gz, tgz
    ARCH_TAR_BZ2,  // tar.bz2, tbz2
    ARCH_TAR_XZ,   // tar.xz, txz
    ARCH_7Z,       // 7z
    ARCH_RAR,      // rar, read-only through 7z's bundled decoder
    ARCH_KIND_COUNT
};

// One member of an archive.
//   rel -- normalized for display and for building the pane's virtual tree:
//          '/' separators, no leading "./", no trailing '/' on directories.
//   raw -- exactly what the listing tool reported, which is what has to be
//          handed back to it for extraction. tar stores members as "./a/b",
//          so rel and raw differ there and passing rel back would miss them.
typedef struct {
    char rel[1024];
    char raw[1024];
    mode_t mode;
    off_t size;
    time_t mtime;
} ArchEntry;

// Same contract as fs_progress_fn: called repeatedly with the running totals,
// return false to abort. Progress from a CLI tool is only as fine as that
// tool's output stream: zip, unzip and tar print one line per member, so the
// bar is per-file; 7z prints a percentage instead, so the bar is per-percent.
typedef bool (*arch_progress_fn)(long long done_bytes, long long total_bytes,
                                 int done_files, int total_files, void* user);

const char* arch_kind_name(ArchKind k);

// Classify by extension. ARCH_NONE for anything we have no tool for.
ArchKind arch_kind_of(const char* path);
inline ArchKind arch_kind_of(const std::string& s) { return arch_kind_of(s.c_str()); }
bool arch_is_archive(const char* path);

// Is the tool that handles this kind installed? Cached after the first lookup.
bool arch_tool_available(ArchKind k);
// Can this kind be written at all? rar has no writer, so it is read-only.
bool arch_can_pack(ArchKind k);
// Does extraction of single members work? Yes for every kind we support.
bool arch_can_extract(ArchKind k);

// List every member of `archive`, files and directories, sorted by rel.
// Directory rows are taken from the tool where it reports them; a tool that
// lists files only would leave the pane unable to show folders, so any
// intermediate path component is synthesized as well.
// Returns false and fills `err` when the listing could not be produced.
bool arch_list(const char* archive, std::vector<ArchEntry>* out, std::string* err);

// Extract the whole archive into `dest` (which must exist).
bool arch_extract(const char* archive, const char* dest,
                  arch_progress_fn cb, void* user, std::string* err);

// Extract the named members, given as `raw` paths from a listing, into `dest`.
bool arch_extract_members(const char* archive, const char* dest,
                          const std::vector<std::string>& raw_members,
                          arch_progress_fn cb, void* user, std::string* err);

// Create `archive` from `rel_sources`, resolved against `cwd`. The kind comes
// from the archive's extension.
bool arch_pack(const char* archive, const char* cwd,
               const std::vector<std::string>& rel_sources,
               arch_progress_fn cb, void* user, std::string* err);

// Integrity test (TC Alt+Shift+F9).
bool arch_test(const char* archive, arch_progress_fn cb, void* user, std::string* err);

// Extract one member to a temporary file and return its path, for viewing an
// archive member without unpacking the whole thing. `out_dir` receives the
// temporary directory holding it, which the caller removes when it is done.
bool arch_extract_temp(const char* archive, const char* raw_member,
                       char* out_path, size_t cap,
                       char* out_dir, size_t dcap, std::string* err);

// Split a virtual archive path ("/real/a.zip/inner/dir") into the archive file
// and the member prefix inside it. A path counts as virtual as soon as one of
// its components is an existing regular file with an archive extension. That
// lets tabs, directory history, the path bar and the hotlist carry an archive
// location without knowing anything about archives. Ordinary directories are
// never shadowed: a real directory named "x.zip" stays a directory.
bool arch_split_virtual(const char* path, char* archive, size_t acap,
                        char* prefix, size_t pcap);

// Look up a member by its display path. Returns null when the listing has no
// such member.
const ArchEntry* arch_find(const std::vector<ArchEntry>& listing, const char* rel);

#endif // ARCHIVE_H
