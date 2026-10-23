#include "icons.h"

#include "archive.h"
#include "texture.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <string>

static const char* icon_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    return (dot && dot != name) ? dot + 1 : "";
}

static bool ext_is(const char* ext, const char* const* list) {
    for (; *list; list++) if (!strcasecmp(ext, *list)) return true;
    return false;
}

static const char* const k_image_ext[] = {
    "png", "jpg", "jpeg", "gif", "bmp", "webp", "tga", "tif", "tiff", "ico", "svg", "psd", nullptr,
};

IconKind icon_kind(const char* name, bool is_dir, bool is_exec) {
    if (!strcmp(name, "..")) return ICON_UP;
    if (is_dir) return ICON_DIR;
    if (arch_is_archive(name)) return ICON_ARCHIVE;
    if (ext_is(icon_ext(name), k_image_ext)) return ICON_IMAGE;
    if (is_exec) return ICON_EXEC;
    return ICON_FILE;
}

#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <map>
#include <vector>

// The kinds whose icon is in the file, not in the registry entry for its extension.
static const char* const k_own_icon_ext[] = {
    "exe", "lnk", "ico", "cur", "url", "scr", "msc", "cpl", "appref-ms", nullptr,
};

// Shell icons are deduplicated by their index in the system image list: every .txt
// is the same picture, and so are most folders. `by_key` maps what was asked for to
// that index, `by_index` the index to its texture.
static std::map<std::string, int> g_by_key;
static std::map<int, unsigned long long> g_by_index;
static int g_budget = 0;

void icon_frame(void) { g_budget = 6; }

static std::wstring icon_widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// An HICON's pixels as RGBA. Icons with an alpha channel carry it in the color
// bitmap; older ones leave it zero and say what is transparent in the mask.
static unsigned long long hicon_texture(HICON h) {
    ICONINFO ii;
    if (!GetIconInfo(h, &ii)) return 0;
    unsigned long long tex = 0;
    BITMAP bm;
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        int w = bm.bmWidth, hgt = bm.bmHeight;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -hgt;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<unsigned char> px((size_t)w * hgt * 4), mask((size_t)w * hgt * 4);
        HDC dc = GetDC(nullptr);
        bool ok = GetDIBits(dc, ii.hbmColor, 0, hgt, px.data(), &bi, DIB_RGB_COLORS) == hgt;
        bool alpha = false;
        for (size_t i = 3; ok && i < px.size(); i += 4) if (px[i]) { alpha = true; break; }
        if (ok && !alpha && ii.hbmMask)
            ok = GetDIBits(dc, ii.hbmMask, 0, hgt, mask.data(), &bi, DIB_RGB_COLORS) == hgt;
        ReleaseDC(nullptr, dc);
        if (ok) {
            for (size_t i = 0; i < px.size(); i += 4) {
                unsigned char b = px[i];
                px[i] = px[i + 2];
                px[i + 2] = b;
                if (!alpha) px[i + 3] = mask[i] ? 0 : 255;
            }
            tex = tex_upload(w, hgt, px.data());
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return tex;
}

unsigned long long icon_shell(const char* dir, const char* name, bool is_dir, bool local) {
    if (!strcmp(name, "..")) return 0;   // the pane draws its own "up"
    if (!tex_available()) return 0;

    static bool com = false;
    if (!com) {
        // The shell asks COM for some icons. A failure because SDL got there first
        // with another threading model is fine: COM is up either way.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        com = true;
    }

    const char* ext = icon_ext(name);
    // A network share answers slowly, so its files get their extension's icon.
    bool own = !is_dir && local && ext_is(ext, k_own_icon_ext) &&
               !(dir[0] && strchr("/\\", dir[0]) && strchr("/\\", dir[1]));
    std::string key;
    if (is_dir) {
        key = "<dir>";
    } else if (own) {
        key = std::string(dir) + "/" + name;
    } else {
        key = ".";
        for (const char* p = ext; *p; p++) key += (char)tolower((unsigned char)*p);
    }

    std::map<std::string, int>::iterator it = g_by_key.find(key);
    if (it == g_by_key.end()) {
        if (own && g_budget <= 0) {
            // Not this frame: show the extension's icon until its turn comes.
            return icon_shell(dir, name, is_dir, false);
        }
        SHFILEINFOW fi = {};
        UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
        std::wstring path;
        DWORD attr = FILE_ATTRIBUTE_NORMAL;
        if (own) {
            g_budget--;
            path = icon_widen(key);
            for (wchar_t& c : path) if (c == L'/') c = L'\\';
            if (!strcasecmp(ext, "lnk")) flags |= SHGFI_LINKOVERLAY;
        } else {
            // Ask about a made-up name: the shell answers from the extension alone
            // and never touches the disk.
            flags |= SHGFI_USEFILEATTRIBUTES;
            if (is_dir) { path = L"folder"; attr = FILE_ATTRIBUTE_DIRECTORY; }
            else path = L"file" + icon_widen(key == "." ? "" : key);
        }
        int index = -1;
        if (SHGetFileInfoW(path.c_str(), attr, &fi, sizeof(fi), flags) && fi.hIcon) {
            // A shortcut's arrow is drawn on top of its target's icon, so it is not
            // the same picture as that index.
            index = fi.iIcon | ((flags & SHGFI_LINKOVERLAY) ? 0x40000000 : 0);
            if (!g_by_index.count(index)) g_by_index[index] = hicon_texture(fi.hIcon);
            DestroyIcon(fi.hIcon);
        }
        it = g_by_key.insert(std::make_pair(key, index)).first;
    }
    if (it->second < 0) return own ? icon_shell(dir, name, is_dir, false) : 0;
    unsigned long long tex = g_by_index[it->second];
    if (!tex && own) return icon_shell(dir, name, is_dir, false);
    return tex;
}

void icon_free_all(void) {
    for (std::map<int, unsigned long long>::iterator it = g_by_index.begin(); it != g_by_index.end(); ++it)
        tex_free(it->second);
    g_by_index.clear();
    g_by_key.clear();
}

#else

unsigned long long icon_shell(const char*, const char*, bool, bool) { return 0; }
void icon_frame(void) {}
void icon_free_all(void) {}

#endif
