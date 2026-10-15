#ifndef MC_TEST_IMAGE_FIXTURES_H
#define MC_TEST_IMAGE_FIXTURES_H

// Pictures built here, in the open, by the test that needs them.
//
// The CI image is `cmake g++ curl zip unzip p7zip-full python3`: no ImageMagick, no
// PIL, nothing that can write a PNG. A preview test that needed a real photograph
// would be a test that silently does nothing on the runner, which is worse than no
// test - it would say the picture path was checked when nothing was decoded. So the
// fixtures are bytes, spelled out, and a failure says which byte.
//
// They are real files, not look-alikes: the PNG below carries a genuine zlib stream
// (stored blocks, which need no compressor) and a genuine CRC per chunk, so the
// decoder is exercised rather than fooled.
//
// Pixels follow one pattern, `pattern_pixel()`, so a test can ask what a decoded
// picture should contain and compare it instead of eyeballing a dump.

#include <stdint.h>
#include <fstream>
#include <string>

#include "sfv.h"

// What one pixel of a fixture holds. Deterministic, and different in x and in y, so
// a decode that mixed up rows, channels, or the bottom-up order of a BMP shows up as
// a wrong value rather than as a picture that looks plausible.
inline void pattern_pixel(int x, int y, unsigned char out[3]) {
    out[0] = (unsigned char)(30 + x * 7);
    out[1] = (unsigned char)(60 + y * 11);
    out[2] = 128;
}

inline void fx_be32(std::string* out, uint32_t v) {
    out->push_back((char)((v >> 24) & 0xFF));
    out->push_back((char)((v >> 16) & 0xFF));
    out->push_back((char)((v >> 8) & 0xFF));
    out->push_back((char)(v & 0xFF));
}

// A PNG chunk is length, then type and data, then the CRC of type and data. Getting
// that order wrong produces a file whose first four bytes are a magic number and
// nothing else, which is a fixture that fails for a reason a reader cannot see.
inline void fx_chunk(std::string* out, const char* type, const std::string& data) {
    std::string body = type;
    body += data;
    fx_be32(out, (uint32_t)data.size());
    out->append(body);
    fx_be32(out, sfv_crc32((const unsigned char*)body.data(), body.size()));
}

inline void fx_push(std::string* out, unsigned char v) { out->push_back((char)v); }

// An 8-bit truecolour PNG, one stored deflate block per row, filter byte 0.
inline std::string fx_png(int w, int h) {
    std::string out;
    out.append("\x89PNG\r\n\x1a\n", 8);

    std::string ihdr;
    fx_be32(&ihdr, (uint32_t)w);
    fx_be32(&ihdr, (uint32_t)h);
    ihdr.push_back(8);    // bit depth
    ihdr.push_back(2);    // colour type: truecolour
    ihdr.push_back(0);    // compression, filter, interlace
    ihdr.push_back(0);
    ihdr.push_back(0);
    fx_chunk(&out, "IHDR", ihdr);

    std::string raw;
    for (int y = 0; y < h; y++) {
        raw.push_back('\0');   // filter: none
        for (int x = 0; x < w; x++) {
            unsigned char p[3];
            pattern_pixel(x, y, p);
            raw.append((const char*)p, 3);
        }
    }
    std::string zraw;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t take = raw.size() - pos;
        if (take > 600) take = 600;
        bool last = (pos + take == raw.size());
        zraw.push_back((char)(last ? 1 : 0));
        zraw.push_back((char)(take & 0xFF));
        zraw.push_back((char)((take >> 8) & 0xFF));
        zraw.push_back((char)((~take) & 0xFF));
        zraw.push_back((char)((~take >> 8) & 0xFF));
        zraw.append(raw, pos, take);
        pos += take;
    }
    zraw.insert(zraw.begin(), '\x78');   // CMF: deflate, 32 KiB window
    zraw.insert(zraw.begin() + 1, '\x01');
    // ADLER32, both halves: s1 is the running sum and s2 the sum of those sums.
    // Writing only s1 makes a zlib stream that decodes as corrupt, which is a
    // confusing way to build a fixture that fails for the wrong reason.
    uint32_t s1 = 1, s2 = 0;
    for (unsigned char c : raw) { s1 = (s1 + c) % 65521; s2 = (s2 + s1) % 65521; }
    fx_be32(&zraw, (s2 << 16) | s1);
    fx_chunk(&out, "IDAT", zraw);
    fx_chunk(&out, "IEND", "");
    return out;
}

// A 24-bit BMP, bottom-up, which is the order stb reads it in and the order a test
// should not assume.
inline std::string fx_bmp(int w, int h) {
    const int stride = (w * 3 + 3) & ~3;
    std::string out;
    out.append("BM", 2);
    auto le32 = [&](uint32_t v) {
        out.push_back((char)(v & 0xFF));
        out.push_back((char)((v >> 8) & 0xFF));
        out.push_back((char)((v >> 16) & 0xFF));
        out.push_back((char)((v >> 24) & 0xFF));
    };
    auto le16 = [&](uint16_t v) {
        out.push_back((char)(v & 0xFF));
        out.push_back((char)((v >> 8) & 0xFF));
    };
    le32(54 + (uint32_t)stride * h);
    le32(0);
    le32(54);
    le32(40);
    le32((uint32_t)w);
    le32((uint32_t)h);
    le16(1);
    le16(24);
    le32(0);
    le32((uint32_t)stride * h);
    le32(2835);
    le32(2835);
    le32(0);
    le32(0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            unsigned char p[3];
            pattern_pixel(x, y, p);
            out.push_back((char)p[2]);   // B
            out.push_back((char)p[1]);   // G
            out.push_back((char)p[0]);   // R
        }
        for (int pad = w * 3; pad < stride; pad++) out.push_back('\0');
    }
    return out;
}

// A solid-colour PNG. The pattern fixture is for checking that a decode kept its
// pixels in the right order; this one is for checking that a picture reached the
// screen, where the colour has to be one the interface itself would not produce.
inline std::string fx_png_solid(int w, int h, unsigned char r, unsigned char g, unsigned char b) {
    std::string out;
    out.append("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    fx_be32(&ihdr, (uint32_t)w);
    fx_be32(&ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(2);
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    fx_chunk(&out, "IHDR", ihdr);

    std::string raw;
    for (int y = 0; y < h; y++) {
        raw.push_back('\0');
        for (int x = 0; x < w; x++) { fx_push(&raw, r); fx_push(&raw, g); fx_push(&raw, b); }
    }
    std::string zraw;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t take = raw.size() - pos;
        if (take > 600) take = 600;
        bool last = (pos + take == raw.size());
        zraw.push_back((char)(last ? 1 : 0));
        zraw.push_back((char)(take & 0xFF));
        zraw.push_back((char)((take >> 8) & 0xFF));
        zraw.push_back((char)((~take) & 0xFF));
        zraw.push_back((char)((~take >> 8) & 0xFF));
        zraw.append(raw, pos, take);
        pos += take;
    }
    zraw.insert(zraw.begin(), '\x78');
    zraw.insert(zraw.begin() + 1, '\x01');
    uint32_t s1 = 1, s2 = 0;
    for (unsigned char c : raw) { s1 = (s1 + c) % 65521; s2 = (s2 + s1) % 65521; }
    fx_be32(&zraw, (s2 << 16) | s1);
    fx_chunk(&out, "IDAT", zraw);
    fx_chunk(&out, "IEND", "");
    return out;
}

// A WEBP header with nothing behind it: enough for the "recognised, but this build
// cannot preview that" path, which is the promise the feature makes about formats it
// does not do.
inline std::string fx_webp() {
    std::string out;
    out.append("RIFF", 4);
    out.append("\x14\x00\x00\x00", 4);
    out.append("WEBPVP8 ", 8);
    out.append("\x08\x00\x00\x00\x01\x02\x34\x00\x01\x02\x34\x00", 12);
    return out;
}

// A PNG that stops in the middle: the file says it is a picture and the decoder
// cannot finish it, which is the case that must fall back rather than draw garbage.
inline std::string fx_truncated_png() {
    std::string png = fx_png(4, 4);
    return png.substr(0, png.size() - 12);
}

// Where the checked-in JPEG fixtures live (they carry EXIF orientation, which cannot
// be written by hand as compactly as a PNG can).
inline std::string fx_fixture_dir() {
#ifdef MC_IMAGE_FIXTURE_DIR
    return MC_IMAGE_FIXTURE_DIR;
#else
    return ".";
#endif
}

inline bool fx_copy_fixture(const std::string& name, const std::string& dest,
                            std::string* err) {
    std::ifstream in(fx_fixture_dir() + "/" + name, std::ios::binary);
    if (!in) { *err = "fixture " + name + " is not where the build put it"; return false; }
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    std::ofstream o(dest, std::ios::binary);
    if (!o) { *err = "cannot write " + dest; return false; }
    o.write(bytes.data(), (std::streamsize)bytes.size());
    return o.good();
}

#endif
