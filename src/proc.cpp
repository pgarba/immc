// The child-process runner. See proc.h for what it is for and why popen() is
// not enough. Extracted from archive.cpp unchanged, so the archive backends and
// the console share one implementation of "run a program and take its output".

#include "proc.h"
#include "platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

const std::vector<std::string> kNoEnv;

#ifndef _WIN32

bool proc_start(const std::vector<std::string>& argv, const char* cwd,
                const std::vector<std::string>& env,
                Proc* p, std::string* err) {
    int pfd[2];
    if (pipe(pfd) != 0) { *err = std::strerror(errno); return false; }
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        *err = std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);   // errors belong in the same stream
        // A tool that wants a password asks on stdin. We never read that prompt,
        // so leaving the terminal attached would leave the child waiting and the
        // UI frozen with a progress bar that never moves. /dev/null turns the
        // prompt into an immediate failure the caller can report.
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        close(pfd[0]); close(pfd[1]);
        for (const std::string& e : env) {
            size_t eq = e.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            setenv(e.substr(0, eq).c_str(), e.substr(eq + 1).c_str(), 1);
        }
        if (cwd && *cwd && chdir(cwd) != 0) _exit(127);
        std::vector<char*> cs;
        for (const std::string& s : argv) cs.push_back(const_cast<char*>(s.c_str()));
        cs.push_back(nullptr);
        execvp(cs[0], cs.data());
        _exit(127);                    // exec failed: 127 is the shell's code for it
    }
    close(pfd[1]);
    p->pid = pid;
    p->fd = pfd[0];
    return true;
}

long proc_read(Proc* p, char* buf, size_t n, int timeout_ms) {
    if (p->fd < 0) return 0;
    for (;;) {
        struct pollfd pf;
        pf.fd = p->fd;
        pf.events = POLLIN;
        pf.revents = 0;
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -2;
        }
        if (r == 0) return -1;
        ssize_t got = read(p->fd, buf, n);
        if (got >= 0) return (long)got;
        if (errno == EINTR || errno == EAGAIN) continue;
        return -2;
    }
}

int proc_finish(Proc* p, bool aborted) {
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    if (p->pid < 0) return -1;
    if (aborted) {
        kill(p->pid, SIGTERM);
        // Give the tool a moment to clean up after itself, then stop insisting.
        for (int i = 0; i < 20; i++) {
            int st = 0;
            if (waitpid(p->pid, &st, WNOHANG) == p->pid) { p->pid = -1; break; }
            usleep(50000);
        }
        if (p->pid > 0) { kill(p->pid, SIGKILL); waitpid(p->pid, nullptr, 0); p->pid = -1; }
    } else {
        int st = 0;
        waitpid(p->pid, &st, 0);
        p->pid = -1;
        if (WIFEXITED(st))  return WEXITSTATUS(st);
        if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    }
    return -1;
}

std::vector<std::string> proc_shell_argv(const std::string& line) {
    return { "/bin/sh", "-c", line };
}

#else   // _WIN32

static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string last_error_text(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (wchar_t*)&msg, 0, nullptr);
    std::string out;
    if (msg) {
        int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
        out.assign((size_t)(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, msg, -1, &out[0], n, nullptr, nullptr);
        LocalFree(msg);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    if (out.empty()) out = "error " + std::to_string(e);
    return out;
}

// The parent's environment with the caller's NAME=value entries laid over it, as the
// sorted, double-NUL-terminated block CreateProcessW wants.
static std::wstring env_block(const std::vector<std::string>& extra) {
    std::vector<std::wstring> vars;
    wchar_t* cur = GetEnvironmentStringsW();
    for (const wchar_t* e = cur; e && *e; e += wcslen(e) + 1) vars.push_back(e);
    if (cur) FreeEnvironmentStringsW(cur);
    for (const std::string& x : extra) {
        size_t eq = x.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::wstring w = widen(x);
        std::wstring name = widen(x.substr(0, eq + 1));
        bool replaced = false;
        for (std::wstring& v : vars) {
            if (v.size() >= name.size() && !_wcsnicmp(v.c_str(), name.c_str(), name.size())) {
                v = w;
                replaced = true;
            }
        }
        if (!replaced) vars.push_back(w);
    }
    std::wstring block;
    for (const std::wstring& v : vars) { block += v; block += L'\0'; }
    block += L'\0';
    return block;
}

bool proc_start(const std::vector<std::string>& argv, const char* cwd,
                const std::vector<std::string>& env,
                Proc* p, std::string* err) {
    if (argv.empty()) { *err = "nothing to run"; return false; }

    std::wstring line = widen(plat_command_line(argv));

    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { *err = last_error_text(GetLastError()); return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // only the child's end crosses over
    // A tool that wants a password asks on stdin. NUL turns the prompt into an
    // immediate failure the caller can report, instead of a wait on a console this
    // GUI process does not have.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    // Only these two handles are inherited. Two children started at once - a console
    // command and an archive listing - would otherwise each hold the other's pipe open,
    // and neither reader would ever see the end of its output.
    HANDLE inherit[2] = { wr, nul };
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_mem(attr_size);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_mem.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                              (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE),
                              nullptr, nullptr);

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;   // errors belong in the same stream
    si.lpAttributeList = attrs;

    std::wstring envb = env.empty() ? std::wstring() : env_block(env);
    std::wstring wcwd = (cwd && *cwd) ? widen(cwd) : std::wstring();

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    // Suspended until it is in the job, so nothing it starts can escape a cancel.
    // CREATE_NO_WINDOW: a console tool started from a GUI app would otherwise open a
    // console window of its own for as long as it runs.
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                  CREATE_SUSPENDED | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE, flags,
                             envb.empty() ? nullptr : (void*)envb.data(),
                             wcwd.empty() ? nullptr : wcwd.c_str(),
                             &si.StartupInfo, &pi);
    DWORD start_err = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!ok) {
        CloseHandle(rd);
        // A program that is not there, or a directory that is not there, is a child
        // that exits 127 - the same answer a POSIX shell gives - and what it would have
        // said is the first thing the caller reads.
        if (start_err == ERROR_FILE_NOT_FOUND || start_err == ERROR_PATH_NOT_FOUND ||
            start_err == ERROR_DIRECTORY || start_err == ERROR_BAD_EXE_FORMAT ||
            start_err == ERROR_ACCESS_DENIED) {
            p->code = 127;
            p->pending = "cannot run " + argv[0] + ": " + last_error_text(start_err) + "\n";
            return true;
        }
        *err = last_error_text(start_err);
        return false;
    }

    p->job = CreateJobObjectW(nullptr, nullptr);
    if (p->job) AssignProcessToJobObject((HANDLE)p->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->out = rd;
    return true;
}

long proc_read(Proc* p, char* buf, size_t n, int timeout_ms) {
    if (!p->pending.empty()) {
        size_t k = p->pending.size() < n ? p->pending.size() : n;
        memcpy(buf, p->pending.data(), k);
        p->pending.erase(0, k);
        return (long)k;
    }
    if (!p->out) return 0;
    // An anonymous pipe cannot be waited on, so it is peeked at. The end of the output
    // is either the pipe closing or the child exiting with nothing left in it: a
    // program the child started in the background (`start notepad`) inherits the
    // pipe, and would otherwise hold the command open until it was closed itself.
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)p->out, nullptr, 0, nullptr, &avail, nullptr))
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -2;
        if (avail > 0) {
            DWORD got = 0;
            DWORD want = (DWORD)(n < avail ? n : avail);
            if (!ReadFile((HANDLE)p->out, buf, want, &got, nullptr))
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -2;
            return (long)got;
        }
        if (p->process && WaitForSingleObject((HANDLE)p->process, 0) == WAIT_OBJECT_0) {
            // Exited. One more look, for output written just before it went.
            if (PeekNamedPipe((HANDLE)p->out, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
                continue;
            return 0;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return -1;
        ULONGLONG left = deadline - now;
        Sleep((DWORD)(left < 10 ? left : 10));
    }
}

int proc_finish(Proc* p, bool aborted) {
    if (p->out) { CloseHandle((HANDLE)p->out); p->out = nullptr; }
    p->pending.clear();
    if (!p->process) {
        int code = p->code;
        p->code = -1;
        return code;
    }
    int code = -1;
    if (aborted) {
        // There is no SIGTERM to give a tool a moment with. Ending the job takes the
        // tool and whatever it started, which is what a cancel means.
        if (p->job) TerminateJobObject((HANDLE)p->job, 1);
        else TerminateProcess((HANDLE)p->process, 1);
        WaitForSingleObject((HANDLE)p->process, 5000);
    } else {
        WaitForSingleObject((HANDLE)p->process, INFINITE);
        DWORD ec = 0;
        if (GetExitCodeProcess((HANDLE)p->process, &ec)) code = (int)ec;
    }
    CloseHandle((HANDLE)p->process);
    p->process = nullptr;
    if (p->job) { CloseHandle((HANDLE)p->job); p->job = nullptr; }
    return code;
}

std::vector<std::string> proc_shell_argv(const std::string& line) {
    // /d skips AutoRun from the registry, /s keeps the quotes inside the line as typed:
    // cmd strips the outer pair and runs the rest exactly. What it writes is in the
    // OEM code page; plat_console_text() is where that becomes UTF-8.
    const char* comspec = getenv("ComSpec");
    return { comspec && *comspec ? comspec : "cmd.exe", "/d", "/s", "/c", "\"" + line + "\"" };
}

#endif
