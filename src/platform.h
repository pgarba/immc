#ifndef MC_PLATFORM_H
#define MC_PLATFORM_H

#include <string>
#include <vector>

// The few places where Linux and Windows do not just spell the same thing
// differently, but mean different things: where a user's files live, where settings
// go, and how a program is handed to the desktop. Everything here answers with '/'
// as the separator, which Windows accepts everywhere this program passes a path, so
// a path built from these joins with "/" + name like any other.

// The user's home: $HOME, or on Windows %USERPROFILE% when HOME is not set. Empty
// when neither says.
std::string plat_home_dir();

// Where the config file, the hotlist and the sites file live:
// ~/.config/mc-clone on Linux, %APPDATA%\mc-clone on Windows.
std::string plat_config_dir();

// Make plat_config_dir() and its parent exist. Already existing is not a failure.
void plat_make_config_dir();

// The directory temporary files go in: $TMPDIR or /tmp, and on Windows the
// directory GetTempPath names.
std::string plat_temp_dir();

// Is this an absolute local path? "/x" on Linux; "C:/x", "C:\x", "\\server\share"
// and "/x" (the current drive's root) on Windows.
bool plat_path_is_absolute(const char* path);

// The root a path starts with, or of the system when the path has none: "/" on Linux,
// "C:/" on Windows. Where "Go to root" goes.
std::string plat_root_of(const char* path);

// Start a program and do not wait for it. On Windows it gets a console window of its
// own, which is what a terminal editor needs and what a GUI program ignores.
bool plat_spawn_detached(const std::vector<std::string>& argv, std::string* err);

// Hand a file to the desktop's default application for it: xdg-open on Linux, the
// shell's "open" verb on Windows.
bool plat_open_default(const char* path, std::string* err);

// What a console command wrote, as UTF-8. On Windows cmd and its built-ins write to a
// pipe in the OEM code page ("ü" is 0x81), whatever chcp says, while git, Python and
// most newer tools write UTF-8. Text that is already valid UTF-8 is kept as it is;
// anything else is read as the OEM code page. Elsewhere the bytes are returned
// unchanged. Hand it whole lines: a character split across two calls is not valid
// UTF-8 on either side of the split.
std::string plat_console_text(const char* s, size_t n);

#ifdef _WIN32
// Text a program wrote in the system's ANSI code page (1252 on a Western install), as
// UTF-8. Windows' own tar is such a program: it prints member names that way. Not
// CP_ACP, which this process's manifest has made UTF-8.
std::string plat_from_ansi(const std::string& s);

// argv as one Windows command line, quoted the way the CRT and CommandLineToArgvW
// split it back apart. When argv[0] is cmd or cmd.exe the words after it go in
// untouched: cmd parses its own command line, and the quoting every other program
// expects is not the quoting it understands.
std::string plat_command_line(const std::vector<std::string>& argv);

// Every drive letter, as "C:/", with a line describing it for the drive picker: its
// label and file system when the volume answers. A card reader with no card in it
// does not, and is listed by letter and kind alone rather than left out - the letter
// is still there.
void plat_list_drives(std::vector<std::string>* roots, std::vector<std::string>* labels);
#endif

#endif
