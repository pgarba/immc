#ifndef MC_PROC_H
#define MC_PROC_H

#include <sys/types.h>

#include <string>
#include <vector>

// The child-process runner shared by the archive backends and the console.
//
// Both need the same four things, and none of them come from popen():
//
//   - argv instead of a shell string, so a member name with a space, a quote or
//     a glob character cannot change what the tool does;
//   - stdout and stderr joined on one pipe, because a tool's explanation of a
//     failure is not separable from its progress and the caller wants both in
//     reading order;
//   - /dev/null on stdin, so a tool that wants a password fails immediately
//     instead of waiting for a terminal this GUI process does not have;
//   - a handle on the process, so a cancel is a signal and not a hope, and an exit
//     code that says which of "exited", "was killed" and "could not be started"
//     happened.
//
// The caller reads with proc_read, in whatever cadence suits it: the archive
// runners poll it between progress updates, the console reads it on a worker
// thread. proc_finish is what closes the process out, and it is the only place
// that waits.
struct Proc {
#ifdef _WIN32
    void* process = nullptr;   // HANDLE of the child
    void* job = nullptr;       // the job it runs in, so a cancel takes its children too
    void* out = nullptr;       // read end of the pipe
    int code = -1;             // exit code of a child that never started (127)
    std::string pending;       // what that child would have said, read before EOF
#else
    pid_t pid = -1;
    int fd = -1;
#endif
};

// Passed to proc_start when the caller has no environment changes to make.
extern const std::vector<std::string> kNoEnv;

// Start argv in cwd (empty or null = inherit) with extra "NAME=value" entries
// applied in the child. Returns false with a message in err when the pipe or
// the fork failed; a failed chdir or exec is not a start failure, it is a child
// that exits 127, which is what a shell reports for a command it cannot run.
//
// On Windows a program named without a path is looked up the way CreateProcess
// looks it up, and an argv whose first word is cmd or cmd.exe has the words after
// it passed through untouched: cmd parses its own command line, and the quoting
// that every other program expects is not the quoting it understands.
bool proc_start(const std::vector<std::string>& argv, const char* cwd,
                const std::vector<std::string>& env,
                Proc* p, std::string* err);

// Read what the child has written to stdout or stderr, waiting at most timeout_ms
// for something to arrive. Returns the number of bytes read, 0 at end of output,
// -1 when the time ran out with nothing to read, and -2 on an error.
long proc_read(Proc* p, char* buf, size_t n, int timeout_ms);

// Close the process out. aborted=false waits for it and returns its exit code,
// or 128+signal when a signal killed it. aborted=true sends SIGTERM, gives the
// tool a moment to clean up after itself, then SIGKILL; on Windows it ends the
// whole job. Both close the pipe. Returns -1 when there was no process.
int proc_finish(Proc* p, bool aborted);

// The argv that runs one line through the system's shell: /bin/sh -c on POSIX,
// cmd.exe on Windows. Pipes, redirection and globbing are what the console's
// commands are typed for.
std::vector<std::string> proc_shell_argv(const std::string& line);

#endif
