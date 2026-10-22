#include "terminal.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// How this particular program wants a command handed to it.
enum TermStyle {
    T_DASH_E,       // terminal -e prog arg          (xterm, konsole, st, urxvt, alacritty)
    T_ONE_STRING,   // terminal -e "prog arg"        (xfce4-terminal, terminator: they split
                    // the string themselves, so the arguments have to arrive as one word)
    T_SEPARATOR     // terminal -- prog arg          (gnome-terminal, kitty, foot, wezterm:
                    // without the separator their own option parser eats the command)
};

struct KnownTerm {
    const char* name;
    TermStyle style;
    const char* before;   // words the program needs before the command, space separated
};

// Probed in this order. A distribution's own wrapper first, because where it exists it
// is the machine's declared answer to "what is the terminal"; then the desktop
// environments'; then the ones people install by hand.
const KnownTerm kKnown[] = {
    {"x-terminal-emulator", T_DASH_E,      ""},
    {"gnome-terminal",      T_SEPARATOR,   "--"},
    {"konsole",             T_DASH_E,      ""},
    {"xfce4-terminal",      T_ONE_STRING,  ""},
    {"alacritty",           T_DASH_E,      ""},
    {"kitty",               T_SEPARATOR,   "--"},
    {"foot",                T_SEPARATOR,   "--"},
    {"wezterm",             T_SEPARATOR,   "launch --"},
    {"xterm",               T_DASH_E,      ""},
    {"urxvt",               T_DASH_E,      ""},
    {"st",                  T_DASH_E,      ""},
    {"qterminal",           T_DASH_E,      ""},
    {"terminator",          T_ONE_STRING,  ""},
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// The quoting for the one style that hands its command to a shell. The other styles
// pass real argv entries and need nothing.
std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

const KnownTerm* known(const std::string& name) {
    for (const KnownTerm& k : kKnown)
        if (name == k.name) return &k;
    return nullptr;
}

}   // namespace

const char* term_editor_name() {
    const char* e = getenv("EDITOR");
#ifdef _WIN32
    if (!e || trim(e).empty()) return "notepad";   // the one every Windows machine has
#else
    if (!e || trim(e).empty()) return "vim";   // the name everyone expects
#endif
    return e;
}

// A regular file the caller can execute. `access(X_OK)` alone is not enough: it is
// true for every directory, so a directory called `xterm` on PATH would be launched.
static bool exec_file(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return access(p.c_str(), X_OK) == 0;
}

#ifdef _WIN32
static const char kPathListSep = ';';
// "wt" is wt.exe: a name is tried as written and then with each extension the shell
// would add, in PATHEXT's order.
static std::vector<std::string> exec_names(const std::string& prog) {
    std::vector<std::string> out = { prog };
    const char* pe = getenv("PATHEXT");
    std::string exts = (pe && *pe) ? pe : ".COM;.EXE;.BAT;.CMD";
    for (char& c : exts) if (c == ';') c = ' ';
    for (const std::string& e : words(exts)) out.push_back(prog + e);
    return out;
}
static bool names_a_path(const std::string& prog) {
    return prog.find_first_of("/\\:") != std::string::npos;
}
#else
static const char kPathListSep = ':';
static std::vector<std::string> exec_names(const std::string& prog) { return { prog }; }
static bool names_a_path(const std::string& prog) {
    return prog.find('/') != std::string::npos;
}
#endif

bool term_which(const std::string& prog, std::string* resolved) {
    if (prog.empty()) return false;
    if (names_a_path(prog)) {
        // A path was written in the config: it means that path.
        for (const std::string& cand : exec_names(prog)) {
            if (exec_file(cand)) {
                if (resolved) *resolved = cand;
                return true;
            }
        }
        return false;
    }
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    const std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(kPathListSep, start);
        if (end == std::string::npos) end = p.size();
        const std::string dir = p.substr(start, end - start);
        if (!dir.empty()) {
            for (const std::string& cand : exec_names(dir + "/" + prog)) {
                if (exec_file(cand)) {
                    if (resolved) *resolved = cand;
                    return true;
                }
            }
        }
        if (end == p.size()) break;
        start = end + 1;
    }
    return false;
}

std::vector<std::string> term_launch_argv(const char* name, const char* editor,
                                          const std::string& path) {
    std::vector<std::string> argv;
    if (!name || !*name || !editor || !*editor) return argv;
    const KnownTerm* k = known(name);
    // An unknown program gets the convention most of the rest follow, which is also
    // the one a person writing `terminal = my-terminal` most likely expects.
    const TermStyle style = k ? k->style : T_DASH_E;
    argv.push_back(name);
    if (style == T_ONE_STRING) {
        argv.push_back("-e");
        argv.push_back(std::string(editor) + " " + quote(path));
        return argv;
    }
    if (k && k->before && *k->before)
        for (const std::string& w : words(k->before)) argv.push_back(w);
    else if (style == T_DASH_E) argv.push_back("-e");
    argv.push_back(editor);
    argv.push_back(path);
    return argv;
}

const std::vector<std::string>& term_candidates() {
    static std::vector<std::string> v;
#ifndef _WIN32
    // Windows has no list to walk: every console program gets a window of its own.
    if (v.empty())
        for (const KnownTerm& k : kKnown) v.push_back(k.name);
#endif
    return v;
}

#ifdef _WIN32
// The editor on its own, in a console window of its own. A console program started
// that way gets a console - which Windows 11 opens in whatever its default terminal
// app is - and a GUI editor like Notepad ignores it. A .cmd or .bat editor (VS Code's
// `code` is code.cmd) is a script that only cmd can run.
static bool editor_argv(const char* editor, const std::string& path,
                        std::vector<std::string>* argv) {
    std::string resolved;
    if (!term_which(editor, &resolved)) return false;
    const size_t dot = resolved.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : resolved.substr(dot);
    if (!_stricmp(ext.c_str(), ".cmd") || !_stricmp(ext.c_str(), ".bat")) {
        // cmd /s strips the outer pair of quotes and runs the rest as it stands.
        *argv = { "cmd", "/d", "/s", "/c", "\"\"" + resolved + "\" \"" + path + "\"\"" };
    } else {
        *argv = { editor, path };
    }
    return true;
}
#endif

TermLaunch term_find_launch(const char* preferred, const std::string& path) {
    TermLaunch out;
    const char* editor = term_editor_name();

    auto list = [&]() {
        std::string s;
        for (const std::string& c : term_candidates()) {
            if (!s.empty()) s += ", ";
            s += c;
        }
        return s;
    };

    // A name taken from the config or the environment. It may be just a name, or a
    // name with the flags that program needs written out ("gnome-terminal --").
    auto try_one = [&](const std::string& value, TermLaunch* into) {
        const std::vector<std::string> w = words(value);
        if (w.empty()) return false;
        std::string resolved;
        if (!term_which(w[0], &resolved)) return false;
        if (w.size() > 1) {
            // The shape was typed on purpose; do not second-guess it. argv[0] is the
            // name as looked up, the same as every other branch, so what the status
            // line reports and what gets exec'd are the same word.
            into->argv.push_back(w[0]);
            for (size_t i = 1; i < w.size(); i++) into->argv.push_back(w[i]);
            into->argv.push_back(editor);
            into->argv.push_back(path);
        } else {
            into->argv = term_launch_argv(w[0].c_str(), editor, path);
        }
        into->name = w[0];
        return !into->argv.empty();
    };

    const std::string pref = trim(preferred ? preferred : "");
    if (!pref.empty()) {
        if (try_one(pref, &out)) return out;
        // The person who wrote the line chose that terminal. Silently launching a
        // different one would be a setting that appears to do nothing at all.
        out.note = "terminal = " + words(pref)[0] + " is not on this machine"
                   " (it is what the config file names)";
        return out;
    }

    const char* env = getenv("TERMINAL");
    if (env && !trim(env).empty() && try_one(trim(env), &out)) return out;

#ifdef _WIN32
    if (editor_argv(editor, path, &out.argv)) {
        out.name = "a window of its own";
        return out;
    }
    out.note = std::string(editor) + " is not on this machine. Set EDITOR to an editor that is,"
               " or terminal = in the config file to run it in a terminal of your choosing.";
    (void)list;
    return out;
#else
    for (const std::string& c : term_candidates()) {
        std::string resolved;
        if (!term_which(c, &resolved)) continue;
        out.argv = term_launch_argv(c.c_str(), editor, path);
        out.name = c;
        return out;
    }

    out.note = "No terminal emulator found. Looked for: " + list() +
               ". Set terminal = in the config file, or $TERMINAL.";
    return out;
#endif
}
