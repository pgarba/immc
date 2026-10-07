#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <sys/types.h>
#include <time.h>

// File entry for a single file or directory
typedef struct {
    char name[256];
    mode_t mode;
    off_t size;
    time_t mtime;
} FileEntry;

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

#endif // FILESYSTEM_H
