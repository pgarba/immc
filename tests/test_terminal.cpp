// Finding a terminal to run an editor in, and the exact command line each one wants.
//
// The bug this suite exists for: a machine had alacritty and none of the five names the
// app probed, so F4 said "No terminal emulator found for vim" - naming the editor while
// the terminal was what was missing. The two things worth pinning down are the lookup
// order and the argument shape, because the shape is where a launch that looks correct
// opens a window that says nothing.

#include "terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(x) do { checks++; if (!(x)) { \
    failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static std::string tmp_root;

// A directory of fake terminals, so the lookup can be pointed at a machine that does
// not have the real ones. They are scripts that print their argv, which also makes the
// shape observable from outside the process if a check ever needs that.
static void make_fake(const std::string& name) {
    std::string p = tmp_root + "/" + name;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "#!/bin/sh\nprintf '%%s\\n' \"$0\" \"$@\"\n");
    fclose(f);
    chmod(p.c_str(), 0755);
}

static void use_path(const std::string& path) { setenv("PATH", path.c_str(), 1); }

static void test_editor_name(void) {
    unsetenv("EDITOR");
    CHECK(!strcmp(term_editor_name(), "vim"));
    setenv("EDITOR", "nano", 1);
    CHECK(!strcmp(term_editor_name(), "nano"));
    // An EDITOR that is set but empty is not an editor.
    setenv("EDITOR", "   ", 1);
    CHECK(!strcmp(term_editor_name(), "vim"));
    unsetenv("EDITOR");
}

static bool is(const std::vector<std::string>& got, const std::vector<std::string>& want) {
    if (got != want) {
        printf("   argv:");
        for (const std::string& a : got) printf(" [%s]", a.c_str());
        printf("\n  want:");
        for (const std::string& a : want) printf(" [%s]", a.c_str());
        printf("\n");
        return false;
    }
    return true;
}

static void test_shapes(void) {
    const std::string p = "/tmp/some file.txt";

    // The -e convention: the command arrives as separate argv entries, which is what
    // makes a path with a space in it safe without quoting.
    CHECK(is(term_launch_argv("xterm", "vim", p), {"xterm", "-e", "vim", p}));
    CHECK(is(term_launch_argv("konsole", "vim", p), {"konsole", "-e", "vim", p}));
    CHECK(is(term_launch_argv("alacritty", "vim", p), {"alacritty", "-e", "vim", p}));
    CHECK(is(term_launch_argv("st", "vim", p), {"st", "-e", "vim", p}));
    CHECK(is(term_launch_argv("urxvt", "vim", p), {"urxvt", "-e", "vim", p}));
    CHECK(is(term_launch_argv("qterminal", "vim", p), {"qterminal", "-e", "vim", p}));

    // The separator style: without `--` these eat the command as their own options.
    CHECK(is(term_launch_argv("gnome-terminal", "vim", p), {"gnome-terminal", "--", "vim", p}));
    CHECK(is(term_launch_argv("kitty", "vim", p), {"kitty", "--", "vim", p}));
    CHECK(is(term_launch_argv("foot", "vim", p), {"foot", "--", "vim", p}));
    CHECK(is(term_launch_argv("wezterm", "vim", p), {"wezterm", "launch", "--", "vim", p}));

    // The one-string style: these split the string themselves, so the path has to
    // arrive quoted inside it.
    CHECK(is(term_launch_argv("xfce4-terminal", "vim", p),
            {"xfce4-terminal", "-e", "vim '/tmp/some file.txt'"}));
    CHECK(is(term_launch_argv("terminator", "vim", p),
            {"terminator", "-e", "vim '/tmp/some file.txt'"}));

    // An apostrophe in a filename survives that quoting instead of ending it.
    CHECK(is(term_launch_argv("terminator", "vim", "/tmp/it's"),
            {"terminator", "-e", "vim '/tmp/it'\\''s'"}));

    // No path quoting for the styles that pass argv: the space is still one argument.
    CHECK(is(term_launch_argv("xterm", "vim", "/tmp/it's"), {"xterm", "-e", "vim", "/tmp/it's"}));

    // An unknown program gets the common convention rather than a guess at its dialect.
    CHECK(is(term_launch_argv("my-terminal", "vim", p), {"my-terminal", "-e", "vim", p}));

    // Nothing to name is nothing to run.
    CHECK(term_launch_argv("", "vim", p).empty());
    CHECK(term_launch_argv(nullptr, "vim", p).empty());
    CHECK(term_launch_argv("xterm", "", p).empty());

    // The editor is the second-to-last word, whatever the shape.
    CHECK(is(term_launch_argv("foot", "nvim", "/x"), {"foot", "--", "nvim", "/x"}));
}

static void test_which(void) {
    make_fake("alacritty");
    use_path(tmp_root);

    std::string resolved;
    CHECK(term_which("alacritty", &resolved));
    CHECK(resolved == tmp_root + "/alacritty");

    // Not on PATH, and not a path.
    CHECK(!term_which("definitely-not-here", &resolved));

    // A file that is there but not executable is not a program.
    FILE* f = fopen((tmp_root + "/plain").c_str(), "w");
    if (f) { fputs("hi\n", f); fclose(f); }
    chmod((tmp_root + "/plain").c_str(), 0644);
    CHECK(!term_which("plain", &resolved));

    // A directory named like a program is not one either.
    mkdir((tmp_root + "/adir").c_str(), 0755);
    CHECK(!term_which("adir", &resolved));

    // A path written out in the config means that path.
    CHECK(term_which(tmp_root + "/alacritty", &resolved));
    CHECK(!term_which(tmp_root + "/plain", &resolved));

    // Empty is not a program, and a PATH full of missing directories is not a find.
    CHECK(!term_which("", &resolved));
    use_path("/nonexistent-dir-xyz:/also-missing");
    CHECK(!term_which("alacritty", &resolved));
    // An empty component does not turn into the current directory.
    use_path(":");
    CHECK(!term_which("alacritty", &resolved));
}

static void test_lookup(void) {
    const std::string p = "/tmp/file.txt";
    unsetenv("TERMINAL");
    unsetenv("EDITOR");

    // The table finds the one that is there, and this is the case that was missing:
    // a machine whose only terminal is alacritty.
    make_fake("alacritty");
    use_path(tmp_root);
    TermLaunch l = term_find_launch("", p);
    CHECK(l.name == "alacritty");
    // argv[0] is the name as looked up, not the path it resolved to: the status line,
    // the argv, and what execvp then finds are all the same word.
    CHECK(is(l.argv, {"alacritty", "-e", "vim", p}));
    CHECK(l.note.empty());

    // The order is the distribution's wrapper first where it exists.
    make_fake("x-terminal-emulator");
    l = term_find_launch("", p);
    CHECK(l.name == "x-terminal-emulator");

    // A value in the config wins over everything, and its shape is honoured.
    make_fake("gnome-terminal");
    l = term_find_launch("gnome-terminal --", p);
    CHECK(l.name == "gnome-terminal");
    CHECK(is(l.argv, {"gnome-terminal", "--", "vim", p}));

    // A bare name in the config uses that program's known shape, not a guess.
    make_fake("kitty");
    l = term_find_launch("kitty", p);
    CHECK(l.name == "kitty");
    CHECK(is(l.argv, {"kitty", "--", "vim", p}));

    // A config value that names a program which is not there is reported. Silently
    // launching somebody else's terminal would look like a setting that does nothing.
    l = term_find_launch("dumb-terminal", p);
    CHECK(l.argv.empty());
    CHECK(l.note.find("dumb-terminal") != std::string::npos);
    CHECK(l.note.find("config") != std::string::npos);

    // $TERMINAL is honoured, and the known shape is used for a known name.
    make_fake("foot");
    setenv("TERMINAL", "foot", 1);
    l = term_find_launch("", p);
    CHECK(l.name == "foot");
    CHECK(is(l.argv, {"foot", "--", "vim", p}));

    // $TERMINAL may carry flags too, same as the config line.
    setenv("TERMINAL", "foot --hold", 1);
    l = term_find_launch("", p);
    CHECK(is(l.argv, {"foot", "--hold", "vim", p}));

    // An $TERMINAL left over from a shell profile that no longer points at anything
    // falls through to the table, which is the friendlier half of the config rule.
    setenv("TERMINAL", "gone-terminal", 1);
    l = term_find_launch("", p);
    CHECK(l.name == "x-terminal-emulator");
    unsetenv("TERMINAL");

    // The editor the launch uses is the one the status line names.
    setenv("EDITOR", "nvim", 1);
    l = term_find_launch("", p);
    CHECK(l.argv.size() == 4);          // program, -e, editor, path
    CHECK(l.argv[2] == "nvim");
    CHECK(l.argv[3] == p);
    unsetenv("EDITOR");

    // Nothing on the machine at all: the message names what was looked for and says
    // how to name one's own, which is the difference between a dead key and a fixable
    // one.
    use_path("/nonexistent-dir-xyz");
    l = term_find_launch("", p);
    CHECK(l.argv.empty());
    CHECK(l.note.find("Looked for:") != std::string::npos);
    CHECK(l.note.find("alacritty") != std::string::npos);
    CHECK(l.note.find("terminal =") != std::string::npos);
    CHECK(l.note.find("$TERMINAL") != std::string::npos);

    // The candidate list is what the message quotes, and it is the list the lookup
    // walks - including the name this machine actually had.
    const std::vector<std::string>& c = term_candidates();
    CHECK(c.size() >= 10);
    CHECK(c[0] == "x-terminal-emulator");
    bool has_alacritty = false, has_xterm = false;
    for (const std::string& n : c) { if (n == "alacritty") has_alacritty = true; if (n == "xterm") has_xterm = true; }
    CHECK(has_alacritty);
    CHECK(has_xterm);
}

int main() {
    char tmpl[] = "/tmp/mc-term-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("cannot create temp dir\n"); return 1; }
    tmp_root = dir;

    test_editor_name();
    test_shapes();
    test_which();
    test_lookup();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
