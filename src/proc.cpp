// The child-process runner. See proc.h for what it is for and why popen() is
// not enough. Extracted from archive.cpp unchanged, so the archive backends and
// the console share one implementation of "run a program and take its output".

#include "proc.h"

#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

const std::vector<std::string> kNoEnv;

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

int proc_finish(Proc* p, bool aborted) {
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
