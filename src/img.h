#ifndef MC_IMG_H
#define MC_IMG_H

#include <cstddef>
#include <string>
#include <vector>

// What a run of bytes is, decided from the bytes and not from a name. A file manager
// that asked the extension would need a list of promises this build cannot keep:
// .webp, .tif and .svg are not things it can decode, and a name is also the first
// thing a file gets wrong.
//
// The formats after IMG_TGA are recognised on purpose. Naming what the file is and
// saying this build cannot open it is a usable answer; printing a hex dump and
// leaving the user to work out why is not.
enum ImgFormat {
    IMG_NONE = 0,
    IMG_PNG,
    IMG_JPEG,
    IMG_BMP,
    IMG_GIF,
    IMG_PNM,
    IMG_TGA,
    IMG_HDR,
    IMG_WEBP,
    IMG_TIFF,
    IMG_SVG,
    IMG_ICO
};

// The formats a decoder in this build can actually turn into pixels.
bool img_decodable(ImgFormat fmt);

// What these bytes are. Reads only the head, so it is safe to call on the first
// 64 KB of a file whose rest has not been read yet - which is exactly when quick
// view has to decide whether reading the rest is worth it.
ImgFormat img_sniff(const unsigned char* p, size_t n);
ImgFormat img_sniff(const std::string& bytes);

// The name a person would recognise: "PNG", "JPEG", "webp", "SVG".
const char* img_format_name(ImgFormat fmt);

// A decoder is present in this build. Without extern_dont_commit/stb/stb_image.h
// every entry point here still answers, with a reason, so a clone that never ran
// scripts/get-deps.sh builds and runs and simply has no pictures.
bool img_decoder_available(void);

// The canvas a file declares, without decoding it. A 200 KB PNG can claim
// 30000x30000, and the only way to find out the cheap way is to read the header
// and stop.
bool img_dims(const std::string& bytes, int* w, int* h, std::string* err);

// The orientation tag a JPEG's EXIF carries, 1..8, or 0 when there is none or the
// EXIF is broken. stb_image reads the pixels and throws this away, and a photo
// from a phone is the case that needs it.
int img_exif_orientation(const std::string& bytes);

// Bytes to RGBA8, top row first, already turned the way the camera meant it.
// `orientation` reports what was applied, so the window can say so.
bool img_decode(const std::string& bytes, ImgFormat fmt, int* w, int* h,
                std::vector<unsigned char>* rgba, int* orientation, std::string* err);

// Box-average down by an integer factor so neither side exceeds max_px. Returns the
// factor used, 1 when the picture already fit. A 4032x3024 photo is never drawn at
// 4032x3024, and paying for it in GPU memory to find that out is the wrong order.
int img_fit(int* w, int* h, std::vector<unsigned char>* rgba, int max_px);

// Apply one of the eight EXIF orientations to an RGBA buffer in place, changing
// *w and *h. Exposed because it is the part worth testing on its own.
void img_apply_orientation(int* w, int* h, std::vector<unsigned char>* rgba, int orientation);

#endif
