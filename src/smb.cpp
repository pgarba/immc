// SMB over libsmbclient. See smb.h for where it sits.
//
// libsmbclient keeps its state in a context (SMBCCTX): the connections it has
// open, and the login it uses. A context must not be used by two threads at once,
// and the browse worker, the UI thread and each background transfer all reach
// sites, so the contexts are cached per thread, one per site root, exactly as the
// curl handles are. A site's login lives in its context's user data, and the
// library asks for it through the auth callback when it connects.

#include "smb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef MC_HAVE_SMBCLIENT

#include <libsmbclient.h>

namespace {

struct SmbLogin {
    std::string user, domain, password;
};

struct SmbCtx {
    std::string root;      // net_site_root(): the key
    SMBCCTX* ctx = nullptr;
    SmbLogin* login = nullptr;
};

void free_ctx(SmbCtx& c) {
    if (c.ctx) {
        smbc_getFunctionPurgeCachedServers(c.ctx)(c.ctx);
        smbc_free_context(c.ctx, 1);
    }
    delete c.login;
    c.ctx = nullptr;
    c.login = nullptr;
}

// Per thread, released when the thread ends (a transfer's worker) or on forget.
struct SmbCache {
    std::vector<SmbCtx> entries;
    ~SmbCache() { for (SmbCtx& c : entries) free_ctx(c); }
};

SmbCache& cache() {
    static thread_local SmbCache c;
    return c;
}

// The library's own diagnostics go to the connection log, beside curl's.
void log_cb(void*, int, const char* msg) {
    if (!msg) return;
    std::string s = msg;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) net_log_add("smb", NETLOG_INFO, s);
}

void auth_cb(SMBCCTX* c, const char*, const char*, char* wg, int wglen, char* un, int unlen,
             char* pw, int pwlen) {
    const SmbLogin* l = (const SmbLogin*)smbc_getOptionUserData(c);
    if (!l) return;
    // An empty user is a guest login; the workgroup buffer arrives holding the
    // smb.conf default, which is kept when the site names no domain.
    snprintf(un, (size_t)unlen, "%s", l->user.c_str());
    snprintf(pw, (size_t)pwlen, "%s", l->password.c_str());
    if (!l->domain.empty()) snprintf(wg, (size_t)wglen, "%s", l->domain.c_str());
}

// The context for a site on this thread, created on first use. A changed login
// (the user typed a new password) drops the connections made with the old one.
SMBCCTX* ctx_for(const NetSite& site, std::string* err) {
    std::string root = net_site_root(site);
    SmbCache& c = cache();
    for (SmbCtx& e : c.entries) {
        if (e.root != root) continue;
        if (e.login->password != site.password || e.login->domain != site.domain) {
            smbc_getFunctionPurgeCachedServers(e.ctx)(e.ctx);
            e.login->password = site.password;
            e.login->domain = site.domain;
        }
        return e.ctx;
    }
    SMBCCTX* ctx = smbc_new_context();
    if (!ctx) { *err = "libsmbclient: cannot create a context"; return nullptr; }
    SmbLogin* login = new SmbLogin{ site.user, site.domain, site.password };
    smbc_setDebug(ctx, 0);
    smbc_setLogCallback(ctx, nullptr, log_cb);
    smbc_setOptionUserData(ctx, login);
    smbc_setFunctionAuthDataWithContext(ctx, auth_cb);
    // A server that does not answer must not hold a pane for minutes.
    smbc_setTimeout(ctx, 15000);
    // A login that is refused must say so, not quietly come back as a guest who
    // sees less: only a site with no user at all asks for the guest account.
    smbc_setOptionNoAutoAnonymousLogin(ctx, site.user.empty() ? 0 : 1);
    // Use a Kerberos ticket when there is one (a machine in a domain), and the
    // password when there is not.
    smbc_setOptionUseKerberos(ctx, 1);
    smbc_setOptionFallbackAfterKerberos(ctx, 1);
    smbc_setOptionUseCCache(ctx, 1);
    if (!smbc_init_context(ctx)) {
        int e = errno;
        smbc_free_context(ctx, 0);
        delete login;
        *err = std::string("libsmbclient: cannot start: ") + strerror(e);
        return nullptr;
    }
    c.entries.push_back({ root, ctx, login });
    net_log_add(root, NETLOG_INFO, "SMB session for " + root +
                (site.user.empty() ? std::string(" (guest)") : std::string()));
    return ctx;
}

// rpath ("/share/dir/name") to the URL libsmbclient opens. The login is not in it:
// the auth callback hands that over. libsmbclient decodes the path, so a '%' or a
// '#' in a name is escaped like any other URL.
std::string smb_url(const NetSite& site, const std::string& rpath) {
    std::string u = "smb://";
    if (site.host.find(':') != std::string::npos) u += "[" + site.host + "]";
    else u += site.host;
    if (site.port > 0 && site.port != net_proto_port(NET_SMB)) u += ":" + std::to_string(site.port);
    std::string p = rpath;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    u += "/" + net_url_escape(p, true);
    return u;
}

// What an errno from libsmbclient means to someone looking at a share. The login
// ones carry kNetLoginRefused, which is what makes the pane ask for a login.
std::string why(int e, bool login_matters) {
    switch (e) {
        case EACCES:
        case EPERM:
            if (login_matters) return std::string(kNetLoginRefused) + ": access denied";
            return "access denied";
        case ENOENT:       return "no such share, folder or file";
        case ENODEV:       return "no such share";
        case ETIMEDOUT:    return "the server did not answer";
        case ECONNREFUSED: return "the server refused the connection (is file sharing on?)";
        case EHOSTUNREACH:
        case ENETUNREACH:  return "the server cannot be reached";
        case EEXIST:       return "it already exists";
        case ENOTEMPTY:    return "the folder is not empty";
        case ENOTDIR:      return "not a folder";
        case EISDIR:       return "is a folder";
        case ENOSPC:       return "the share is full";
        case EBUSY:        return "in use by another program on the server";
        default:           return strerror(e);
    }
}

bool fail(const NetSite& site, const std::string& what, int e, bool login_matters, std::string* err) {
    std::string msg = why(e, login_matters);
    net_log_add(net_site_root(site), NETLOG_ERROR, what + ": " + msg);
    if (err) *err = msg;
    return false;
}

void fill_entry(FileEntry* fe, const char* name, const struct stat& st) {
    memset(fe, 0, sizeof(*fe));
    snprintf(fe->name, sizeof(fe->name), "%s", name);
    fe->mode = st.st_mode;
    fe->size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    fe->mtime = st.st_mtime;
}

bool is_server(const std::string& rpath) {
    for (char c : rpath) if (c != '/') return false;
    return true;
}

} // namespace

bool smb_available() { return true; }

void smb_what(char* out, size_t n) { snprintf(out, n, "libsmbclient %s", smbc_version()); }

void smb_forget(const char* root) {
    SmbCache& c = cache();
    for (size_t i = 0; i < c.entries.size();) {
        if (!root || c.entries[i].root == root) {
            free_ctx(c.entries[i]);
            c.entries.erase(c.entries.begin() + (long)i);
        } else {
            i++;
        }
    }
}

void smb_shutdown() { smb_forget(nullptr); }

bool smb_list(const NetSite& site, const std::string& rpath, std::vector<FileEntry>* out,
              std::string* err, fs_progress_fn cb, void* user) {
    out->clear();
    if (!site.valid()) { if (err) *err = "no host"; return false; }
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    net_log_add(net_site_root(site), NETLOG_SENT, "list " + url);
    SMBCFILE* d = smbc_getFunctionOpendir(ctx)(ctx, url.c_str());
    if (!d) return fail(site, url, errno, true, err);

    if (is_server(rpath)) {
        // The server itself: its shares are the folders. Printers, IPC$ and the
        // administrative C$ / ADMIN$ are left out, as Explorer leaves them out.
        smbc_readdir_fn rd = smbc_getFunctionReaddir(ctx);
        while (struct smbc_dirent* de = rd(ctx, d)) {
            if (de->smbc_type != SMBC_FILE_SHARE) continue;
            size_t len = strlen(de->name);
            if (!len || de->name[len - 1] == '$') continue;
            FileEntry fe;
            memset(&fe, 0, sizeof fe);
            snprintf(fe.name, sizeof fe.name, "%s", de->name);
            fe.mode = S_IFDIR | 0755;
            out->push_back(fe);
        }
    } else {
        smbc_readdirplus2_fn rd = smbc_getFunctionReaddirPlus2(ctx);
        struct stat st;
        int n = 0;
        while (const struct libsmb_file_info* fi = rd(ctx, d, &st)) {
            if (!fi->name || !strcmp(fi->name, ".") || !strcmp(fi->name, "..")) continue;
            FileEntry fe;
            fill_entry(&fe, fi->name, st);
            out->push_back(fe);
            // A pane that moved on stops a long listing here.
            if (cb && (++n % 256) == 0 && !cb(0, 0, n, 0, user)) {
                smbc_getFunctionClosedir(ctx)(ctx, d);
                out->clear();
                if (err) *err = "aborted";
                return false;
            }
        }
    }
    smbc_getFunctionClosedir(ctx)(ctx, d);
    net_log_add(net_site_root(site), NETLOG_RECV, std::to_string(out->size()) + " entries");
    return true;
}

bool smb_stat(const NetSite& site, const std::string& rpath, FileEntry* out, std::string* err) {
    memset(out, 0, sizeof(*out));
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    struct stat st;
    if (smbc_getFunctionStat(ctx)(ctx, url.c_str(), &st) != 0) {
        int e = errno;
        if (err) *err = why(e, false);
        return false;
    }
    std::string base = rpath;
    while (!base.empty() && base.back() == '/') base.pop_back();
    size_t slash = base.find_last_of('/');
    fill_entry(out, slash == std::string::npos ? base.c_str() : base.c_str() + slash + 1, st);
    return true;
}

static const size_t kChunk = 1 << 20;   // SMB2 reads and writes up to 1 MB at once

bool smb_download(const NetSite& site, const std::string& rpath, const char* local,
                  fs_progress_fn cb, void* user, std::string* err,
                  long long resume_from, long long* got) {
    if (got) *got = 0;
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    SMBCFILE* fh = smbc_getFunctionOpen(ctx)(ctx, url.c_str(), O_RDONLY, 0);
    if (!fh) return fail(site, url, errno, false, err);
    long long total = -1;
    struct stat st;
    if (smbc_getFunctionFstat(ctx)(ctx, fh, &st) == 0) total = (long long)st.st_size;
    if (resume_from > 0 && total >= 0 && resume_from > total) resume_from = 0;

    // The same sidecar contract as the FTP download: <local>.part, renamed into
    // place only once the whole file is there.
    std::string tmp = net_part_path(local);
    if (resume_from <= 0) remove(tmp.c_str());
    FILE* f = fopen(tmp.c_str(), resume_from > 0 ? "ab" : "wb");
    if (!f) {
        smbc_getFunctionClose(ctx)(ctx, fh);
        if (err) *err = std::string("cannot write ") + tmp + ": " + strerror(errno);
        return false;
    }
    if (resume_from > 0 && smbc_getFunctionLseek(ctx)(ctx, fh, (off_t)resume_from, SEEK_SET) < 0) {
        int e = errno;
        fclose(f);
        smbc_getFunctionClose(ctx)(ctx, fh);
        return fail(site, url, e, false, err);
    }

    std::vector<char> buf(kChunk);
    long long done = 0, want = total >= 0 ? total - (resume_from > 0 ? resume_from : 0) : -1;
    bool ok = true, aborted = false;
    int read_errno = 0;
    smbc_read_fn rd = smbc_getFunctionRead(ctx);
    for (;;) {
        ssize_t n = rd(ctx, fh, buf.data(), buf.size());
        if (n < 0) { read_errno = errno; ok = false; break; }
        if (n == 0) break;
        if (fwrite(buf.data(), 1, (size_t)n, f) != (size_t)n) {
            if (err) *err = std::string("cannot write ") + tmp + ": " + strerror(errno);
            ok = false;
            break;
        }
        done += n;
        if (cb && !cb(done, want, 1, 1, user)) { aborted = true; ok = false; break; }
    }
    smbc_getFunctionClose(ctx)(ctx, fh);
    if (fclose(f) != 0 && ok) {
        if (err) *err = std::string("cannot write ") + tmp + ": " + strerror(errno);
        ok = false;
    }
    if (got) *got = done;
    if (!ok) {
        struct stat ps;
        if (stat(tmp.c_str(), &ps) == 0 && ps.st_size == 0) remove(tmp.c_str());
        if (aborted) { if (err) *err = "aborted"; }
        else if (read_errno) fail(site, url, read_errno, false, err);
        return false;
    }
    if (rename(tmp.c_str(), local) != 0) {
        if (err) *err = std::string("cannot put ") + local + " in place: " + strerror(errno);
        return false;
    }
    return true;
}

bool smb_download_prefix(const NetSite& site, const std::string& rpath, std::string* out,
                         size_t max_bytes, fs_progress_fn cb, void* user, std::string* err) {
    out->clear();
    if (!max_bytes) return true;
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    SMBCFILE* fh = smbc_getFunctionOpen(ctx)(ctx, url.c_str(), O_RDONLY, 0);
    if (!fh) return fail(site, url, errno, false, err);
    smbc_read_fn rd = smbc_getFunctionRead(ctx);
    std::vector<char> buf(kChunk);
    bool ok = true;
    while (out->size() < max_bytes) {
        size_t want = max_bytes - out->size();
        if (want > buf.size()) want = buf.size();
        ssize_t n = rd(ctx, fh, buf.data(), want);
        if (n < 0) { int e = errno; ok = fail(site, url, e, false, err); break; }
        if (n == 0) break;
        out->append(buf.data(), (size_t)n);
        if (cb && !cb((long long)out->size(), (long long)max_bytes, 1, 1, user)) {
            if (err) *err = "aborted";
            ok = false;
            break;
        }
    }
    smbc_getFunctionClose(ctx)(ctx, fh);
    if (!ok) out->clear();
    return ok;
}

// Every folder above `rpath` that the share does not have yet. A copy of a file
// into a folder that is not there is an upload of a tree, not a mistake - the
// FTP store creates missing folders too.
static void make_parents(SMBCCTX* ctx, const NetSite& site, const std::string& rpath) {
    std::string p = rpath;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    size_t share_end = p.find('/');
    if (share_end == std::string::npos) return;
    for (size_t at = p.find('/', share_end + 1); at != std::string::npos; at = p.find('/', at + 1)) {
        std::string url = smb_url(site, p.substr(0, at));
        smbc_getFunctionMkdir(ctx)(ctx, url.c_str(), 0755);   // EEXIST is fine
    }
}

bool smb_upload(const NetSite& site, const std::string& rpath, const char* local,
                fs_progress_fn cb, void* user, std::string* err, long long resume_from) {
    FILE* f = fopen(local, "rb");
    if (!f) { if (err) *err = std::string("cannot read ") + local + ": " + strerror(errno); return false; }
    struct stat lst;
    long long size = -1;
    if (fstat(fileno(f), &lst) == 0 && S_ISREG(lst.st_mode)) size = (long long)lst.st_size;
    if (resume_from > 0 && size >= 0 && resume_from > size) resume_from = 0;

    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) { fclose(f); return false; }
    std::string url = smb_url(site, rpath);
    int flags = O_WRONLY | O_CREAT | (resume_from > 0 ? 0 : O_TRUNC);
    SMBCFILE* fh = smbc_getFunctionOpen(ctx)(ctx, url.c_str(), flags, 0644);
    if (!fh && errno == ENOENT) {
        make_parents(ctx, site, rpath);
        fh = smbc_getFunctionOpen(ctx)(ctx, url.c_str(), flags, 0644);
    }
    if (!fh) { int e = errno; fclose(f); return fail(site, url, e, false, err); }
    if (resume_from > 0) {
        // Continue where the file on the share ends, which is the offset asked for:
        // anything past it is cut off first, so the result is exactly the source.
        smbc_getFunctionFtruncate(ctx)(ctx, fh, (off_t)resume_from);
        smbc_getFunctionLseek(ctx)(ctx, fh, (off_t)resume_from, SEEK_SET);
        fseeko(f, (off_t)resume_from, SEEK_SET);
    }

    std::vector<char> buf(kChunk);
    long long done = 0, want = size >= 0 ? size - (resume_from > 0 ? resume_from : 0) : -1;
    bool ok = true;
    smbc_write_fn wr = smbc_getFunctionWrite(ctx);
    for (;;) {
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            if (ferror(f)) { if (err) *err = std::string("cannot read ") + local; ok = false; }
            break;
        }
        size_t at = 0;
        while (at < n) {
            ssize_t w = wr(ctx, fh, buf.data() + at, n - at);
            if (w <= 0) { int e = errno; ok = fail(site, url, e ? e : EIO, false, err); break; }
            at += (size_t)w;
        }
        if (!ok) break;
        done += (long long)n;
        if (cb && !cb(done, want, 1, 1, user)) { if (err) *err = "aborted"; ok = false; break; }
    }
    fclose(f);
    if (smbc_getFunctionClose(ctx)(ctx, fh) != 0 && ok) ok = fail(site, url, errno, false, err);
    return ok;
}

bool smb_mkdir(const NetSite& site, const std::string& rpath, std::string* err) {
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    if (smbc_getFunctionMkdir(ctx)(ctx, url.c_str(), 0755) != 0) return fail(site, url, errno, false, err);
    return true;
}

bool smb_remove(const NetSite& site, const std::string& rpath, bool is_dir, std::string* err) {
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    int rc = is_dir ? smbc_getFunctionRmdir(ctx)(ctx, url.c_str())
                    : smbc_getFunctionUnlink(ctx)(ctx, url.c_str());
    if (rc != 0) return fail(site, url, errno, false, err);
    return true;
}

bool smb_rename(const NetSite& site, const std::string& rpath, const char* new_name,
                std::string* err) {
    if (!new_name || !*new_name) { if (err) *err = "nothing to rename"; return false; }
    if (strchr(new_name, '/') || strchr(new_name, '\\')) { if (err) *err = "a new name cannot contain / or \\"; return false; }
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string from = rpath;
    while (!from.empty() && from.back() == '/') from.pop_back();
    size_t slash = from.find_last_of('/');
    std::string parent = slash == std::string::npos ? std::string() : from.substr(0, slash);
    std::string ourl = smb_url(site, from), nurl = smb_url(site, parent + "/" + new_name);
    if (smbc_getFunctionRename(ctx)(ctx, ourl.c_str(), ctx, nurl.c_str()) != 0)
        return fail(site, ourl, errno, false, err);
    return true;
}

bool smb_chmod(const NetSite& site, const std::string& rpath, int mode, std::string* err) {
    // SMB has no Unix permission bits. libsmbclient maps a mode with no write bit
    // to the DOS read-only attribute, which is the one thing that can be set.
    SMBCCTX* ctx = ctx_for(site, err);
    if (!ctx) return false;
    std::string url = smb_url(site, rpath);
    if (smbc_getFunctionChmod(ctx)(ctx, url.c_str(), (mode_t)mode) != 0)
        return fail(site, url, errno, false, err);
    return true;
}

#else  // ---------------------------------------------------------------------

static bool no_smb(std::string* err) {
#ifdef _WIN32
    if (err) *err = "on Windows a share is opened as \\\\server\\share";
#else
    if (err) *err = "this build has no SMB support: install libsmbclient and rebuild";
#endif
    return false;
}

bool smb_available() { return false; }
void smb_what(char* out, size_t n) {
#ifdef _WIN32
    snprintf(out, n, "SMB through Windows");
#else
    snprintf(out, n, "no SMB support in this build");
#endif
}
void smb_forget(const char*) {}
void smb_shutdown() {}
bool smb_list(const NetSite&, const std::string&, std::vector<FileEntry>* out, std::string* err,
              fs_progress_fn, void*) { out->clear(); return no_smb(err); }
bool smb_stat(const NetSite&, const std::string&, FileEntry*, std::string* err) { return no_smb(err); }
bool smb_download(const NetSite&, const std::string&, const char*, fs_progress_fn, void*,
                  std::string* err, long long, long long* got) { if (got) *got = 0; return no_smb(err); }
bool smb_download_prefix(const NetSite&, const std::string&, std::string* out, size_t,
                         fs_progress_fn, void*, std::string* err) { out->clear(); return no_smb(err); }
bool smb_upload(const NetSite&, const std::string&, const char*, fs_progress_fn, void*,
                std::string* err, long long) { return no_smb(err); }
bool smb_mkdir(const NetSite&, const std::string&, std::string* err) { return no_smb(err); }
bool smb_remove(const NetSite&, const std::string&, bool, std::string* err) { return no_smb(err); }
bool smb_rename(const NetSite&, const std::string&, const char*, std::string* err) { return no_smb(err); }
bool smb_chmod(const NetSite&, const std::string&, int, std::string* err) { return no_smb(err); }

#endif // MC_HAVE_SMBCLIENT
