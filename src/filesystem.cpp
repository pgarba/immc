#include "filesystem.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <cstdio>

FileEntry* file_entry_new(const char* name, mode_t mode, off_t size, time_t mtime) {
    FileEntry* entry = (FileEntry*)calloc(1, sizeof(FileEntry));
    if (!entry) return nullptr;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->mode = mode;
    entry->size = size;
    entry->mtime = mtime;
    return entry;
}

void file_entry_free(FileEntry* entry) {
    free(entry);
}

int compare_files(const void* a, const void* b) {
    // qsort passes addresses of array elements; the elements are FileEntry*
    const FileEntry* fa = *(const FileEntry* const*)a;
    const FileEntry* fb = *(const FileEntry* const*)b;
    
    // Directories first
    bool a_dir = S_ISDIR(fa->mode);
    bool b_dir = S_ISDIR(fb->mode);
    if (a_dir != b_dir) return a_dir ? -1 : 1;
    
    // Then by name
    return strcasecmp(fa->name, fb->name);
}

void list_directory(const char* path, FileEntry*** out_entries, int* out_count) {
    *out_entries = nullptr;
    *out_count = 0;
    
    DIR* dir = opendir(path);
    if (!dir) return;
    
    // First pass: count entries (skip "." and ".." — they are pinned manually)
    struct dirent* de;
    int count = 0;
    while ((de = readdir(dir)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        count++;
    }
    rewinddir(dir);
    
    // Allocate array: 1 slot for ".." plus the real entries
    *out_entries = (FileEntry**)calloc(count + 1, sizeof(FileEntry*));
    if (!*out_entries) {
        closedir(dir);
        return;
    }
    
    // ".." comes first (MC-style); "." is not shown
    struct stat st;
    char parent[4100];
    snprintf(parent, sizeof(parent), "%s/..", path);
    if (stat(parent, &st) == 0) {
        (*out_entries)[0] = file_entry_new("..", st.st_mode, st.st_size, st.st_mtime);
    } else {
        (*out_entries)[0] = file_entry_new("..", S_IFDIR | 0755, 0, 0);
    }
    
    // Second pass: fill entries starting at index 1
    int i = 1;
    while ((de = readdir(dir)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        
        char fullpath[4100];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, de->d_name);
        
        if (stat(fullpath, &st) == 0) {
            FileEntry* entry = file_entry_new(de->d_name, st.st_mode, st.st_size, st.st_mtime);
            if (entry) {
                (*out_entries)[i++] = entry;
            }
        }
    }
    *out_count = i;
    closedir(dir);
    
    // Sort the real entries (leaving ".." pinned at the front)
    if (i > 2) {
        qsort(*out_entries + 1, (size_t)(i - 1), sizeof(FileEntry*), compare_files);
    }
}

void free_entries(FileEntry** entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        file_entry_free(entries[i]);
    }
    free(entries);
}

bool is_directory(const char* name, mode_t mode) {
    return S_ISDIR(mode);
}

bool is_executable(const char* name, mode_t mode) {
    return (mode & S_IXUSR) != 0;
}
