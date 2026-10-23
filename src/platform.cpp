#include "platform.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

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
#include <signal.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// An environment variable read as UTF-16, so a user name outside the ANSI code page
// survives, with its backslashes turned into the separator the rest of the program
// uses.
static std::string env_path(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return std::string();
    std::string s = narrow(buf);
    for (char& c : s) if (c == '\\') c = '/';
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}

static std::string last_error_text() {
    DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (wchar_t*)&msg, 0, nullptr);
    std::string out = narrow(msg);
    if (msg) LocalFree(msg);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out.empty() ? "error " + std::to_string(e) : out;
}
#endif

std::string plat_home_dir() {
#ifdef _WIN32
    std::string h = env_path(L"HOME");
    if (h.empty()) h = env_path(L"USERPROFILE");
    return h;
#else
    const char* h = getenv("HOME");
    return h ? h : "";
#endif
}

std::string plat_config_dir() {
#ifdef _WIN32
    std::string base = env_path(L"APPDATA");
    if (base.empty()) {
        std::string home = plat_home_dir();
        if (home.empty()) return ".mc-clone";
        base = home + "/AppData/Roaming";
    }
    return base + "/mc-clone";
#else
    std::string home = plat_home_dir();
    if (home.empty()) return ".mc-clone";
    return home + "/.config/mc-clone";
#endif
}

void plat_make_config_dir() {
    std::string dir = plat_config_dir();
    // Create the parent (~/.config) and then the directory itself; "already exists"
    // is the usual answer and not an error.
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0) mkdir(dir.substr(0, slash).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
}

std::string plat_temp_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW((DWORD)(sizeof buf / sizeof buf[0]), buf);
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return "C:/Windows/Temp";
    std::string s = narrow(buf);
    for (char& c : s) if (c == '\\') c = '/';
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
#else
    const char* t = getenv("TMPDIR");
    return (t && *t) ? t : "/tmp";
#endif
}

bool plat_path_is_absolute(const char* p) {
    if (!p || !*p) return false;
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
        (p[2] == '/' || p[2] == '\\'))
        return true;
    return p[0] == '/' || p[0] == '\\';
#else
    return p[0] == '/';
#endif
}

std::string plat_root_of(const char* p) {
#ifdef _WIN32
    if (p && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        char root[4] = { (char)toupper((unsigned char)p[0]), ':', '/', '\0' };
        return root;
    }
    std::string sys = env_path(L"SystemDrive");
    return (sys.size() >= 2 ? sys.substr(0, 2) : std::string("C:")) + "/";
#else
    (void)p;
    return "/";
#endif
}

bool plat_spawn_detached(const std::vector<std::string>& argv, std::string* err) {
    if (argv.empty()) { *err = "nothing to run"; return false; }
#ifdef _WIN32
    std::wstring line = widen(plat_command_line(argv));
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        *err = last_error_text();
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) { *err = strerror(errno); return false; }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);   // remote.cpp ignores it; the program should not
        std::vector<char*> cs;
        for (const std::string& a : argv) cs.push_back(const_cast<char*>(a.c_str()));
        cs.push_back(nullptr);
        execvp(cs[0], cs.data());
        // Found a moment ago and still unrunnable: a name on PATH that went away, or a
        // file that is not really executable. Say which one, on stderr, where a
        // forked child is allowed to say things.
        fprintf(stderr, "immc: cannot run %s: %s\n", cs[0], strerror(errno));
        _exit(127);
    }
    return true;
#endif
}

bool plat_open_default(const char* path, std::string* err) {
#ifdef _WIN32
    std::wstring w = widen(path);
    for (wchar_t& c : w) if (c == L'/') c = L'\\';   // the shell wants its own separator
    HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return true;
    *err = (INT_PTR)r == SE_ERR_NOASSOC ? "no application is associated with it"
                                         : last_error_text();
    return false;
#else
    if (system("command -v xdg-open >/dev/null 2>&1") != 0) {
        *err = "no xdg-open";
        return false;
    }
    return plat_spawn_detached({ "xdg-open", path }, err);
#endif
}

std::string plat_console_text(const char* s, size_t n) {
#ifdef _WIN32
    if (n == 0) return std::string();
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, nullptr, 0) > 0)
        return std::string(s, n);
    // Not CP_OEMCP: the manifest that makes this process's code pages UTF-8 makes that
    // one UTF-8 too. cmd has no such manifest and uses the system locale's.
    static UINT oem = [] {
        DWORD cp = 0;
        if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
                            (LPWSTR)&cp, sizeof cp / sizeof(wchar_t)) == 0 || cp == 0)
            cp = 437;
        return (UINT)cp;
    }();
    int wn = MultiByteToWideChar(oem, 0, s, (int)n, nullptr, 0);
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(oem, 0, s, (int)n, &w[0], wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], un, nullptr, nullptr);
    return out;
#else
    return std::string(s, n);
#endif
}

#ifdef _WIN32
std::string plat_from_ansi(const std::string& s) {
    if (s.empty()) return s;
    static UINT ansi = [] {
        DWORD cp = 0;
        if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                            (LPWSTR)&cp, sizeof cp / sizeof(wchar_t)) == 0 || cp == 0)
            cp = 1252;
        return (UINT)cp;
    }();
    int wn = MultiByteToWideChar(ansi, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(ansi, 0, s.data(), (int)s.size(), &w[0], wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], un, nullptr, nullptr);
    return out;
}

// One argument quoted the way CommandLineToArgvW and the CRT split it back apart:
// backslashes are literal except in front of a quote, where they escape it.
static void append_quoted(std::string* line, const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) {
        *line += a;
        return;
    }
    *line += '"';
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { backslashes++; continue; }
        if (c == '"') line->append(backslashes * 2 + 1, '\\');
        else line->append(backslashes, '\\');
        backslashes = 0;
        *line += c;
    }
    line->append(backslashes * 2, '\\');
    *line += '"';
}

std::string plat_command_line(const std::vector<std::string>& argv) {
    std::string line;
    if (argv.empty()) return line;
    append_quoted(&line, argv[0]);
    const size_t slash = argv[0].find_last_of("/\\");
    const std::string base = slash == std::string::npos ? argv[0] : argv[0].substr(slash + 1);
    const bool raw = !_stricmp(base.c_str(), "cmd") || !_stricmp(base.c_str(), "cmd.exe");
    for (size_t i = 1; i < argv.size(); i++) {
        line += ' ';
        if (raw) line += argv[i];
        else append_quoted(&line, argv[i]);
    }
    return line;
}

void plat_list_drives(std::vector<std::string>* roots, std::vector<std::string>* labels) {
    wchar_t drives[512];
    DWORD n = GetLogicalDriveStringsW((DWORD)(sizeof drives / sizeof drives[0]), drives);
    if (n == 0 || n >= sizeof drives / sizeof drives[0]) return;
    for (const wchar_t* d = drives; *d; d += wcslen(d) + 1) {
        const char* kind = "drive";
        switch (GetDriveTypeW(d)) {
        case DRIVE_REMOVABLE: kind = "removable"; break;
        case DRIVE_FIXED:     kind = "fixed"; break;
        case DRIVE_REMOTE:    kind = "network"; break;
        case DRIVE_CDROM:     kind = "optical"; break;
        case DRIVE_RAMDISK:   kind = "ram disk"; break;
        }
        // Asking an empty optical drive about its volume pops a "no disk" dialog on
        // some systems; this mode turns that into a quiet failure.
        UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
        wchar_t vol[MAX_PATH + 1] = L"", fs[MAX_PATH + 1] = L"";
        BOOL have = GetVolumeInformationW(d, vol, MAX_PATH + 1, nullptr, nullptr, nullptr,
                                          fs, MAX_PATH + 1);
        SetErrorMode(old_mode);
        const char root[4] = { (char)d[0], ':', '/', '\0' };
        const std::string v = narrow(vol), f = narrow(fs);
        char label[920];
        if (have)
            snprintf(label, sizeof(label), "%-6s %-24s %s (%s)", root,
                     v.empty() ? "(no label)" : v.c_str(), kind, f.c_str());
        else
            snprintf(label, sizeof(label), "%-6s %-24s %s", root, "(not ready)", kind);
        roots->push_back(root);
        labels->push_back(label);
    }
}
#endif
