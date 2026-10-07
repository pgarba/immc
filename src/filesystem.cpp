#include "filesystem.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <cstdio>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

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

bool fs_make_dir(const char* path) {
    return mkdir(path, 0755) == 0;
}

bool fs_copy_entry(const char* src, const char* dst) {
    struct stat st;
    if (stat(src, &st) != 0) return false;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) return false;
        DIR* dir = opendir(src);
        if (!dir) return false;
        struct dirent* de;
        bool ok = true;
        while ((de = readdir(dir)) != nullptr) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char s2[4100], d2[4100];
            snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name);
            snprintf(d2, sizeof(d2), "%s/%s", dst, de->d_name);
            if (!fs_copy_entry(s2, d2)) ok = false;
        }
        closedir(dir);
        return ok;
    }

    // Regular file: read + write, preserve mode and mtime
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }

    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    if (!ok) return false;

    chmod(dst, st.st_mode & 0777);
    struct timespec times[2] = { st.st_mtime, st.st_mtime };
    utimensat(AT_FDCWD, dst, times, 0);
    return true;
}

bool fs_delete_entry(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return false;

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return false;
        struct dirent* de;
        bool ok = true;
        while ((de = readdir(dir)) != nullptr) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char p2[4100];
            snprintf(p2, sizeof(p2), "%s/%s", path, de->d_name);
            if (!fs_delete_entry(p2)) ok = false;
        }
        closedir(dir);
        if (!ok) return false;
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

bool fs_move_entry(const char* src, const char* dst) {
    if (rename(src, dst) == 0) return true;   // same filesystem: fast path
    if (!fs_copy_entry(src, dst)) return false;
    return fs_delete_entry(src);
}

void format_mode(mode_t mode, char* out) {
    static const char* rwx = "rwx";
    for (int i = 0; i < 3; i++) {
        for (int b = 2; b >= 0; b--) {
            *out++ = (mode & (1 << (i * 3 + b))) ? rwx[2 - b] : '-';
        }
    }
    *out = '\0';
}

long fs_read_preview(const char* path, char* buf, size_t max_bytes) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, max_bytes, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\0') return -1; // binary
    }
    buf[n] = '\0';
    return (long)n;
}
