#ifndef REMOTE_H
#define REMOTE_H

#include <string>
#include <vector>
#include "filesystem.h"   // FileEntry, fs_progress_fn

// Remote sites. Archives shell out to zip, tar and 7z; a remote pane cannot,
// for two reasons. A password passed in argv is readable by any process through
// /proc, and a fresh login plus a fresh TLS handshake for every directory
// listing would make browsing unusable. So this module links libcurl and keeps
// one handle per site alive across navigations.
//
// libcurl is optional at build time the same way FreeType is: without the dev
// package the app still builds, every transport call below returns false with
// "this build has no FTP support", and the site and URL plumbing keeps working,
// because that is what the dialogs and the tests exercise. A build without it
// says so rather than offering a connect that cannot work.

// What kind of connection a site wants. The scheme a user types picks the first
// three; implicit FTPS has no scheme of its own, so it is a per-site mode that
// writes `ftps://` and is told apart by its port and its AUTH command.
enum NetProto {
    NET_FTP = 0,        // ftp://   plain, upgraded with AUTH TLS when offered
    NET_FTPS,           // ftps://  explicit FTPS, required
    NET_FTPS_IMPLICIT,  // ftps://  TLS from the first byte, conventionally on 990
    NET_SFTP,           // sftp://  or ssh://, over libssh2
    NET_PROTO_COUNT
};

// One saved site, or one quick connect. `password` is only written to the sites
// file when `remember` is set, and it is never part of any URL the UI shows.
struct NetSite {
    std::string name;       // what the site list shows; empty for a quick connect
    NetProto proto = NET_FTP;
    std::string host;
    int port = 0;           // 0 = the protocol's default port
    std::string user;
    std::string password;
    bool remember = false;  // only then is `password` saved
    std::string path;       // the directory to open on connect
    bool verify_tls = true; // false = "accept the certificate anyway", shown in the UI
    bool passive = true;    // false = active mode, PORT instead of PASV

    bool valid() const { return !host.empty(); }
};

// Protocol vocabulary. `net_proto_key` is the name written in the sites file and
// `net_proto_of` reads it back, so a hand-edited file says what the dialog shows.
const char* net_proto_name(NetProto p);     // "FTP", "FTPS", "FTPS (implicit)", "SFTP"
const char* net_proto_key(NetProto p);      // "ftp", "ftps", "ftps-implicit", "sftp"
const char* net_proto_scheme(NetProto p);   // "ftp://", "ftps://", "sftp://"
int  net_proto_port(NetProto p);            // 21, 21, 990, 22
bool net_proto_of(const char* name, NetProto* out);
// The port a site actually connects to: its own if set, else the default.
int net_site_port(const NetSite& s);

// The URL that identifies a site: scheme, user, host, port. It never carries a
// password, and it is the string that appears in a pane's virtual path, so a
// pane can be moved between tabs and history without losing where it was.
std::string net_site_root(const NetSite& s);
// A root plus a path on the server, as the pane holds it: "ftp://host/pub/in".
std::string net_virtual_path(const std::string& root, const std::string& rpath);

// Parse what a user types into the quick-connect field: "host", "user@host",
// "user@host:2121/pub", or a full URL with any of the four schemes. A bare host
// means plain FTP. Returns false with `err` filled when there is no host in it.
bool net_parse_url(const char* text, NetSite* out, std::string* err);

// Split a pane's current_dir into the site root and the path on the server.
// False for an ordinary local directory, which is the common case.
bool net_is_remote(const char* path);
bool net_split_virtual(const char* path, std::string* root, std::string* rpath);
// The parent of a path on the server; "" for the server root, which is the
// signal that the pane is about to leave the site. "" and "/" both name the
// root, so ask net_path_is_root() rather than comparing to one of them.
std::string net_path_parent(const std::string& rpath);
bool net_path_is_root(const std::string& rpath);

// Join a server directory and a name into a path, collapsing the root however it
// happens to be spelled. Never concatenate a "/" by hand: "" + "/" + name and
// "/" + "/" + name are both wrong on a server that honours an empty component.
std::string net_join_remote(const std::string& dir, const std::string& name);
// Percent-decode the name of a listing row: MLSD and LIST both escape names
// that would otherwise be ambiguous.
std::string net_url_unescape(const std::string& s);

// Saved sites live in their own file, created 0600, because the main config is
// written with a plain fopen() and lands world-readable. Passwords are stored in
// cleartext when the user asks for it, and the dialog says so in one line.
std::string net_sites_path();
bool net_sites_load(std::vector<NetSite>* out, std::string* err);
bool net_sites_save(const std::vector<NetSite>& sites, std::string* err);
// Find the saved site a pane's root URL belongs to, so a restart can put the
// password back. Null when nothing is saved for that host.
const NetSite* net_sites_find(const std::vector<NetSite>& sites, const std::string& root);

// Transport. net_available() is false in a build without libcurl; net_what()
// is the line About and --version print.
bool net_available();
void net_what(char* out, size_t n);
// Release the cached handles. Called when a site is edited or removed, so an
// old login is not reused after its password changed.
void net_forget(const NetSite* site);   // null = every site
void net_shutdown();

// List one directory on the server. MLSD is tried first because it is
// machine-readable and carries type, size, modify time and unix.mode; a server
// without it gets a LIST, whose unix and DOS dialects are parsed here. Both
// fill FileEntry, so the pane sorts and formats a remote row exactly like a
// local one. Dotfiles are not filtered here: the pane does that, the same way
// it does for archive members.
bool net_list(const NetSite& site, const std::string& rpath,
              std::vector<FileEntry>* out, std::string* err);

// One entry, by listing its parent and matching the basename. A bare "MLST"
// would be the obvious way and is not available: curl ignores CURLOPT_CUSTOMREQUEST
// "MLST" on a file URL, sends SIZE and then RETR instead, and the fact line never
// arrives. So this is one listing of the parent directory, which is also what
// makes it cheap to answer for many names at once - see net_list().
bool net_stat(const NetSite& site, const std::string& rpath,
              FileEntry* out, std::string* err);

// Download to and upload from a local file. `cb` is the same callback the copy
// operations use, so the progress dialog's Abort stops a transfer the way
// killing a child process stops unzip: it returns false to abort.
bool net_download(const NetSite& site, const std::string& rpath, const char* local,
                  fs_progress_fn cb, void* user, std::string* err);
bool net_upload(const NetSite& site, const std::string& rpath, const char* local,
                fs_progress_fn cb, void* user, std::string* err);

// Mutations. These are sent as FTP commands against the entry's parent
// directory, which is what makes them work with a path relative to the server's
// working directory instead of assuming an absolute path the server accepts.
bool net_mkdir(const NetSite& site, const std::string& rpath, std::string* err);
bool net_remove(const NetSite& site, const std::string& rpath, bool is_dir,
                std::string* err);

// Delete an entry and, if it is a directory, everything under it. FTP has no
// recursive delete, so this lists each directory and removes the deepest entries
// first. The callback runs once per entry removed: a directory has no byte
// total, so the count is what a user watches.
bool net_remove_tree(const NetSite& site, const std::string& rpath,
                     fs_progress_fn cb, void* user, std::string* err);

// Fetch a remote entry, file or directory, to a local path. A directory becomes
// one mkdir and one fetch per entry: FTP has no recursive retrieve.
bool net_download_tree(const NetSite& site, const std::string& rpath, const char* local,
                       fs_progress_fn cb, void* user, std::string* err);

// Put a local entry, file or directory, at a remote path. A directory becomes one
// MKD and one store per entry, which is the only way a server gets a folder.
bool net_upload_tree(const NetSite& site, const char* local, const std::string& rpath,
                     fs_progress_fn cb, void* user, std::string* err);
bool net_rename(const NetSite& site, const std::string& rpath, const char* new_name,
                std::string* err);
// SITE CHMOD. Not every server implements it; the server's own reply is what
// ends up in `err`, rather than a generic failure.
bool net_chmod(const NetSite& site, const std::string& rpath, int mode, std::string* err);

// Listing line parsers, exposed for the tests. Each fills name, mode, size and
// mtime from one line of a listing.
bool net_parse_mlsd(const char* line, FileEntry* out);
bool net_parse_list_line(const char* line, FileEntry* out);
// MLSD's `modify` fact: "20230131120000", UTC.
time_t net_parse_mlsd_time(const char* value);
// An rwx string ("drwxr-xr-x") into a mode_t, for the unix LIST dialect.
mode_t net_mode_from_rwx(const char* rwx);

#endif // REMOTE_H
