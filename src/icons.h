#ifndef MC_ICONS_H
#define MC_ICONS_H

// The small icon in front of each name in a pane, the way Total Commander and
// Explorer show one.
//
// On Windows it is the shell's own icon, the one Explorer shows: by extension for
// most files, so a listing of a thousand .txt files asks the shell once, and by the
// file itself for the kinds that carry their own (.exe, .lnk, .ico ...). Elsewhere,
// and whenever the shell has no answer, the pane draws a small icon of its own for
// the kind icon_kind() names - which is also all a test without a display can see.

enum IconKind {
    ICON_FILE,
    ICON_UP,        // ".."
    ICON_DIR,
    ICON_EXEC,
    ICON_ARCHIVE,
    ICON_IMAGE,
};

// What a row is, for the drawn icon. Only the name is looked at, never the disk.
IconKind icon_kind(const char* name, bool is_dir, bool is_exec);

// The shell's small icon for `name` in `dir`, as a texture (see texture.h), or 0
// when there is none: not Windows, no GL context, or the shell does not know.
// `local` says `dir` is a folder on this machine that can be asked about its files;
// a remote listing, or an archive's, gets the icon of the extension alone. Cached:
// the first frame a kind is seen asks the shell, later ones do not.
unsigned long long icon_shell(const char* dir, const char* name, bool is_dir, bool local);

// The same at the shell's large size (32 px at 100%), for the launch bar, where a
// small icon blown up would be a blur. Cached apart from the small ones.
unsigned long long icon_shell_large(const char* dir, const char* name, bool is_dir);

// Call once per frame: a file's own icon costs a disk read, and only a few are
// fetched each frame so a folder of a thousand programs does not freeze the window.
// Until its turn the file shows its extension's icon.
void icon_frame(void);

// Drop every cached icon (before the GL context goes).
void icon_free_all(void);

#endif
