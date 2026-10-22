#ifndef MC_WIN_POSIX_H
#define MC_WIN_POSIX_H

// The slice of POSIX this program uses, for MSVC.
//
// The code was written against Linux, and most of it is the kind of POSIX that has an
// obvious Windows meaning: list a directory, stat a file, make a folder. This header
// gives those calls that meaning, so the code that lists, copies and deletes reads the
// same on both systems. The parts that do not translate - starting a process, the
// trash, the list of mounts, who owns a file - are not here; they are written out for
// Windows where they are used.
//
// It is force-included into every translation unit of the app and its tests (see
// CMakeLists.txt), so a header that only says `mode_t` gets the same type as the file
// that includes it. Paths are UTF-8 throughout: the executables carry a manifest that
// sets the process code page to UTF-8, which is what makes fopen() and the CRT's other
// narrow-string functions take the same bytes the rest of the program passes around.
// The functions defined here convert to UTF-16 themselves and do not depend on that.
//
// The CRT headers that declare these names are included first, on purpose: several
// of the definitions below are macros (stat, mkdir, access, rename), and a CRT header
// included after them would have its own declarations rewritten.

#ifndef _WIN32
#error "src/win/posix.h is for Windows builds only"
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif

// off_t is a 32-bit long in the CRT, which would make every file over 2 GB the wrong
// size in a pane. The CRT's own _off_t keeps its width, because struct _stat is laid
// out with it; only the POSIX name gets 64 bits.
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef long _off_t;
typedef long long off_t;
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
extern "C" {
#endif

typedef unsigned int mode_t;
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef long long ssize_t;

// ---- file types and permission bits ------------------------------------------------
//
// The CRT defines S_IFMT, S_IFDIR, S_IFCHR and S_IFREG with the POSIX values. The rest
// use the values Linux uses, so a mode built on one system means the same on the other.
#ifndef S_IFLNK
#define S_IFLNK  0xA000
#endif
#ifndef S_IFIFO
#define S_IFIFO  0x1000
#endif
#ifndef S_IFBLK
#define S_IFBLK  0x6000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0xC000
#endif

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

// ---- stat ---------------------------------------------------------------------------
//
// The CRT's struct stat has a 16-bit mode, a 32-bit size and no idea what a symbolic
// link is. This one is filled from the file's attributes instead. The mode is made up
// the way Windows' own POSIX layers make it up: readable always, writable unless the
// read-only attribute is set, executable for a directory and for the extensions the
// shell would run. That is enough for the panes to colour an .exe and for the
// properties dialog to show something true about the read-only bit.
struct mc_stat {
    unsigned int st_dev;
    unsigned long long st_ino;
    mode_t st_mode;
    unsigned int st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    off_t st_size;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
    unsigned long st_attrib;   // the raw FILE_ATTRIBUTE_* bits, for the few who ask
};

int mc_stat(const char* path, struct mc_stat* st);    // follows a link
int mc_lstat(const char* path, struct mc_stat* st);   // describes the link itself
int mc_fstat(int fd, struct mc_stat* st);

#define stat  mc_stat
#define lstat mc_lstat
#define fstat mc_fstat

// ---- directories --------------------------------------------------------------------

struct dirent {
    char d_name[1024];        // UTF-8; a 255-character NTFS name can need 765 bytes
    unsigned long d_attrib;   // FILE_ATTRIBUTE_* of the entry, which readdir has for free
};

typedef struct MC_DIR MC_DIR;
#define DIR MC_DIR

DIR* opendir(const char* path);
struct dirent* readdir(DIR* d);
void rewinddir(DIR* d);
int closedir(DIR* d);

int mc_mkdir(const char* path, mode_t mode);
#define mkdir(p, m) mc_mkdir((p), (m))
int mc_rmdir(const char* path);
#define rmdir mc_rmdir
int mc_unlink(const char* path);
#define unlink mc_unlink

// POSIX rename replaces an existing file; the CRT's refuses. Callers here rely on the
// POSIX meaning (a temporary written next to a file, then put in its place).
int mc_rename(const char* from, const char* to);
#define rename mc_rename

// ---- access -------------------------------------------------------------------------

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
// The CRT's _access rejects X_OK as an invalid argument. Here X_OK asks whether the
// shell would run it: a file with an extension from PATHEXT.
int mc_access(const char* path, int mode);
#define access mc_access

int mc_chmod(const char* path, mode_t mode);   // only the owner's write bit means anything
#define chmod mc_chmod

int symlink(const char* target, const char* linkpath);
// The target a link was made with, with '/' for the separators symlink() turned into
// backslashes. Not NUL-terminated, as in POSIX.
ssize_t readlink(const char* path, char* buf, size_t size);

#define AT_FDCWD (-100)
int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);

// ---- the rest -----------------------------------------------------------------------

char* mkdtemp(char* tmpl);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int usleep(unsigned int usec);

// dirname() and basename() in the POSIX manner - they may write into their argument
// and return a pointer into it - but aware that "C:/" is a root the way "/" is.
char* dirname(char* path);
char* basename(char* path);

// The 64-bit seek, under the name POSIX gives the off_t one: a resumed upload of a
// file over 2 GB starts at the right byte.
#define fseeko _fseeki64
#define ftello _ftelli64

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

static __inline struct tm* localtime_r(const time_t* t, struct tm* out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
static __inline struct tm* gmtime_r(const time_t* t, struct tm* out) {
    return gmtime_s(out, t) == 0 ? out : NULL;
}
#define timegm _mkgmtime

#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  0
int clock_gettime(int clock, struct timespec* ts);

#ifdef __cplusplus
}
#endif

#endif
