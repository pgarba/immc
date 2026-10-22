// The Windows meaning of the POSIX calls declared in posix.h. Every path that comes
// in is UTF-8 and is converted to UTF-16 here, so these work whatever the process code
// page turns out to be.

#include "posix.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <winnetwk.h>
#include <lm.h>

#include <string>
#include <vector>

static std::wstring widen(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static std::string narrow(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// GetLastError() in the errno vocabulary the callers print with strerror().
static int errno_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:     return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD: return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:      return EEXIST;
    case ERROR_DIR_NOT_EMPTY:    return ENOTEMPTY;
    case ERROR_DIRECTORY:        return ENOTDIR;
    case ERROR_NOT_SAME_DEVICE:  return EXDEV;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_WRITE_PROTECT:    return EROFS;
    default:                     return EIO;
    }
}

static int fail(void) {
    errno = errno_of(GetLastError());
    return -1;
}

static time_t filetime_to_time(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart == 0) return 0;
    // 100 ns ticks since 1601 to seconds since 1970.
    return (time_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static FILETIME time_to_filetime(time_t t) {
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)t * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

// Would the shell run a file with this name? The answer cmd.exe gives: its extension
// is one of PATHEXT's.
static bool runnable_name(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    const wchar_t* sep = wcspbrk(name, L"/\\");
    if (!dot) return false;
    for (const wchar_t* s = sep; s; s = wcspbrk(s + 1, L"/\\"))
        if (s > dot) return false;   // the dot was in a directory name
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"PATHEXT", buf, (DWORD)(sizeof buf / sizeof buf[0]));
    const wchar_t* exts = (n > 0 && n < sizeof buf / sizeof buf[0])
                              ? buf : L".COM;.EXE;.BAT;.CMD";
    size_t dl = wcslen(dot);
    for (const wchar_t* p = exts; *p;) {
        const wchar_t* e = wcschr(p, L';');
        size_t len = e ? (size_t)(e - p) : wcslen(p);
        if (len == dl && _wcsnicmp(p, dot, dl) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static mode_t mode_of(DWORD attrib, bool is_link, const wchar_t* name) {
    if (is_link) return S_IFLNK | 0777;
    if (attrib & FILE_ATTRIBUTE_DIRECTORY) {
        mode_t m = S_IFDIR | 0555;
        if (!(attrib & FILE_ATTRIBUTE_READONLY)) m |= 0200;
        return m;
    }
    mode_t m = S_IFREG | 0444;
    if (!(attrib & FILE_ATTRIBUTE_READONLY)) m |= 0200;
    if (runnable_name(name)) m |= 0111;
    return m;
}

// A name surrogate reparse point - a symbolic link or a junction - is what POSIX would
// call a link. Other reparse points (OneDrive placeholders, dedup) are files that
// happen to be stored oddly, and are described as what they hold.
static bool is_link_tag(DWORD attrib, DWORD tag) {
    return (attrib & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

static void fill_from_handle_info(const BY_HANDLE_FILE_INFORMATION& fi, const wchar_t* name,
                                  struct mc_stat* st) {
    memset(st, 0, sizeof *st);
    st->st_dev   = fi.dwVolumeSerialNumber;
    st->st_ino   = ((unsigned long long)fi.nFileIndexHigh << 32) | fi.nFileIndexLow;
    st->st_mode  = mode_of(fi.dwFileAttributes, false, name);
    st->st_nlink = fi.nNumberOfLinks;
    st->st_size  = (off_t)(((unsigned long long)fi.nFileSizeHigh << 32) | fi.nFileSizeLow);
    st->st_atime = filetime_to_time(fi.ftLastAccessTime);
    st->st_mtime = filetime_to_time(fi.ftLastWriteTime);
    st->st_ctime = filetime_to_time(fi.ftCreationTime);
    st->st_attrib = fi.dwFileAttributes;
}

static std::wstring unc_server(const std::wstring& w);

static int stat_impl(const char* path, struct mc_stat* st, bool follow) {
    std::wstring w = widen(path);
    if (w.empty()) { errno = ENOENT; return -1; }
    // A server on its own has no attributes to ask for - it is not a folder to
    // Windows - but it is the directory its shares are listed in (see opendir).
    if (!unc_server(w).empty()) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 1;
        st->st_attrib = FILE_ATTRIBUTE_DIRECTORY;
        return 0;
    }

    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &ad)) return fail();

    bool link = false;
    if (ad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // The tag is only in the directory entry, so this one costs a find.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(w.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            link = is_link_tag(fd.dwFileAttributes, fd.dwReserved0);
            FindClose(h);
        }
    }

    if (link && follow) {
        // Open through the link and describe what is at the other end. A link that
        // points nowhere fails here, which is what stat() says about a dangling link.
        HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return fail();
        BY_HANDLE_FILE_INFORMATION fi;
        BOOL ok = GetFileInformationByHandle(h, &fi);
        CloseHandle(h);
        if (!ok) return fail();
        fill_from_handle_info(fi, w.c_str(), st);
        return 0;
    }

    memset(st, 0, sizeof *st);
    st->st_mode  = mode_of(ad.dwFileAttributes, link, w.c_str());
    st->st_nlink = 1;
    st->st_size  = link ? 0 : (off_t)(((unsigned long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow);
    st->st_atime = filetime_to_time(ad.ftLastAccessTime);
    st->st_mtime = filetime_to_time(ad.ftLastWriteTime);
    st->st_ctime = filetime_to_time(ad.ftCreationTime);
    st->st_attrib = ad.dwFileAttributes;
    return 0;
}

extern "C" int mc_stat(const char* path, struct mc_stat* st)  { return stat_impl(path, st, true); }
extern "C" int mc_lstat(const char* path, struct mc_stat* st) { return stat_impl(path, st, false); }

extern "C" int mc_fstat(int fd, struct mc_stat* st) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) {
        // A pipe or a console has no file information; it is still a valid descriptor.
        memset(st, 0, sizeof *st);
        st->st_mode = (GetFileType(h) == FILE_TYPE_PIPE ? S_IFIFO : S_IFCHR) | 0600;
        return 0;
    }
    fill_from_handle_info(fi, L"", st);
    return 0;
}

// ---- directories --------------------------------------------------------------------

struct MC_DIR {
    std::wstring pattern;
    HANDLE h = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fd;
    bool have = false;      // fd holds an entry readdir has not returned yet
    struct dirent ent;
    // A server's shares, when the directory is "//server" rather than a folder.
    bool shares = false;
    std::vector<std::string> names;
    size_t next = 0;
};

// ---- network servers ----------------------------------------------------------------

static bool unc_sep(wchar_t c) { return c == L'/' || c == L'\\'; }

// "//server" or "\\server" with nothing after it: the name of the server, or "" for
// any other path. Windows has no folder to open there, but it is what a person types
// to see what a server shares, and Explorer answers it with the list of shares.
static std::wstring unc_server(const std::wstring& w) {
    if (w.size() < 3 || !unc_sep(w[0]) || !unc_sep(w[1]) || unc_sep(w[2])) return L"";
    size_t i = 2;
    while (i < w.size() && !unc_sep(w[i])) i++;
    std::wstring name = w.substr(2, i - 2);
    while (i < w.size() && unc_sep(w[i])) i++;
    if (i != w.size()) return L"";
    if (name == L"?" || name == L".") return L"";   // \\?\ and \\.\ are not servers
    return name;
}

// "\\server\share" for a path on a share, "" for a path that is not on one.
static std::wstring unc_share_root(const std::wstring& w) {
    if (w.size() < 3 || !unc_sep(w[0]) || !unc_sep(w[1]) || unc_sep(w[2])) return L"";
    size_t i = 2;
    while (i < w.size() && !unc_sep(w[i])) i++;
    std::wstring server = w.substr(2, i - 2);
    while (i < w.size() && unc_sep(w[i])) i++;
    size_t s = i;
    while (i < w.size() && !unc_sep(w[i])) i++;
    if (i == s || server == L"?" || server == L".") return L"";
    return L"\\\\" + server + L"\\" + w.substr(s, i - s);
}

// The refusals that mean "log in first" rather than "not there".
static bool needs_login(DWORD e) {
    return e == ERROR_ACCESS_DENIED || e == ERROR_LOGON_FAILURE ||
           e == ERROR_INVALID_PASSWORD || e == ERROR_BAD_USERNAME ||
           e == ERROR_NOT_AUTHENTICATED || e == ERROR_SESSION_CREDENTIAL_CONFLICT;
}

// Ask Windows for a session with a share - the credentials prompt Explorer shows,
// with its "remember my credentials" box - so a password given once is the system's
// to keep and not this program's.
static bool unc_login(const std::wstring& remote) {
    NETRESOURCEW nr = {};
    nr.dwType = RESOURCETYPE_ANY;
    nr.lpRemoteName = const_cast<wchar_t*>(remote.c_str());
    return WNetAddConnection2W(&nr, nullptr, nullptr, CONNECT_INTERACTIVE | CONNECT_PROMPT) == NO_ERROR;
}

// The disk shares a server offers, without the administrative ones (C$, ADMIN$).
static bool share_names(const std::wstring& server, std::vector<std::string>* out) {
    std::wstring s = L"\\\\" + server;
    for (int attempt = 0; attempt < 2; attempt++) {
        LPBYTE buf = nullptr;
        DWORD read = 0, total = 0, resume = 0;
        NET_API_STATUS r = NetShareEnum(const_cast<wchar_t*>(s.c_str()), 1, &buf,
                                        MAX_PREFERRED_LENGTH, &read, &total, &resume);
        if (r == NERR_Success || r == ERROR_MORE_DATA) {
            SHARE_INFO_1* si = (SHARE_INFO_1*)buf;
            for (DWORD i = 0; i < read; i++) {
                if ((si[i].shi1_type & STYPE_MASK) != STYPE_DISKTREE) continue;
                if (si[i].shi1_type & STYPE_SPECIAL) continue;
                out->push_back(narrow(si[i].shi1_netname));
            }
            if (buf) NetApiBufferFree(buf);
            return true;
        }
        if (buf) NetApiBufferFree(buf);
        SetLastError(r);
        if (attempt == 0 && needs_login(r) && unc_login(s + L"\\IPC$")) continue;
        return false;
    }
    return false;
}

static bool dir_start(MC_DIR* d) {
    d->h = FindFirstFileExW(d->pattern.c_str(), FindExInfoBasic, &d->fd,
                            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    d->have = d->h != INVALID_HANDLE_VALUE;
    // An empty drive root has no "." either, and that is a directory, not an error.
    return d->have || GetLastError() == ERROR_FILE_NOT_FOUND;
}

extern "C" DIR* opendir(const char* path) {
    std::wstring w = widen(path);
    if (w.empty()) { errno = ENOENT; return nullptr; }
    std::wstring server = unc_server(w);
    if (!server.empty()) {
        MC_DIR* d = new MC_DIR();
        d->shares = true;
        if (!share_names(server, &d->names)) { fail(); delete d; return nullptr; }
        return d;
    }
    // FindFirstFile on "x\*" succeeds for a file named x on some file systems, so a
    // directory is checked for first: opendir on a file is ENOTDIR, and the callers
    // use opendir as the "is this a directory" probe.
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES && needs_login(GetLastError())) {
        // A share that wants a login gets the system's prompt once, then a retry.
        std::wstring share = unc_share_root(w);
        if (!share.empty() && unc_login(share)) a = GetFileAttributesW(w.c_str());
    }
    if (a == INVALID_FILE_ATTRIBUTES) { fail(); return nullptr; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { errno = ENOTDIR; return nullptr; }

    MC_DIR* d = new MC_DIR();
    d->pattern = w;
    if (d->pattern.back() != L'/' && d->pattern.back() != L'\\') d->pattern += L'\\';
    d->pattern += L'*';
    if (!dir_start(d)) { fail(); delete d; return nullptr; }
    return d;
}

extern "C" struct dirent* readdir(DIR* d) {
    if (d && d->shares) {
        if (d->next >= d->names.size()) return nullptr;
        const std::string& n = d->names[d->next++];
        snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", n.c_str());
        // A share whose name ends in $ is hidden by convention, and Explorer leaves
        // it out of the list the same way.
        d->ent.d_attrib = FILE_ATTRIBUTE_DIRECTORY |
                          (!n.empty() && n.back() == '$' ? FILE_ATTRIBUTE_HIDDEN : 0);
        return &d->ent;
    }
    if (!d || !d->have) return nullptr;
    std::string n = narrow(d->fd.cFileName);
    snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", n.c_str());
    d->ent.d_attrib = d->fd.dwFileAttributes;
    d->have = FindNextFileW(d->h, &d->fd) != 0;
    return &d->ent;
}

extern "C" void rewinddir(DIR* d) {
    if (!d) return;
    if (d->shares) { d->next = 0; return; }
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    dir_start(d);
}

extern "C" int closedir(DIR* d) {
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    delete d;
    return 0;
}

extern "C" int mc_mkdir(const char* path, mode_t) {
    std::wstring w = widen(path);
    return CreateDirectoryW(w.c_str(), nullptr) ? 0 : fail();
}

extern "C" int mc_rmdir(const char* path) {
    std::wstring w = widen(path);
    return RemoveDirectoryW(w.c_str()) ? 0 : fail();
}

extern "C" int mc_unlink(const char* path) {
    std::wstring w = widen(path);
    if (DeleteFileW(w.c_str())) return 0;
    // A link to a directory is a directory entry, and goes the way directories go.
    DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
        (a & FILE_ATTRIBUTE_REPARSE_POINT) && RemoveDirectoryW(w.c_str()))
        return 0;
    // POSIX unlink does not care about a read-only bit; that belongs to the file's
    // contents, not to the directory entry. Clear it and try once more.
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(w.c_str())) return 0;
        SetFileAttributesW(w.c_str(), a);
    }
    return fail();
}

extern "C" int mc_rename(const char* from, const char* to) {
    std::wstring f = widen(from), t = widen(to);
    // No MOVEFILE_COPY_ALLOWED: a rename across volumes fails with EXDEV, which is the
    // answer the callers expect before they fall back to copy and delete themselves.
    if (MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING)) return 0;
    return fail();
}

// ---- access -------------------------------------------------------------------------

extern "C" int mc_access(const char* path, int mode) {
    std::wstring w = widen(path);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return fail();
    if ((mode & W_OK) && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        errno = EACCES;
        return -1;
    }
    if ((mode & X_OK) && !(a & FILE_ATTRIBUTE_DIRECTORY) && !runnable_name(w.c_str())) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

extern "C" int mc_chmod(const char* path, mode_t mode) {
    std::wstring w = widen(path);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return fail();
    DWORD n = (mode & S_IWUSR) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
    if (n == a) return 0;
    return SetFileAttributesW(w.c_str(), n) ? 0 : fail();
}

extern "C" int symlink(const char* target, const char* linkpath) {
    std::wstring t = widen(target), l = widen(linkpath);
    // Windows needs to know up front whether a link points at a directory.
    DWORD a = GetFileAttributesW(t.c_str());
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    for (wchar_t& c : t) if (c == L'/') c = L'\\';   // a link's target is stored verbatim
    if (CreateSymbolicLinkW(l.c_str(), t.c_str(), flags)) return 0;
    // Older systems, and newer ones outside developer mode, refuse the unprivileged
    // flag outright rather than ignoring it.
    if (GetLastError() == ERROR_INVALID_PARAMETER &&
        CreateSymbolicLinkW(l.c_str(), t.c_str(), flags & ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return 0;
    return fail();
}

// The layout of a symbolic link's reparse data. It lives in the driver kit's ntifs.h,
// not in the SDK, so it is spelled out here for the one member that is read.
struct MC_REPARSE_SYMLINK {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG Flags;               // absent for a junction, whose names start here instead
    WCHAR PathBuffer[1];
};

extern "C" ssize_t readlink(const char* path, char* buf, size_t size) {
    std::wstring w = widen(path);
    HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail();
    union { MC_REPARSE_SYMLINK r; unsigned char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; } data;
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, &data, sizeof data, &got, nullptr);
    CloseHandle(h);
    if (!ok) { errno = EINVAL; return -1; }   // not a link at all

    const WCHAR* names;
    USHORT off, len;
    if (data.r.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        names = data.r.PathBuffer;
        off = data.r.PrintNameOffset;
        len = data.r.PrintNameLength;
    } else if (data.r.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        names = (const WCHAR*)&data.r.Flags;
        off = data.r.PrintNameOffset;
        len = data.r.PrintNameLength;
    } else {
        errno = EINVAL;
        return -1;
    }
    std::wstring target(names + off / sizeof(WCHAR), len / sizeof(WCHAR));
    for (wchar_t& c : target) if (c == L'\\') c = L'/';
    std::string t = narrow(target.c_str());
    size_t n = t.size() < size ? t.size() : size;
    memcpy(buf, t.data(), n);
    return (ssize_t)n;
}

extern "C" int utimensat(int, const char* path, const struct timespec times[2], int) {
    std::wstring w = widen(path);
    HANDLE h = CreateFileW(w.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail();
    FILETIME at = time_to_filetime(times[0].tv_sec);
    FILETIME mt = time_to_filetime(times[1].tv_sec);
    BOOL ok = SetFileTime(h, nullptr, &at, &mt);
    CloseHandle(h);
    return ok ? 0 : fail();
}

// ---- the rest -----------------------------------------------------------------------

extern "C" char* mkdtemp(char* tmpl) {
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { errno = EINVAL; return nullptr; }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned long long r = GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 20)
                               ^ ((unsigned long long)attempt * 2654435761ULL);
        LARGE_INTEGER pc;
        QueryPerformanceCounter(&pc);
        r ^= (unsigned long long)pc.QuadPart * 6364136223846793005ULL;
        for (int i = 0; i < 6; i++) {
            tmpl[n - 6 + i] = chars[r % 36];
            r /= 36;
        }
        if (mc_mkdir(tmpl, 0700) == 0) return tmpl;
        if (errno != EEXIST) return nullptr;
    }
    errno = EEXIST;
    return nullptr;
}

extern "C" int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

extern "C" int unsetenv(const char* name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

extern "C" int usleep(unsigned int usec) {
    Sleep((usec + 999) / 1000);
    return 0;
}

// Where the root part of a path ends: "/" is 1, "C:/" is 3, "//server/share/" is the
// whole UNC prefix. Everything up to there is not a component and never goes away.
static size_t root_len(const char* p) {
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return sep(p[2]) ? 3 : 2;
    if (sep(p[0]) && sep(p[1])) {
        size_t i = 2;
        for (int part = 0; part < 2; part++) {
            while (p[i] && !sep(p[i])) i++;
            if (p[i]) i++;
        }
        return i;
    }
    return sep(p[0]) ? 1 : 0;
}

extern "C" char* dirname(char* path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    size_t root = root_len(path);
    size_t n = strlen(path);
    while (n > root && sep(path[n - 1])) n--;            // trailing separators
    // At the root of a share, up is the server: "//server/share" -> "//server",
    // which is where the server's shares are listed (see opendir).
    if (n <= root && sep(path[0]) && sep(path[1]) && path[2] && !sep(path[2])) {
        size_t s = 2;
        while (path[s] && !sep(path[s])) s++;
        size_t after = s;
        while (path[after] && sep(path[after])) after++;
        if (path[after]) {                               // a share follows the server
            path[s] = '\0';
            return path;
        }
    }
    while (n > root && !sep(path[n - 1])) n--;           // the last component
    while (n > root && sep(path[n - 1])) n--;            // the separators before it
    if (n == 0) return dot;
    path[n] = '\0';
    return path;
}

extern "C" char* basename(char* path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    size_t root = root_len(path);
    size_t n = strlen(path);
    while (n > root && sep(path[n - 1])) path[--n] = '\0';
    if (n == root) return path;
    char* b = path + n;
    while (b > path + root && !sep(b[-1])) b--;
    return b;
}

extern "C" int clock_gettime(int clock, struct timespec* ts) {
    if (clock == CLOCK_MONOTONIC) {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        ts->tv_sec = (time_t)(c.QuadPart / f.QuadPart);
        ts->tv_nsec = (long)((c.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
        return 0;
    }
    return timespec_get(ts, TIME_UTC) ? 0 : -1;
}
