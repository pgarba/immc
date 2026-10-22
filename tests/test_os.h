#ifndef MC_TEST_OS_H
#define MC_TEST_OS_H

// What the suites need from the machine they run on, said once.
//
// The suites build their fixtures with shell lines - `mkdir -p`, `printf ... >`, `rm -rf`
// - because a line of sh says what a fixture is more plainly than the C to make it. On
// Windows those lines run in the sh that Git for Windows installs, which has the same
// tools; cmd.exe, which is what system() would start, has none of them. And the scratch
// trees go in the system's temporary directory, which on Windows is not /tmp.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <process.h>
#endif

// The directory scratch trees are made in: /tmp, or %TEMP% with '/' separators.
static inline std::string test_tmp_dir() {
#ifdef _WIN32
    const char* t = getenv("TEMP");
    if (!t || !*t) t = getenv("TMP");
    std::string s = (t && *t) ? t : "C:/Windows/Temp";
    for (char& c : s) if (c == '\\') c = '/';
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
#else
    return "/tmp";
#endif
}

// A fresh directory under test_tmp_dir() named prefix-XXXXXX, or "" when none could
// be made.
static inline std::string test_mkdtemp(const char* prefix) {
    std::string t = test_tmp_dir() + "/" + prefix + "-XXXXXX";
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", t.c_str());
    const char* d = mkdtemp(buf);
    return d ? std::string(d) : std::string();
}

// Point the app's home - and with it the settings directory - at a scratch tree for
// as long as this lives, and put the real one back after. Settings are
// ~/.config/mc-clone on Linux and %APPDATA%\mc-clone on Windows, so APPDATA is pointed
// at <dir>/.config: a test can expect the same file in the same place on both.
// The old values are copied, not kept as getenv() pointers, which setenv() is free
// to invalidate.
struct TestHome {
    struct Saved { const char* name; bool had; std::string value; };
    Saved saved[2] = { { "HOME", false, "" }, { "APPDATA", false, "" } };

    explicit TestHome(const std::string& dir) {
        for (Saved& s : saved) {
            const char* v = getenv(s.name);
            s.had = v != nullptr;
            s.value = v ? v : "";
        }
        setenv("HOME", dir.c_str(), 1);
#ifdef _WIN32
        setenv("APPDATA", (dir + "/.config").c_str(), 1);
#endif
    }
    ~TestHome() {
        for (Saved& s : saved) {
            if (s.had) setenv(s.name, s.value.c_str(), 1);
            else unsetenv(s.name);
        }
    }
    TestHome(const TestHome&) = delete;
    TestHome& operator=(const TestHome&) = delete;
};

// Run one line of sh and return 0 when it succeeded, as system() does.
static inline int test_system(const std::string& cmd) {
#ifdef _WIN32
    // The tar a fixture is made with is the one the app runs: Windows' own bsdtar in
    // System32, which CreateProcess finds before anything on PATH. Git's GNU tar would
    // also read "C:/x.tar" as a file on a host called C.
    const std::string line = "tar() { \"$SYSTEMROOT/System32/tar.exe\" \"$@\"; }; " + cmd;
    // One argument, quoted the way the MSYS runtime splits a command line back apart.
    std::string q = "\"";
    size_t bs = 0;
    for (char c : line) {
        if (c == '\\') { bs++; continue; }
        q.append(c == '"' ? bs * 2 + 1 : bs, '\\');
        bs = 0;
        q += c;
    }
    q.append(bs * 2, '\\');
    q += '"';
    fflush(stdout);
    fflush(stderr);
    const char* argv[] = { "sh", "-c", q.c_str(), nullptr };
    intptr_t rc = _spawnvp(_P_WAIT, "sh", argv);
    return rc < 0 ? -1 : (int)rc;
#else
    return system(cmd.c_str());
#endif
}

#endif
