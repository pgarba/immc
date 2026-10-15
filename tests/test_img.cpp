// Picture recognition and decoding: what a file is, what its pixels are, which way
// it should be turned, and what shrinking it does.
//
// The recognition half runs in every build. The decode half needs the header
// scripts/get-deps.sh fetches, and in a build without it the decode checks say so and
// are skipped rather than failing for want of a dependency - the same rule the remote
// suite follows when the build has no libcurl.
//
// The fixtures are built here, byte by byte (tests/image_fixtures.h), because the CI
// image has no image tooling: a test that needed a photograph would quietly do nothing
// on the runner.

#include "img.h"

#include <stdio.h>
#include <string>
#include <vector>

#include "image_fixtures.h"

static int checks = 0, failures = 0;
#define CHECK(x) do { checks++; if (!(x)) { \
    failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void test_sniff(void) {
    // A format is what the bytes are, not what the file is called. This is the rule
    // that makes photo.JPG, photo.jpg.txt and a PNG named `data` all work.
    CHECK(img_sniff(std::string("\x89PNG\r\n\x1a\n" "rest", 12)) == IMG_PNG);
    CHECK(img_sniff(std::string("\xFF\xD8\xFF\xE0rest", 6)) == IMG_JPEG);
    CHECK(img_sniff(std::string("BMrest", 6)) == IMG_NONE);   // "BM" and nothing behind it
    CHECK(img_sniff(std::string("GIF89a1234", 8)) == IMG_GIF);
    CHECK(img_sniff(std::string("GIF87a1234", 8)) == IMG_GIF);
    CHECK(img_sniff(std::string("P6\n1 1\n255\n\xFF", 11)) == IMG_PNM);
    CHECK(img_sniff(std::string("P7\n1 1\n255\n\xFF", 11)) == IMG_PNM);
    CHECK(img_sniff(std::string("#!RADIANCE\n1 1\n", 14)) == IMG_HDR);
    CHECK(img_sniff(std::string("#!NEWENV\n", 9)) == IMG_HDR);
    CHECK(img_sniff(std::string("#! no newline here at all", 24)) == IMG_NONE);

    // Recognised but not previewed: named, not hex-dumped.
    CHECK(img_sniff(std::string("RIFF\x00\x00\x00\x00WEBPVP8 ", 16)) == IMG_WEBP);
    CHECK(img_sniff(std::string("II\x2a\x00", 4)) == IMG_TIFF);
    CHECK(img_sniff(std::string("MM\x00\x2a", 4)) == IMG_TIFF);
    CHECK(img_sniff(std::string("<svg xmlns=\"x\">", 14)) == IMG_SVG);
    CHECK(img_sniff(std::string("<?xml version=\"1.0\"?>\n<svg a=\"b\">", 30)) == IMG_SVG);
    // An ICO directory: reserved id, one image, and a plausible 1x1 entry. The
    // header is 6 bytes and a directory entry is 16 - width, height, colour count,
    // reserved, planes, bit count, image size, offset - so the whole thing is 22.
    // Writing 20 and asking for 22 reads past the end of the literal, which is the
    // kind of bug a byte-level fixture is supposed to catch in somebody else's code.
    CHECK(img_sniff(std::string("\x00\x00\x01\x00\x01\x00"
                                "\x01\x01\x20\x00\x01\x00\x20\x00\x04\x00\x00\x00\x00\x00\x00\x00",
                                22)) == IMG_ICO);
    CHECK(img_sniff(std::string("\x00\x00\x01\x00\x01\x00\x01\x01\x20\x00\x01\x00", 12))
          == IMG_TGA);   // too short to be an icon directory, so it is the TGA it looks like
    // A colour-mapped TGA starts with the same four bytes as an icon header, and the
    // directory is the only thing that tells them apart.
    CHECK(img_sniff(std::string("\x00\x00\x01\x00\x01\x00"
                                "\x01\x01\x20\x00\x01\x00\x20\x00\x04\x00\x00\x00\x00\x00\x00\x00",
                                22)) == IMG_ICO);
    CHECK(img_sniff(std::string("\x00\x00\x02\x00\x01\x00"
                                "\x01\x01\x20\x00\x01\x00\x20\x00\x04\x00\x00\x00\x00\x00\x00\x00",
                                22)) == IMG_TGA);

    // Not a picture at all.
    CHECK(img_sniff(std::string("hello world")) == IMG_NONE);
    CHECK(img_sniff(std::string()) == IMG_NONE);
    CHECK(img_sniff(std::string("\x89PNG", 4)) == IMG_NONE);      // magic only, no header
    CHECK(img_sniff(std::string("BM\x00\x00", 4)) == IMG_NONE);   // shorter than its header
    CHECK(img_sniff(std::string("BM\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                                "\x00\x00\x99\x99\x99\x99\x00\x00\x00\x00", 24))
          == IMG_NONE);   // "BM" with a header size no BMP writer produces

    // A header cut off mid-field is still the format it claims to be - the magic is
    // intact - and it is the decode that refuses it, not the recognition.
    std::string cut = fx_png(4, 4).substr(0, 20);
    CHECK(img_sniff(cut) == IMG_PNG);

    // The names a refusal prints.
    CHECK(std::string(img_format_name(IMG_WEBP)) == "webp");
    CHECK(std::string(img_format_name(IMG_NONE)) == "unknown");
    CHECK(img_decodable(IMG_PNG) && img_decodable(IMG_JPEG) && img_decodable(IMG_GIF));
    CHECK(!img_decodable(IMG_WEBP) && !img_decodable(IMG_TIFF) && !img_decodable(IMG_SVG));
    CHECK(!img_decodable(IMG_NONE));
}

static void test_png(void) {
    std::string png = fx_png(2, 2);
    ImgFormat f = img_sniff(png);
    CHECK(f == IMG_PNG);

    int w = 0, h = 0, orient = 0;
    std::vector<unsigned char> rgba;
    std::string err;
    CHECK(img_decode(png, f, &w, &h, &rgba, &orient, &err));
    CHECK(err.empty());
    CHECK(w == 2 && h == 2);
    CHECK(rgba.size() == 2 * 2 * 4);
    CHECK(orient == 0);   // a PNG has no EXIF orientation here: nothing was applied

    // stb is asked for 4 channels, so every pixel is four bytes and the alpha is
    // opaque. The values are the fixture's own pattern, so a decode that mixed up
    // channels or rows lands here.
    const unsigned char* p00 = &rgba[0];
    CHECK(p00[0] == 30 && p00[1] == 60 && p00[2] == 128 && p00[3] == 255);
    const unsigned char* p10 = &rgba[(1 + 0 * 2) * 4];
    CHECK(p10[0] == 37 && p10[1] == 60);
    const unsigned char* p01 = &rgba[(0 + 1 * 2) * 4];
    CHECK(p01[0] == 30 && p01[1] == 71);
    const unsigned char* p11 = &rgba[(1 + 1 * 2) * 4];
    CHECK(p11[0] == 37 && p11[1] == 71 && p11[2] == 128);

    // Dimensions without pixels, cheaply enough to ask before allocating them.
    int dw = 0, dh = 0;
    std::string dwhy;
    CHECK(img_dims(png, &dw, &dh, &dwhy));
    CHECK(dw == 2 && dh == 2);
    std::string junk = "not a picture at all";
    CHECK(!img_dims(junk, &dw, &dh, &dwhy));
    // A header claiming a canvas no preview should agree to. The IHDR CRC is
    // recomputed after the change, so what refuses this file is the size guard and
    // not a checksum - which is the difference between this check and a broken file.
    auto png_declaring = [](int want_w, int want_h) {
        std::string p = fx_png(2, 2);
        auto be32_at = [&](size_t at, uint32_t v) {
            p[at]     = (char)((v >> 24) & 0xFF);
            p[at + 1] = (char)((v >> 16) & 0xFF);
            p[at + 2] = (char)((v >> 8) & 0xFF);
            p[at + 3] = (char)(v & 0xFF);
        };
        be32_at(16, (uint32_t)want_w);
        be32_at(20, (uint32_t)want_h);
        // The IHDR data is 13 bytes, so its CRC starts at 29, not at 28: one byte
        // out and the file is refused for a checksum rather than for its size.
        be32_at(29, sfv_crc32((const unsigned char*)p.data() + 12, 17));
        return p;
    };
    std::string huge = png_declaring(9000, 9000);
    CHECK(img_sniff(huge) == IMG_PNG);
    CHECK(img_dims(huge, &dw, &dh, &dwhy));
    CHECK(dw == 9000 && dh == 9000);
    CHECK(!img_decode(huge, IMG_PNG, &w, &h, &rgba, &orient, &err));
    CHECK(err.find("too large") != std::string::npos);
    CHECK(err.find("9000x9000") != std::string::npos);

    // A file that says it is a picture and cannot be finished falls back rather than
    // drawing half a picture.
    std::string broken = fx_truncated_png();
    CHECK(img_sniff(broken) == IMG_PNG);
    CHECK(!img_decode(broken, IMG_PNG, &w, &h, &rgba, &orient, &err));
    CHECK(!err.empty());

    // A format this build recognises but does not decode says which one it is.
    std::string webp = fx_webp();
    CHECK(img_sniff(webp) == IMG_WEBP);
    CHECK(!img_decode(webp, IMG_WEBP, &w, &h, &rgba, &orient, &err));
    CHECK(err.find("webp") != std::string::npos);
}

static void test_bmp(void) {
    std::string bmp = fx_bmp(3, 2);
    CHECK(img_sniff(bmp) == IMG_BMP);
    int w = 0, h = 0, orient = 0;
    std::vector<unsigned char> rgba;
    std::string err;
    CHECK(img_decode(bmp, IMG_BMP, &w, &h, &rgba, &orient, &err));
    CHECK(w == 3 && h == 2);
    CHECK(rgba.size() == 3 * 2 * 4);
    // A BMP stores its first row last. If the decoder got that backwards the top-left
    // pixel would be the pattern's (0,1).
    CHECK(rgba[0] == 30 && rgba[1] == 60 && rgba[2] == 128);
    const unsigned char* br = &rgba[(2 + 1 * 3) * 4];
    CHECK(br[0] == 44 && br[1] == 71 && br[2] == 128);
}

// EXIF orientation, read by hand because stb_image discards the tag and a phone
// photograph is stored sideways on purpose.
//
// The TIFF block inside APP1 can be little- or big-endian, so the builder writes its
// own fields rather than patching bytes afterwards: a test that pokes a hand-built
// file at fixed offsets is a test that breaks whenever the builder changes, and the
// two byte orders are the point of half these checks.
static std::string jpeg_with_orientation(int orientation, bool big_endian = false) {
    std::string out;
    out.append("\xFF\xD8", 2);

    std::string tiff;
    tiff.append(big_endian ? "MM" : "II", 2);
    auto w16 = [&](unsigned v) {
        if (big_endian) { tiff.push_back((char)((v >> 8) & 0xFF)); tiff.push_back((char)(v & 0xFF)); }
        else { tiff.push_back((char)(v & 0xFF)); tiff.push_back((char)((v >> 8) & 0xFF)); }
    };
    auto w32 = [&](unsigned v) {
        if (big_endian) {
            tiff.push_back((char)((v >> 24) & 0xFF)); tiff.push_back((char)((v >> 16) & 0xFF));
            tiff.push_back((char)((v >> 8) & 0xFF));  tiff.push_back((char)(v & 0xFF));
        } else {
            tiff.push_back((char)(v & 0xFF));         tiff.push_back((char)((v >> 8) & 0xFF));
            tiff.push_back((char)((v >> 16) & 0xFF)); tiff.push_back((char)((v >> 24) & 0xFF));
        }
    };
    w16(42);            // the TIFF magic, in the byte order the block declared
    w32(8);             // IFD0 begins straight after the 8-byte header
    w16(1);             // one entry
    w16(0x0112);        // orientation
    w16(3);             // SHORT
    w32(1);             // one value
    w16((unsigned)orientation);
    w32(0);             // no next IFD

    std::string exif;
    exif.append("Exif\0\0", 6);        // NULs in the literal: give the length
    exif += tiff;
    size_t len = exif.size() + 2;
    out.push_back((char)0xFF);         // a marker is two bytes
    out.push_back((char)0xE1);         // APP1, where Exif lives
    out.push_back((char)((len >> 8) & 0xFF));
    out.push_back((char)(len & 0xFF));
    out += exif;
    out.append("\xFF\xD9", 2);
    return out;
}

static void test_exif(void) {
    for (int want = 1; want <= 8; want++) {
        std::string j = jpeg_with_orientation(want);
        CHECK(img_sniff(j) == IMG_JPEG);
        CHECK(img_exif_orientation(j) == want);
    }
    // Big-endian TIFF is the other half of the same tag.
    std::string be = jpeg_with_orientation(6, true);
    CHECK(img_exif_orientation(be) == 6);
    CHECK(img_exif_orientation(be) == 6);

    // A JPEG with no APP1 has no opinion about orientation.
    std::string bare = std::string("\xFF\xD8\xFF\xD9", 4);
    CHECK(img_exif_orientation(bare) == 0);

    // Malformed EXIF must be survived, not trusted. Every one of these is a byte
    // sequence a file on the internet could plausibly have.
    std::string truncated = jpeg_with_orientation(6).substr(0, 20);
    CHECK(img_exif_orientation(truncated) == 0);
    std::string wrongtag = jpeg_with_orientation(6);
    wrongtag[22] = '\x11'; wrongtag[23] = '\x01';     // 0x0111, not 0x0112
    CHECK(img_exif_orientation(wrongtag) == 0);
    std::string tiny_count = jpeg_with_orientation(6);
    tiny_count[20] = '\x00'; tiny_count[21] = '\x00'; // zero entries
    CHECK(img_exif_orientation(tiny_count) == 0);
    std::string huge_count = jpeg_with_orientation(6);
    huge_count[20] = '\xFF'; huge_count[21] = '\xFF'; // more entries than a sane IFD0
    CHECK(img_exif_orientation(huge_count) == 0);
    std::string bad_offset = jpeg_with_orientation(6);
    bad_offset[16] = '\x7F';                           // IFD0 outside the block
    CHECK(img_exif_orientation(bad_offset) == 0);
    std::string no_magic = std::string("\xFF\xE1\x00\x10\x00\x00\x00\x00\x00\x00"
                                       "\x00\x00\x00\x00\x00\x00", 16);
    CHECK(img_exif_orientation(no_magic) == 0);
    // An orientation tag outside 1..8 is not a rotation instruction, and is not
    // passed through as one.
    std::string nine = jpeg_with_orientation(9);
    CHECK(img_exif_orientation(nine) == 0);
}

static void test_apply_orientation(void) {
    // 2x1, red then blue. Each orientation is a different arrangement of the same two
    // pixels, and all eight are reachable.
    auto src = [] { return std::vector<unsigned char>{255, 0, 0, 255, 0, 0, 255, 255}; };
    int w = 2, h = 1;

    auto expect = [&](int orient, int ew, int eh, const std::vector<int>& firsts) {
        // The source size is reset every time: orientations 5..8 swap width and
        // height, and a test that let the previous call's result carry over would
        // spend the rest of its checks rotating the wrong picture.
        std::vector<unsigned char> px = src();
        w = 2; h = 1;
        img_apply_orientation(&w, &h, &px, orient);
        CHECK(w == ew && h == eh);
        CHECK(px.size() == (size_t)ew * eh * 4);
        for (size_t i = 0; i < firsts.size(); i++) CHECK(px[i * 4] == (unsigned char)firsts[i]);
    };

    // A one-row picture: left-right flips swap the two pixels, a top-bottom flip
    // cannot move anything, and the four that turn the picture end up one pixel wide
    // and two tall.
    expect(1, 2, 1, {255, 0});
    expect(2, 2, 1, {0, 255});
    expect(3, 2, 1, {0, 255});
    expect(4, 2, 1, {255, 0});
    expect(5, 1, 2, {255, 0});
    expect(6, 1, 2, {255, 0});
    expect(7, 1, 2, {0, 255});
    expect(8, 1, 2, {0, 255});
    // 0 and anything out of range leave the pixels alone.
    std::vector<unsigned char> px = src();
    w = 2; h = 1;
    img_apply_orientation(&w, &h, &px, 0);
    CHECK(w == 2 && h == 1 && px[0] == 255 && px[4] == 0);
    w = 2; h = 1;
    img_apply_orientation(&w, &h, &px, 99);
    CHECK(w == 2 && h == 1 && px[0] == 255 && px[4] == 0);

    // A square that is turned 90 degrees: the top-left pixel moves, and the size
    // stays, so a test that only looked at the size would miss it.
    std::vector<unsigned char> sq{1, 0, 0, 255, 2, 0, 0, 255,
                                  3, 0, 0, 255, 4, 0, 0, 255};
    w = 2; h = 2;
    img_apply_orientation(&w, &h, &sq, 6);
    CHECK(w == 2 && h == 2);
    CHECK(sq[0] == 3 && sq[4] == 1 && sq[8] == 4 && sq[12] == 2);
}

// The real fixtures: tests/make_image_fixtures.py writes these, and they are the
// only JPEGs in the suite that came from an encoder rather than from this file.
static void test_jpeg_fixtures(void) {
    const std::string dir = fx_fixture_dir();
    std::string plain = dir + "/fixture_plain.jpg";
    std::string turned = dir + "/fixture_orient6.jpg";

    auto read_file = [&](const std::string& p, std::string* out) -> bool {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return !out->empty();
    };

    std::string a, b;
    if (!read_file(plain, &a) || !read_file(turned, &b)) {
        failures++;
        printf("FAIL: JPEG fixtures missing - run tests/make_image_fixtures.py\n");
        return;
    }

    CHECK(img_sniff(a) == IMG_JPEG);
    CHECK(img_sniff(b) == IMG_JPEG);
    CHECK(img_exif_orientation(a) == 0);   // written without the tag
    CHECK(img_exif_orientation(b) == 6);   // written with it

    int w = 0, h = 0, orient = 0;
    std::vector<unsigned char> rgba, rgba6;
    std::string err;
    CHECK(img_decode(a, IMG_JPEG, &w, &h, &rgba, &orient, &err));
    CHECK(w == 16 && h == 8);
    CHECK(orient == 0);
    // The fixture paints a red block in the top-left corner on a green field.
    CHECK(rgba[0] > 150 && rgba[1] < 90);
    CHECK(rgba[(15 + 7 * 16) * 4 + 1] > 150);   // bottom-right is green

    CHECK(img_decode(b, IMG_JPEG, &w, &h, &rgba6, &orient, &err));
    CHECK(orient == 6);
    CHECK(w == 8 && h == 16);   // turned: the long side is now vertical
    CHECK(rgba6.size() == rgba.size());
    // Orientation 6 puts the source's top-left corner at the top-right.
    CHECK(rgba6[(7 + 0 * 8) * 4] > 150);
    // And the red block moved with it rather than staying where the decoder put it.
    CHECK(rgba6[(0 + 15 * 8) * 4] < 90);
}

static void test_fit(void) {
    // 4x2, each pixel its own value, so shrinking can be checked rather than assumed.
    std::vector<unsigned char> px;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 4; x++) {
            px.push_back((unsigned char)(x + 1));
            px.push_back((unsigned char)(y + 1));
            px.push_back(7);
            px.push_back(255);
        }
    int w = 4, h = 2;
    img_fit(&w, &h, &px, 2);
    CHECK(w == 2 && h == 1);
    CHECK(px.size() == 8);
    // Box average: the first target takes the source's first two columns.
    CHECK(px[0] == 1 && px[1] == 1);
    CHECK(px[4] == 3 && px[5] == 1);

    // Already small enough: untouched, and the buffer is not resized.
    std::vector<unsigned char> same = px;
    w = 2; h = 1;
    img_fit(&w, &h, &same, 2048);
    CHECK(w == 2 && h == 1 && same == px);

    // A tall thin image keeps its shape rather than its longest side.
    std::vector<unsigned char> tall(4 * 40 * 4, 9);
    w = 4; h = 40;
    img_fit(&w, &h, &tall, 20);
    CHECK(w == 2 && h == 20);

    // Degenerate input must not divide by zero.
    std::vector<unsigned char> none;
    w = 0; h = 0;
    img_fit(&w, &h, &none, 10);
    CHECK(w == 0 && h == 0);
}

static void test_decoder_presence(void) {
    // A build without the fetched header still recognises, and says what is missing
    // instead of failing quietly.
    std::string png = fx_png(2, 2);
    int w = 0, h = 0, orient = 0;
    std::vector<unsigned char> rgba;
    std::string err;
    bool have = img_decoder_available();
    bool ok = img_decode(png, IMG_PNG, &w, &h, &rgba, &orient, &err);
#ifdef MC_HAVE_STB_IMAGE
    CHECK(have);
    CHECK(ok);
    CHECK(w == 2 && h == 2);
#else
    CHECK(!have);
    CHECK(!ok);
    CHECK(err.find("scripts/get-deps.sh") != std::string::npos);
#endif
}

int main() {
    test_sniff();
    test_exif();
    test_apply_orientation();
    test_fit();

    // Without the fetched header there is nothing to decode, and a suite that fails
    // for want of a dependency is a suite that hides the thing it is really testing.
    // The recognition half - what a file is, which way it should be turned, what
    // shrinking it does - needs no decoder and runs either way.
    if (!img_decoder_available()) {
        printf("no stb_image.h in this build: decode checks skipped\n");
        test_decoder_presence();
        printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }

    test_png();
    test_bmp();
    test_jpeg_fixtures();
    test_decoder_presence();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
