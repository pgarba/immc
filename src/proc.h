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
//   - a pid, so a cancel is a signal and not a hope, and an exit code that says
//     which of "exited", "was killed" and "could not be started" happened.
//
// The caller reads p->fd itself, in whatever cadence suits it: the archive
// runners poll it between progress updates, the console reads it on a worker
// thread. proc_finish is what closes the process out, and it is the only place
// that waits.
struct Proc {
    pid_t pid = -1;
    int fd = -1;
};

// Passed to proc_start when the caller has no environment changes to make.
extern const std::vector<std::string> kNoEnv;

// Start argv in cwd (empty or null = inherit) with extra "NAME=value" entries
// applied in the child. Returns false with a message in err when the pipe or
// the fork failed; a failed chdir or exec is not a start failure, it is a child
// that exits 127, which is what a shell reports for a command it cannot run.
bool proc_start(const std::vector<std::string>& argv, const char* cwd,
                const std::vector<std::string>& env,
                Proc* p, std::string* err);

// Close the process out. aborted=false waits for it and returns its exit code,
// or 128+signal when a signal killed it. aborted=true sends SIGTERM, gives the
// tool a moment to clean up after itself, then SIGKILL. Both close p->fd.
// Returns -1 when there was no process.
int proc_finish(Proc* p, bool aborted);

#endif
