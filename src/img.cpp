// The picture half of quick view: recognise a format, ask it how big it claims to
// be, decode it, turn it the way the camera meant it, and shrink it to something a
// window can draw.
//
// Nothing in here knows about SDL, ImGui, OpenGL or panes. That is deliberate: the
// whole module runs headless in a test, on files the test wrote itself, and the
// half that cannot be tested that way is src/texture.cpp, which is four functions
// wide.

#include "img.h"

#include <cstdio>
#include <cstring>

#ifdef MC_HAVE_STB_IMAGE
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

// A preview is not a photo editor. A 4032x3024 phone picture is 12 million pixels
// and 48 MB of RGBA; a 9000x9000 scan is 81 million and 324 MB, and the only thing
// the window can do with it is shrink it. Both caps are below the point where the
// allocation is the problem rather than the drawing, and both name the numbers so
// the refusal is information rather than a dead end.
static const int k_img_max_px_side = 8192;
static const long long k_img_max_pixels = 40LL * 1000 * 1000;

bool img_decoder_available(void) {
#ifdef MC_HAVE_STB_IMAGE
    return true;
#else
    return false;
#endif
}

bool img_decodable(ImgFormat fmt) {
    switch (fmt) {
        case IMG_PNG: case IMG_JPEG: case IMG_BMP: case IMG_GIF:
        case IMG_PNM: case IMG_TGA: case IMG_HDR:
            return true;
        default:
            return false;
    }
}

const char* img_format_name(ImgFormat fmt) {
    switch (fmt) {
        case IMG_PNG:   return "PNG";
        case IMG_JPEG:  return "JPEG";
        case IMG_BMP:   return "BMP";
        case IMG_GIF:   return "GIF";
        case IMG_PNM:   return "PNM";
        case IMG_TGA:   return "TGA";
        case IMG_HDR:   return "HDR";
        case IMG_WEBP:  return "webp";
        case IMG_TIFF:  return "TIFF";
        case IMG_SVG:   return "SVG";
        case IMG_ICO:   return "ICO";
        default:        return "unknown";
    }
}

// A TGA header is id length 0, colour map type 0 or 1, then an image type. The other
// three bytes are not checked, so a binary that happens to start 00 00 02 00 gets
// sniffed as a picture and then fails to decode, which lands it back on the hex dump.
// Sniffing is a question about what is worth reading, not a promise about what will
// be drawn.
static bool tga_head(const unsigned char* p, size_t n) {
    if (n < 4) return false;
    if (p[0] != 0 || (p[1] != 0 && p[1] != 1)) return false;
    switch (p[2]) {
        case 1: case 2: case 3: case 9: case 10: case 11: return true;
        default: return false;
    }
}

// "<svg" is not at byte zero when the file carries an XML declaration or a comment,
// and it is never far. 512 bytes is generous for a header and small enough that a
// text file that merely mentions svg in its first line is not misread as one.
static bool svg_head(const unsigned char* p, size_t n) {
    size_t limit = n < 512 ? n : 512;
    for (size_t i = 0; i + 3 < limit; i++)
        if (p[i] == '<' && !memcmp(p + i, "<svg", 4)) return true;
    return false;
}

ImgFormat img_sniff(const unsigned char* p, size_t n) {
    if (!p || n < 4) return IMG_NONE;

    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return IMG_PNG;
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return IMG_JPEG;
    // "BM" alone is not enough: a file can start with those two letters and be
    // nothing of the kind. The header size that follows is one of the five Windows
    // and OS/2 forms, and a BMP shorter than its own header is not a BMP.
    if (n >= 26 && p[0] == 'B' && p[1] == 'M') {
        unsigned dib = (unsigned)p[14] | ((unsigned)p[15] << 8) |
                       ((unsigned)p[16] << 16) | ((unsigned)p[17] << 24);
        if (dib == 12 || dib == 40 || dib == 64 || dib == 108 || dib == 124) return IMG_BMP;
    }
    if (n >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) return IMG_GIF;
    if (n >= 3 && p[0] == 'P' && p[1] >= '1' && p[1] <= '7' &&
        (p[2] == '\n' || p[2] == ' ' || p[2] == '\t')) return IMG_PNM;

    // An icon header is 00 00 01 00 followed by a count and a directory, and a
    // colour-mapped TGA starts with the same four bytes. The directory is what tells
    // them apart, so the count and the first entry's size are checked before this
    // is willing to say "icon".
    if (n >= 22 && !memcmp(p, "\x00\x00\x01\x00", 4)) {
        unsigned count = (unsigned)p[4] | ((unsigned)p[5] << 8);
        if (count >= 1 && count <= 1024 && (p[6] || p[7])) return IMG_ICO;
    }

    if (tga_head(p, n)) return IMG_TGA;
    // Radiance HDR: "#!" or "#?" then a newline inside the first few bytes. The
    // two-letter start is the whole signature - "#!RADIANCE" and "#!NEWENV" are both
    // written by real tools - and the newline is what separates them from a shell
    // comment that happens to start with the same two characters.
    if ((p[0] == '#' && (p[1] == '!' || p[1] == '?'))) {
        for (size_t i = 2; i < n && i < 16; i++)
            if (p[i] == '\n') return IMG_HDR;
    }

    // Recognised, and this build has no decoder for it. Said out loud rather than
    // answered with bytes of hex.
    if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return IMG_WEBP;
    if ((!memcmp(p, "II\x2a\x00", 4)) || (!memcmp(p, "MM\x00\x2a", 4))) return IMG_TIFF;
    if (svg_head(p, n)) return IMG_SVG;

    return IMG_NONE;
}

ImgFormat img_sniff(const std::string& bytes) {
    return img_sniff(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

// ---------------------------------------------------------------------------
// EXIF orientation
//
// stb_image decodes the pixels and discards the metadata, and orientation is the
// one piece of that metadata people notice: a photo taken in portrait on a phone
// is stored landscape with a tag saying which way to turn it. Everything here walks
// bytes from a file of unknown provenance, so every offset is bounds-checked and a
// segment that makes no sense answers "no orientation" rather than anything else.
// ---------------------------------------------------------------------------

static int tiff_u16(const unsigned char* t, size_t n, size_t off, bool le) {
    if (off + 1 >= n) return 0;
    return le ? (t[off] | (t[off + 1] << 8)) : ((t[off] << 8) | t[off + 1]);
}

static unsigned long tiff_u32(const unsigned char* t, size_t n, size_t off, bool le) {
    if (off + 3 >= n) return 0;
    if (le)
        return (unsigned long)t[off] | ((unsigned long)t[off + 1] << 8) |
               ((unsigned long)t[off + 2] << 16) | ((unsigned long)t[off + 3] << 24);
    return ((unsigned long)t[off] << 24) | ((unsigned long)t[off + 1] << 16) |
           ((unsigned long)t[off + 2] << 8) | (unsigned long)t[off + 3];
}

// The TIFF block inside an APP1 segment: byte order, the 42, IFD0, and tag 0x0112.
static int tiff_orientation(const unsigned char* t, size_t n) {
    if (n < 8) return 0;
    bool le;
    if (!memcmp(t, "II", 2)) le = true;
    else if (!memcmp(t, "MM", 2)) le = false;
    else return 0;

    if (tiff_u16(t, n, 2, le) != 42) return 0;
    unsigned long ifd = tiff_u32(t, n, 4, le);
    if (ifd + 2 > n) return 0;

    unsigned long count = (unsigned long)tiff_u16(t, n, (size_t)ifd, le);
    // A directory with more entries than a sane IFD0 has is a corrupted count, and
    // walking it would read the rest of the file as tags.
    if (count > 64) return 0;

    for (unsigned long e = 0; e < count; e++) {
        size_t entry = (size_t)ifd + 2 + (size_t)e * 12;
        if (entry + 12 > n) return 0;
        if (tiff_u16(t, n, entry, le) != 0x0112) continue;
        // Orientation is one SHORT, which TIFF stores in the value field itself.
        int v = tiff_u16(t, n, entry + 8, le);
        return (v >= 1 && v <= 8) ? v : 0;
    }
    return 0;
}

int img_exif_orientation(const std::string& bytes) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t n = bytes.size();
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 0;

    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) { i++; continue; }          // resync: a stray byte is not fatal
        unsigned char marker = p[i + 1];
        if (marker == 0xFF) { i++; continue; }        // fill byte
        if (marker == 0xD8) { i += 2; continue; }     // another SOI
        // Start of scan: everything after this is compressed data, not metadata.
        if (marker == 0xDA || marker == 0xD9) break;
        if (marker >= 0xD0 && marker <= 0xD7) { i += 2; continue; }   // standalone markers

        size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (len < 2 || i + 2 + len > n) break;        // segment runs past the file

        if (marker == 0xE1) {                         // APP1, where Exif lives
            const unsigned char* body = p + i + 4;
            size_t blen = len - 2;
            if (blen > 6 && !memcmp(body, "Exif\x00\x00", 6))
                return tiff_orientation(body + 6, blen - 6);
        }
        i += 2 + len;
    }
    return 0;
}

// EXIF says what to do to the stored pixels to get the picture the camera took.
// Reading it as "display pixel (x,y) comes from stored pixel (sx,sy)" keeps the
// eight cases to one loop, and the swapped sizes fall out of cases 5..8.
void img_apply_orientation(int* w, int* h, std::vector<unsigned char>* rgba, int orientation) {
    if (orientation <= 1 || orientation > 8) return;
    const int W = *w, H = *h;
    if (W <= 0 || H <= 0 || rgba->size() < (size_t)W * H * 4) return;

    std::vector<unsigned char> src;
    src.swap(*rgba);

    const bool swap = (orientation >= 5);
    const int NW = swap ? H : W;
    const int NH = swap ? W : H;

    rgba->assign((size_t)NW * NH * 4, 0);
    for (int y = 0; y < NH; y++) {
        for (int x = 0; x < NW; x++) {
            int sx, sy;
            switch (orientation) {
                case 2:  sx = W - 1 - x; sy = y;       break;   // flip left-right
                case 3:  sx = W - 1 - x; sy = H - 1 - y; break;  // rotate 180
                case 4:  sx = x;         sy = H - 1 - y; break;  // flip top-bottom
                case 5:  sx = y;         sy = x;         break;  // transpose
                case 6:  sx = y;         sy = H - 1 - x; break;  // rotate 90 clockwise
                case 7:  sx = W - 1 - y; sy = H - 1 - x; break;  // transverse
                case 8:  sx = W - 1 - y; sy = x;         break;  // rotate 90 counter-clockwise
                default: sx = x;         sy = y;         break;
            }
            const unsigned char* s = &src[((size_t)sy * W + (size_t)sx) * 4];
            unsigned char* d = &(*rgba)[((size_t)y * NW + (size_t)x) * 4];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    *w = NW;
    *h = NH;
}

// ---------------------------------------------------------------------------
// Dimensions and decoding
// ---------------------------------------------------------------------------

bool img_dims(const std::string& bytes, int* w, int* h, std::string* err) {
    *w = 0; *h = 0;
#ifdef MC_HAVE_STB_IMAGE
    int comp = 0;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                               (int)bytes.size(), w, h, &comp)) {
        *err = "cannot read the image header";
        *w = 0; *h = 0;
        return false;
    }
    return true;
#else
    (void)bytes;
    *err = "this build has no image decoder: run scripts/get-deps.sh";
    return false;
#endif
}

bool img_decode(const std::string& bytes, ImgFormat fmt, int* w, int* h,
                std::vector<unsigned char>* rgba, int* orientation, std::string* err) {
    *w = 0; *h = 0;
    if (orientation) *orientation = 0;
    rgba->clear();

    if (!img_decodable(fmt)) {
        *err = std::string(img_format_name(fmt)) +
               ": this build cannot decode it (Alt+F3 opens it with the desktop's program)";
        return false;
    }
    if (!img_decoder_available()) {
        *err = "this build has no image decoder: run scripts/get-deps.sh";
        return false;
    }

    int dw = 0, dh = 0;
    if (!img_dims(bytes, &dw, &dh, err)) return false;
    if (dw <= 0 || dh <= 0) { *err = "the image header makes no sense"; return false; }
    // stb_image has its own guard and it fires first on the truly absurd - a
    // 30000x30000 PNG is refused by stb before it will report the size at all. This
    // one is the guard that catches the merely very large, which is the case a real
    // scan or a panorama arrives as.
    if (dw > k_img_max_px_side || dh > k_img_max_px_side ||
        (long long)dw * (long long)dh > k_img_max_pixels) {
        char b[128];
        snprintf(b, sizeof b, "too large to preview (%dx%d)", dw, dh);
        *err = b;
        return false;
    }

#ifdef MC_HAVE_STB_IMAGE
    // The thread-local form of the flip flag: this app can have three quick views
    // on two threads, and a global set by one of them is a decision nobody agreed to.
    // It is also left off: stb returns row 0 as the top row and the texture is
    // uploaded and drawn in that order.
    stbi_set_flip_vertically_on_load_thread(0);

    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                        (int)bytes.size(), &dw, &dh, &comp, STBI_rgb_alpha);
    if (!px) {
        const char* why = stbi_failure_reason();
        *err = why && *why ? why : "cannot decode this file as an image";
        return false;
    }
    rgba->assign(px, px + (size_t)dw * (size_t)dh * 4);
    stbi_image_free(px);
#endif

    // Only JPEG carries orientation here. PNG has an eXIf chunk and some cameras
    // write it; it is rare enough that leaving it out is honest, and saying so is
    // better than half-implementing it.
    int o = (fmt == IMG_JPEG) ? img_exif_orientation(bytes) : 0;
    if (o >= 2) img_apply_orientation(&dw, &dh, rgba, o);
    *w = dw;
    *h = dh;
    if (orientation) *orientation = o;
    return true;
}

int img_fit(int* w, int* h, std::vector<unsigned char>* rgba, int max_px) {
    const int W = *w, H = *h;
    if (W <= 0 || H <= 0 || max_px <= 0) return 1;
    if (W <= max_px && H <= max_px) return 1;

    int f = (W > H) ? (W + max_px - 1) / max_px : (H + max_px - 1) / max_px;
    if (f < 2) f = 2;
    int NW = W / f, NH = H / f;
    if (NW < 1) NW = 1;
    if (NH < 1) NH = 1;

    std::vector<unsigned char> out((size_t)NW * NH * 4, 0);
    for (int y = 0; y < NH; y++) {
        for (int x = 0; x < NW; x++) {
            // The last cell takes whatever is left rather than a full f x f block, so
            // a size that is not a multiple of the factor still uses every pixel.
            int y0 = y * f, y1 = (y == NH - 1) ? H : y0 + f;
            int x0 = x * f, x1 = (x == NW - 1) ? W : x0 + f;
            unsigned int a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const unsigned char* p = &(*rgba)[((size_t)sy * W + (size_t)sx) * 4];
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            }
            if (!n) n = 1;
            unsigned char* d = &out[((size_t)y * NW + (size_t)x) * 4];
            d[0] = (unsigned char)(r / n);
            d[1] = (unsigned char)(g / n);
            d[2] = (unsigned char)(b / n);
            d[3] = (unsigned char)(a / n);
        }
    }
    rgba->swap(out);
    *w = NW; *h = NH;
    return f;
}
