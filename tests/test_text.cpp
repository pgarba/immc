// What alphabet a preview's bytes are in: UTF-8 validation, the CP437 table, and the
// rule that decides when a file is ANSI art rather than a Western European text file.
//
// The rule is the part worth testing. The transcode is arithmetic against a table;
// the judgement is "do these bytes look like someone drew a box", and getting it wrong
// in one direction turns a German text file into line-drawing characters, while getting
// it wrong the other way leaves an .nfo unreadable.

#include "text.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int checks = 0, failures = 0;
#define CHECK(x) do { checks++; if (!(x)) { \
    failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// The anchors everyone remembers, so a mistyped table row is a failing check and not a
// file that looks slightly wrong on screen.
static void test_table(void) {
    struct { unsigned char b; const char* utf8; } want[] = {
        { 0xB0, "\xE2\x96\x91" },   // ░
        { 0xB3, "\xE2\x94\x82" },   // │
        { 0xBA, "\xE2\x95\x91" },   // ║
        { 0xBF, "\xE2\x94\x90" },   // ┐
        { 0xC3, "\xE2\x94\x9C" },   // ├
        { 0xC4, "\xE2\x94\x80" },   // ─
        { 0xCD, "\xE2\x95\x90" },   // ═
        { 0xD9, "\xE2\x94\x98" },   // ┘
        { 0xDB, "\xE2\x96\x88" },   // █
        { 0xDF, "\xE2\x96\x80" },   // ▀
        { 0xBC, "\xE2\x95\x9D" },   // ╝  (0xDD is ╪: the corner is BC, not DD)
    };
    for (auto& w : want) {
        std::string one(1, (char)w.b);
        CHECK(text_cp437_to_utf8(one) == w.utf8);
    }

    // The two code pages people confuse. CP437 has no `ã`; that is Latin-1. CP437's
    // 0x84 is `ä`, and its 0x82 is `é`.
    CHECK(text_cp437_to_utf8(std::string(1, (char)0x84)) == "\xC3\xA4");   // ä
    CHECK(text_cp437_to_utf8(std::string(1, (char)0x82)) == "\xC3\xA9");   // é
    CHECK(text_cp437_to_utf8(std::string(1, (char)0xA0)) == "\xC3\xA1");   // á
    CHECK(text_cp437_to_utf8(std::string(1, (char)0xE1)) == "\xC3\x9F");   // ß

    // ASCII passes through untouched, so a plain text file is byte-identical.
    CHECK(text_cp437_to_utf8("hello\nworld\n") == "hello\nworld\n");
    CHECK(text_cp437_to_utf8("").empty());

    // 0xDF is three bytes in UTF-8 and 0x41 is one: the output is longer than the
    // input exactly where the art is.
    std::string in(100, (char)0xDF), out = text_cp437_to_utf8(in);
    CHECK(out.size() == 300);
    CHECK(text_cp437_to_utf8(std::string(100, 'A')).size() == 100);
}

static void test_utf8(void) {
    CHECK(text_is_utf8("plain ascii"));
    CHECK(text_is_utf8(""));
    CHECK(text_is_utf8("\xC3\xA9"));                  // é, two bytes
    CHECK(text_is_utf8("\xE2\x94\x80"));              // ─, three bytes
    CHECK(text_is_utf8("\xF0\x9F\x98\x80"));          // an emoji, four bytes
    CHECK(text_is_utf8("\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"));

    CHECK(!text_is_utf8("\x80"));                     // a bare continuation byte
    CHECK(!text_is_utf8("\xE2\x94"));                 // cut in half, as a byte cap does
    CHECK(!text_is_utf8("\xE0\x80\x80"));             // over-long: after E0 the second byte starts at A0
    CHECK(!text_is_utf8("\xE2\x94" "\x00"));          // a continuation byte that is not one
    CHECK(!text_is_utf8("\xC0\x80"));                 // over-long encoding of NUL
    CHECK(!text_is_utf8("\xC1\xBF"));                 // over-long encoding of 0x7F
    CHECK(!text_is_utf8("\xED\xA0\x80"));             // a surrogate half
    CHECK(!text_is_utf8("\xF4\x90\x80\x80"));         // past U+10FFFF
    CHECK(!text_is_utf8(std::string("\xDA\xC4\xBF", 3)));   // CP437, and not UTF-8

    // The forms that look wrong and are not: C2 80 is the shortest legal encoding of
    // U+0080, and E2 A0 80 is a Braille pattern, not an over-long form.
    CHECK(text_is_utf8("\xC2\x80"));
    CHECK(text_is_utf8("\xE2\xA0\x80"));

    // A file that is valid UTF-8 with the art already spelled correctly must never be
    // transcoded, however much of it is box drawing.
    std::string modern = "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90\n\xE2\x94\x82 hi \xE2\x94\x82\n";
    CHECK(text_is_utf8(modern));
    CHECK(!text_looks_cp437_art(modern));
}

// A real .nfo shape: a double-line border, a title, a table, and shade characters.
static std::string nfo_cp437(void) {
    std::string s;
    s += "\xC9\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBB\n";      // ══════════╗ ... ╔╗ spelling
    s += "\xBA Release Notes \xBA\n";                        // ║ text ║
    s += "\xC8\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBC\n";      // ╚═════════╝
    s += "\n";
    s += "\xB3 name \xB3 size \xB3\n";                       // │ name │ size │
    s += "\xC3\xC4\xC4\xC4\xC6\xC4\xC4\xC4\xD4\n";          // ├───╞───┤
    s += "\xB3 file \xB3 12kB \xB3\n";
    s += "\xB0\xB0\xB0\xB0\xB0\xB0\xB0\xB0\xB0\xB0\n";      // ░░░░░░░░░░
    return s;
}

static void test_shape(void) {
    std::string s = nfo_cp437();
    int total = 0, distinct = 0, run = 0;
    text_cp437_art_shape(s, &total, &distinct, &run);
    CHECK(total > 30);
    // C9 CD BB BA C8 BC B3 C3 C6 D4 B0 ... a border uses many different bytes.
    CHECK(distinct >= 8);
    CHECK(run >= 4);          // the border line itself

    // Nothing high: nothing to shape.
    text_cp437_art_shape("all ascii\n", &total, &distinct, &run);
    CHECK(total == 0 && distinct == 0 && run == 0);

    // A run counts consecutive art bytes and resets at the first byte outside them.
    std::string r;
    r.append("\xDF\xDF\xDF\xDF\xDF", 5);
    r += " text ";
    r.append("\xDF\xDF", 2);
    text_cp437_art_shape(r, &total, &distinct, &run);
    CHECK(total == 7);
    CHECK(run == 5);
    CHECK(distinct == 1);
}

static void test_art_rule(void) {
    // A .nfo, which is what this exists for.
    CHECK(text_looks_cp437_art(nfo_cp437()));

    // A divider line on its own: four of the same byte is still a border.
    CHECK(text_looks_cp437_art(std::string("\xDB\xDB\xDB\xDB\xDB\n")));

    // A table whose box characters only meet in its rules. The rules are runs, which
    // is the point of the first half of the rule.
    std::string table;
    table += "\xBA a \xBA b \xBA\n";
    table += "\xC3\xC4\xC4\xC6\xC4\xC4\xD4\n";
    table += "\xBA c \xBA d \xBA\n";
    table += "\xC8\xCD\xCD\xCB\xCD\xCD\xBC\n";
    int total = 0, distinct = 0, run = 0;
    text_cp437_art_shape(table, &total, &distinct, &run);
    CHECK(run >= 4);                 // a horizontal rule is a run of box characters
    CHECK(distinct >= 6);
    CHECK(text_looks_cp437_art(table));

    // The other shape: box characters that never touch, held apart by text. No run at
    // all, and only distinctness can tell this from a few stray high bytes.
    std::string scattered;
    scattered += "\xBA a \xBF b \xBC\n";
    scattered += "\xBA c \xBD d \xB3\n";
    scattered += "\xC8 e \xC9 f \xCA\n";
    text_cp437_art_shape(scattered, &total, &distinct, &run);
    CHECK(total == 9);
    CHECK(run == 1);
    CHECK(distinct >= 6);
    CHECK(text_looks_cp437_art(scattered));

    // Too few to be anything. Three bytes of a box is not a drawing. Split literals:
    // "\xC4\xC4\xC4b" would be read as one enormous hex escape, C444B.
    CHECK(!text_looks_cp437_art(std::string("a\xC4\xC4\xC4" "b", 6)));

    // A Western European text file saved in 8-bit. The accents that live outside
    // 0xB0-0xDF are the easy half; the ones that happen to live inside it - Ä, Ö, Ü, ß -
    // are the reason the rule looks at runs and not at counts, because four accented
    // letters in a row is not a thing prose does.
    std::string latin = "caf\xe9 na\xefve r\xe9sum\xe9\n", latin2;
    latin2 += "\xDC" "bersicht \xFC" "ber die Stra\xDF" "e\n";       // Übersicht über die Straße
    latin2 += "seit 1987 in K\xf6ln und Z\xfcrich\n";
    text_cp437_art_shape(latin2, &total, &distinct, &run);
    CHECK(run < 4);
    CHECK(distinct < 6);
    CHECK(!text_looks_cp437_art(latin));
    CHECK(!text_looks_cp437_art(latin2));

    // ASCII is valid UTF-8, so it is never art as far as this rule is concerned.
    CHECK(!text_looks_cp437_art("README\n=====\n"));
}

static void test_decide(void) {
    std::string art = nfo_cp437();
    std::string utf8 = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n";
    std::string prose = "just words\n";

    // auto: art is transcoded, UTF-8 and plain text are left alone.
    CHECK(text_decide_codepage(art, TEXT_CODEPAGE_AUTO) == TEXT_CODEPAGE_CP437);
    CHECK(text_decide_codepage(utf8, TEXT_CODEPAGE_AUTO) == TEXT_CODEPAGE_UTF8);
    CHECK(text_decide_codepage(prose, TEXT_CODEPAGE_AUTO) == TEXT_CODEPAGE_UTF8);

    // The overrides do what they say, in both directions: utf8 refuses even a real
    // .nfo, and cp437 applies even to a Latin-1 file the rule turned down.
    CHECK(text_decide_codepage(art, TEXT_CODEPAGE_UTF8) == TEXT_CODEPAGE_UTF8);
    CHECK(text_decide_codepage(utf8, TEXT_CODEPAGE_CP437) == TEXT_CODEPAGE_CP437);
    CHECK(text_decide_codepage("caf\xe9\n", TEXT_CODEPAGE_CP437) == TEXT_CODEPAGE_CP437);
    CHECK(text_decide_codepage("caf\xe9\n", TEXT_CODEPAGE_AUTO) == TEXT_CODEPAGE_UTF8);

    // Config names, and the rule that an unrecognised one falls back rather than
    // silently selecting something else.
    CHECK(text_codepage_from_name("auto") == TEXT_CODEPAGE_AUTO);
    CHECK(text_codepage_from_name("") == TEXT_CODEPAGE_AUTO);
    CHECK(text_codepage_from_name("utf8") == TEXT_CODEPAGE_UTF8);
    CHECK(text_codepage_from_name("utf-8") == TEXT_CODEPAGE_UTF8);
    CHECK(text_codepage_from_name("cp437") == TEXT_CODEPAGE_CP437);
    CHECK(text_codepage_from_name("koi8-r") == TEXT_CODEPAGE_AUTO);
    CHECK(text_codepage_from_name(nullptr) == TEXT_CODEPAGE_AUTO);
    CHECK(!strcmp(text_codepage_name(TEXT_CODEPAGE_AUTO), "auto"));
    CHECK(!strcmp(text_codepage_name(TEXT_CODEPAGE_UTF8), "utf8"));
    CHECK(!strcmp(text_codepage_name(TEXT_CODEPAGE_CP437), "cp437"));
    // What the About window and a test can round-trip through.
    CHECK(text_codepage_from_name(text_codepage_name(TEXT_CODEPAGE_CP437)) == TEXT_CODEPAGE_CP437);
}

int main() {
    test_table();
    test_utf8();
    test_shape();
    test_art_rule();
    test_decide();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
