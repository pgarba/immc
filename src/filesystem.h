#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <sys/types.h>
#include <time.h>
#include <vector>

// File entry for a single file or directory
typedef struct {
    char name[256];
    mode_t mode;
    off_t size;
    time_t mtime;
} FileEntry;

// Sort modes for directory listings. Direct keys: Ctrl+F3 name, Ctrl+F4 ext,
// Ctrl+F5 date, Ctrl+F6 size, Ctrl+F7 unsorted (readdir order, no qsort).
enum FsSortMode { FS_SORT_NAME = 0, FS_SORT_EXT, FS_SORT_SIZE, FS_SORT_DATE,
                  FS_SORT_NONE };
void fs_set_sort_mode(int mode);
int  fs_get_sort_mode();
void fs_set_show_hidden(bool show);   // Alt+. toggles dotfiles
bool fs_get_show_hidden();

// Create a new file entry
FileEntry* file_entry_new(const char* name, mode_t mode, off_t size, time_t mtime);

// Free a file entry
void file_entry_free(FileEntry* entry);

// List directory contents. Caller passes address of a FileEntry** variable.
void list_directory(const char* path, FileEntry*** out_entries, int* out_count);

// Free all entries in a list
void free_entries(FileEntry** entries, int count);

// Check if entry is a directory
bool is_directory(const char* name, mode_t mode);

// Check if entry is executable
bool is_executable(const char* name, mode_t mode);

// File operations (return true on success)
bool fs_copy_entry(const char* src, const char* dst);   // recursive for directories
bool fs_delete_entry(const char* path);                 // recursive for directories
bool fs_move_entry(const char* src, const char* dst);   // rename, fallback copy+delete
bool fs_make_dir(const char* path);
bool fs_exists(const char* path);
bool fs_touch(const char* path);                  // create an empty file, fails if it exists
bool fs_symlink(const char* src, const char* dst); // link at dst pointing at src
bool fs_trash(const char* path);                  // hand the file to a desktop trash helper
bool fs_trash_available(void);                    // is such a helper installed at all?

// One entry of a recursive (TC branch view) listing. `rel` is the path relative
// to the root that was walked, with '/' separators.
typedef struct {
    char rel[1024];
    mode_t mode;
    off_t size;
    time_t mtime;
} FsTreeEntry;

// Walk `root` recursively and fill `out`, sorted by rel. Dotfiles follow
// fs_get_show_hidden(); symlinks are never followed. Stops after max_depth
// levels below the root and at max_entries rows. Returns the row count.
int fs_list_tree(const char* root, std::vector<FsTreeEntry>* out,
                 int max_depth, int max_entries);

// Format a mode as an rwx string, e.g. "rw-r--r--"
void format_mode(mode_t mode, char* out);

// Read up to max_bytes of a file into buf (NUL-terminated). Returns bytes read,
// or -1 if the file is binary (contains NUL) or unreadable.
long fs_read_preview(const char* path, char* buf, size_t max_bytes);
long fs_hex_dump(const char* path, char* out, size_t out_size, long max_bytes);
void fs_count_tree(const char* path, long long* total_size, long* nfiles, long* ndirs);

// Progress callback for copy/move: called after each unit of work with the
// bytes/files completed and the totals (computed up-front with fs_count_tree).
// Return false to abort the operation.
typedef bool (*fs_progress_fn)(long long done_bytes, long long total_bytes,
                               int done_files, int total_files, void* user);
bool fs_copy_entry_progress(const char* src, const char* dst, fs_progress_fn cb, void* user);
bool fs_move_entry_progress(const char* src, const char* dst, fs_progress_fn cb, void* user);
bool fs_delete_entry_progress(const char* path, fs_progress_fn cb, void* user);

#endif // FILESYSTEM_H
