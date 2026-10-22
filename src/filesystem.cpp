#include "filesystem.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <cstdio>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#endif
#include <vector>
#include <string>
#include <algorithm>

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

static int g_sort_mode = FS_SORT_NAME;
static bool g_show_hidden = false;

void fs_set_sort_mode(int mode) { g_sort_mode = mode; }
int  fs_get_sort_mode()        { return g_sort_mode; }
void fs_set_show_hidden(bool s){ g_show_hidden = s; }
bool fs_get_show_hidden()      { return g_show_hidden; }

bool fs_entry_hidden(const struct dirent* de) {
    if (de->d_name[0] == '.') return true;
#ifdef _WIN32
    if (de->d_attrib & FILE_ATTRIBUTE_HIDDEN) return true;
#endif
    return false;
}

bool fs_exists(const char* path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

// Extension for sort-by-ext (empty string when no dot)
static const char* ext_of(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

int compare_files(const void* a, const void* b) {
    // qsort passes addresses of array elements; the elements are FileEntry*
    const FileEntry* fa = *(const FileEntry* const*)a;
    const FileEntry* fb = *(const FileEntry* const*)b;
    
    // Directories first
    bool a_dir = S_ISDIR(fa->mode);
    bool b_dir = S_ISDIR(fb->mode);
    if (a_dir != b_dir) return a_dir ? -1 : 1;
    
    switch (g_sort_mode) {
    case FS_SORT_EXT: {
        int r = strcasecmp(ext_of(fa->name), ext_of(fb->name));
        if (r) return r;
        return strcasecmp(fa->name, fb->name);
    }
    case FS_SORT_SIZE:
        if (fa->size != fb->size) return fa->size < fb->size ? -1 : 1;
        return strcasecmp(fa->name, fb->name);
    case FS_SORT_DATE:
        if (fa->mtime != fb->mtime) return fa->mtime < fb->mtime ? -1 : 1;
        return strcasecmp(fa->name, fb->name);
    case FS_SORT_NAME:
    default:
        return strcasecmp(fa->name, fb->name);
    }
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
        if (!g_show_hidden && fs_entry_hidden(de)) continue;
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
        if (!g_show_hidden && fs_entry_hidden(de)) continue;
        
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
    
    // Sort the real entries (leaving ".." pinned at the front).
    // FS_SORT_NONE keeps readdir() order, so qsort is skipped entirely.
    if (i > 2 && g_sort_mode != FS_SORT_NONE) {
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

// Shift+F4: an empty file. O_EXCL so an existing name is never truncated.
bool fs_touch(const char* path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return false;
    close(fd);
    return true;
}

// Shift+Ctrl+F5: a link in another directory. `src` is expected absolute.
bool fs_symlink(const char* src, const char* dst) {
    return symlink(src, dst) == 0;
}

#ifdef _WIN32
// TC splits delete into "direkt" and "Papierkorb". On Windows the bin is the shell's:
// a delete with undo allowed is a move to the Recycle Bin, with no dialog of its own -
// the app already asked.
bool fs_trash(const char* path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (n <= 0) return false;
    // A list of paths, each NUL-terminated, the whole list ending in one more NUL.
    std::wstring from((size_t)n + 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &from[0], n);
    for (wchar_t& c : from) if (c == L'/') c = L'\\';   // the shell will not take '/'
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// Every Windows desktop has a Recycle Bin. A volume without one (a network share, a
// USB stick formatted by something else) makes the shell delete outright, which is
// the same thing the POSIX side does when no helper is installed.
bool fs_trash_available(void) {
    return true;
}
#else
// Run a helper as: prog [sub] path. True only if it exited 0.
static bool run_helper(const char* prog, const char* sub, const char* path) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        if (sub) execlp(prog, prog, sub, path, (char*)nullptr);
        else     execlp(prog, prog, path, (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// TC splits delete into "direkt" and "Papierkorb". The trash half is handed to
// a desktop helper; when neither is installed the caller deletes directly.
bool fs_trash(const char* path) {
    if (run_helper("gio", "trash", path)) return true;
    if (run_helper("trash-put", nullptr, path)) return true;
    return false;
}

// Probed once per run: the confirm dialog has to say "move to trash" or
// "delete" before anything is touched.
bool fs_trash_available(void) {
    static int cached = -1;
    if (cached < 0)
        cached = system("command -v gio >/dev/null 2>&1 || "
                        "command -v trash-put >/dev/null 2>&1") == 0 ? 1 : 0;
    return cached == 1;
}
#endif

// Copy with an optional progress context: totals are computed up-front by the
// caller (fs_copy_entry_progress), and each completed file reports progress.
struct fs_copy_ctx {
    fs_progress_fn cb;
    void* user;
    long long done_bytes, total_bytes;
    int done_files, total_files;
};

static bool copy_report(fs_copy_ctx* c) {
    if (!c->cb) return true;
    return c->cb(c->done_bytes, c->total_bytes, c->done_files, c->total_files, c->user);
}

static bool fs_copy_rec(const char* src, const char* dst, fs_copy_ctx* ctx) {
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
            if (!fs_copy_rec(s2, d2, ctx)) ok = false;
        }
        closedir(dir);
        if (ctx && !copy_report(ctx)) return false;   // keep empty dirs on the bar
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
    // Both braces spelled out: { mtime, mtime } would fill the first timespec's
    // seconds and nanoseconds and leave the modification time at zero.
    struct timespec times[2] = { { st.st_mtime, 0 }, { st.st_mtime, 0 } };
    utimensat(AT_FDCWD, dst, times, 0);

    if (ctx) {
        ctx->done_files++;
        ctx->done_bytes += st.st_size;
        if (!copy_report(ctx)) return false;
    }
    return true;
}

bool fs_copy_entry(const char* src, const char* dst) {
    return fs_copy_rec(src, dst, nullptr);
}

bool fs_copy_entry_progress(const char* src, const char* dst, fs_progress_fn cb, void* user) {
    fs_copy_ctx ctx = { cb, user, 0, 0, 0, 0 };
    long long tb; long nf, nd;
    fs_count_tree(src, &tb, &nf, &nd);
    ctx.total_bytes = tb;
    ctx.total_files = (int)nf;
    if (!copy_report(&ctx)) return false;
    return fs_copy_rec(src, dst, &ctx);
}

bool fs_move_entry_progress(const char* src, const char* dst, fs_progress_fn cb, void* user) {
    if (rename(src, dst) == 0) {   // same filesystem: instant, one final report
        if (cb) {
            long long tb; long nf, nd;
            fs_count_tree(dst, &tb, &nf, &nd);
            cb(tb, tb, (int)nf, (int)nf, user);
        }
        return true;
    }
    if (!fs_copy_entry_progress(src, dst, cb, user)) return false;
    return fs_delete_entry(src);
}

static bool fs_delete_rec(const char* path, fs_copy_ctx* ctx) {
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
            if (!fs_delete_rec(p2, ctx)) ok = false;
        }
        closedir(dir);
        if (!ok) return false;
        if (rmdir(path) != 0) return false;
        if (ctx && !copy_report(ctx)) return false;
        return true;
    }
    if (unlink(path) != 0) return false;
    if (ctx) {
        ctx->done_files++;
        ctx->done_bytes += st.st_size;
        if (!copy_report(ctx)) return false;
    }
    return true;
}

bool fs_delete_entry(const char* path) {
    return fs_delete_rec(path, nullptr);
}

bool fs_delete_entry_progress(const char* path, fs_progress_fn cb, void* user) {
    fs_copy_ctx ctx = { cb, user, 0, 0, 0, 0 };
    long long tb; long nf, nd;
    fs_count_tree(path, &tb, &nf, &nd);
    ctx.total_bytes = tb;
    ctx.total_files = (int)(nf + nd);   // files + dirs, so the bar reaches 100%
    if (!copy_report(&ctx)) return false;
    return fs_delete_rec(path, &ctx);
}

bool fs_move_entry(const char* src, const char* dst) {
    if (rename(src, dst) == 0) return true;   // same filesystem: fast path
    if (!fs_copy_entry(src, dst)) return false;
    return fs_delete_entry(src);
}

void format_mode(mode_t mode, char* out) {
    static const char* rwx = "rwx";
    // Owner, group, others, in that order: the owner's triplet is the high one.
    for (int i = 2; i >= 0; i--) {
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

// What the head of a file says about what it is. Only the magic numbers people
// actually meet get a name; everything else that is not text gets the plain answer,
// because "not a text file" is already the decision that matters.
bool fs_is_binary_file(const char* path, char* why, size_t why_n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;                      // unreadable is not the same as binary
    unsigned char head[8192];
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    if (n == 0) return false;                  // an empty file is text, and harmless

    auto name = [&](const char* what) {
        if (why) snprintf(why, why_n, "%s", what);
        return true;
    };
    auto has = [&](const void* magic, size_t m) {
        return n >= m && memcmp(head, magic, m) == 0;
    };

    static const unsigned char elf[4] = {0x7F, 'E', 'L', 'F'};
    if (has(elf, sizeof elf) && n >= 20) {
        // e_type is two bytes at offset 16, in the endianness the file itself declares
        // at offset 5. Naming the kind is the difference between "not text" and
        // "that .so you are holding is a shared library".
        const int le = head[5] == 1;
        const unsigned char t0 = head[16], t1 = head[17];
        unsigned int etype = le ? (unsigned)t0 | ((unsigned)t1 << 8)
                                : (unsigned)t1 | ((unsigned)t0 << 8);
        switch (etype) {
            case 1: return name("an ELF object file");
            case 2: return name("an ELF executable");
            case 3: return name("an ELF shared object");
            case 4: return name("an ELF core dump");
            default: return name("an ELF file");
        }
    }
    if (head[0] == 'M' && head[1] == 'Z') return name("a Windows executable");
    if (has("!<arch>", 7)) return name("an archive (ar)");
    if (has("%PDF", 4)) return name("a PDF");
    if (head[0] == 0x1F && head[1] == 0x8B) return name("a compressed file (gzip)");
    if (has("BZh", 3)) return name("a compressed file (bzip2)");
    static const unsigned char xz[6] = {0x7A, 0x76, 0x5F, 0x27, 0x2A, 0x01};
    if (has(xz, sizeof xz)) return name("a compressed file (xz)");
    if (head[0] == 'P' && head[1] == 'K' && head[2] == 0x03 && head[3] == 0x04)
        return name("an archive (zip)");

    // The same tell the quick view uses for a hex dump: a NUL byte in the head is not
    // something a text file has.
    for (size_t i = 0; i < n; i++)
        if (head[i] == '\0') return name("not a text file");
    return false;
}

// Recursive tree count for the properties dialog. Symlinks are counted but
// never followed (avoids loops). ndirs counts subdirectories, not the root.
static void count_tree_recursive(const char* path, long long* total_size,
                                 long* nfiles, long* ndirs, int depth) {
    if (depth > 64) return;
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) {
            (*nfiles)++;
            *total_size += st.st_size;
        } else if (S_ISDIR(st.st_mode)) {
            (*ndirs)++;
            count_tree_recursive(full, total_size, nfiles, ndirs, depth + 1);
        } else {
            (*nfiles)++;
            *total_size += st.st_size;
        }
    }
    closedir(dir);
}

// Branch view (Ctrl+B): one depth-first walk of `root`, collecting every
// directory and file below it with its path relative to the root. Symlinks are
// not followed, so a link loop can never make the walk run away.
int fs_list_tree(const char* root, std::vector<FsTreeEntry>* out,
                 int max_depth, int max_entries) {
    out->clear();
    if (max_depth < 1 || max_entries < 1) return 0;
    size_t root_len = strlen(root);
    while (root_len && root[root_len - 1] == '/') root_len--;

    std::vector<std::pair<int, std::string>> stack;
    stack.push_back(std::make_pair(0, std::string(root, root_len)));

    while (!stack.empty()) {
        int depth = stack.back().first;
        std::string dir = stack.back().second;
        stack.pop_back();
        if (depth >= max_depth) continue;

        // Everything below `dir` is reported relative to the walked root.
        const char* prefix = dir.c_str() + root_len;
        while (*prefix == '/') prefix++;

        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (!g_show_hidden && fs_entry_hidden(de)) continue;
            char full[4100];
            snprintf(full, sizeof(full), "%s/%s", dir.c_str(), de->d_name);
            struct stat st;
            if (lstat(full, &st) != 0) continue;

            FsTreeEntry e;
            if (prefix[0]) snprintf(e.rel, sizeof(e.rel), "%s/%s", prefix, de->d_name);
            else           snprintf(e.rel, sizeof(e.rel), "%s", de->d_name);
            e.mode  = st.st_mode;
            e.size  = st.st_size;
            e.mtime = st.st_mtime;
            out->push_back(e);

            if (S_ISDIR(st.st_mode)) stack.push_back(std::make_pair(depth + 1, std::string(full)));
            if ((int)out->size() >= max_entries) { closedir(d); goto done; }
        }
        closedir(d);
    }
done:
    std::sort(out->begin(), out->end(),
              [](const FsTreeEntry& a, const FsTreeEntry& b) { return strcmp(a.rel, b.rel) < 0; });
    return (int)out->size();
}

void fs_count_tree(const char* path, long long* total_size, long* nfiles, long* ndirs) {
    *total_size = 0; *nfiles = 0; *ndirs = 0;
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        *total_size = st.st_size;
        *nfiles = 1;
        return;
    }
    count_tree_recursive(path, total_size, nfiles, ndirs, 0);
}

// Classic hex dump: "offset  xx xx ... |ascii|", 16 bytes per line.
// Returns the number of input bytes dumped, or -1 if the file can't be read.
long fs_hex_dump(const char* path, char* out, size_t out_size, long max_bytes) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    size_t o = 0;
    out[0] = '\0';
    unsigned char data[4096];
    long total = 0;

    while (total < max_bytes) {
        size_t want = sizeof(data);
        if (total + (long)want > max_bytes) want = (size_t)(max_bytes - total);
        size_t n = fread(data, 1, want, f);
        if (!n) break;

        for (size_t i = 0; i < n; i += 16) {
            int cnt = (int)((n - i) < 16 ? (n - i) : 16);
            char hexpart[64];
            int hp = 0;
            for (int j = 0; j < 16; j++) {
                if (j == 8) hexpart[hp++] = ' ';
                if (j < cnt) hp += snprintf(hexpart + hp, sizeof(hexpart) - hp, "%02x ", data[i + j]);
                else { hexpart[hp++] = ' '; hexpart[hp++] = ' '; hexpart[hp++] = ' '; }
            }
            hexpart[hp] = '\0';
            char asciipart[20];
            int ap = 0;
            for (int j = 0; j < 16; j++) {
                unsigned char c = (j < cnt) ? data[i + j] : ' ';
                asciipart[ap++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            asciipart[ap] = '\0';

            char line[96];
            snprintf(line, sizeof(line), "%08lx  %s |%s|\n", (unsigned long)(total + i), hexpart, asciipart);
            size_t ll = strlen(line);
            if (o + ll >= out_size - 1) { o = out_size - 1; break; }
            memcpy(out + o, line, ll);
            o += ll;
        }
        total += (long)n;
        if (o >= out_size - 1) break;
    }
    out[o] = '\0';
    fclose(f);
    return total;
}
