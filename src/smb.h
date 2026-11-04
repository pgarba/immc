#ifndef SMB_H
#define SMB_H

// SMB shares as remote sites, over libsmbclient (Samba's client library). This is
// the backend remote.cpp hands a NET_SMB site to; nothing else calls it. It speaks
// SMB2/3 itself, so a share opens without GVfs, without a mount and without root,
// and the login is passed to the library in memory - never on a command line.
//
// Paths follow the rest of the remote layer: "" or "/" is the server, whose
// "directory" is its list of shares, and "/share/dir/file" is a file on a share.
//
// Optional at build time, like libcurl: without libsmbclient (and always on
// Windows, where \\server\share is a path the system opens) smb_available() is
// false and every call fails with a reason.

#include <string>
#include <vector>
#include "remote.h"

bool smb_available();
void smb_what(char* out, size_t n);
// Drop the connections for a site root (null = every site) on the calling thread.
void smb_forget(const char* root);
void smb_shutdown();

bool smb_list(const NetSite& site, const std::string& rpath, std::vector<FileEntry>* out,
              std::string* err, fs_progress_fn cb, void* user);
bool smb_stat(const NetSite& site, const std::string& rpath, FileEntry* out, std::string* err);
bool smb_download(const NetSite& site, const std::string& rpath, const char* local,
                  fs_progress_fn cb, void* user, std::string* err,
                  long long resume_from, long long* got);
bool smb_download_prefix(const NetSite& site, const std::string& rpath, std::string* out,
                         size_t max_bytes, fs_progress_fn cb, void* user, std::string* err);
bool smb_upload(const NetSite& site, const std::string& rpath, const char* local,
                fs_progress_fn cb, void* user, std::string* err, long long resume_from);
bool smb_mkdir(const NetSite& site, const std::string& rpath, std::string* err);
bool smb_remove(const NetSite& site, const std::string& rpath, bool is_dir, std::string* err);
bool smb_rename(const NetSite& site, const std::string& rpath, const char* new_name,
                std::string* err);
bool smb_chmod(const NetSite& site, const std::string& rpath, int mode, std::string* err);

#endif // SMB_H
