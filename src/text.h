#ifndef MC_TEXT_H
#define MC_TEXT_H

#include <string>

// What alphabet a preview's bytes are written in.
//
// The quick view hands a file's bytes to ImGui, and ImGui reads text as UTF-8. A file
// that is UTF-8, or pure ASCII, is therefore fine untouched. An `.nfo`, a `.diz`, a
// BBS ANSI art file - anything from the DOS world - is written in CP437, where the
// box-drawing and shade characters are the single bytes 0xB0 to 0xDF. Those bytes are
// not valid UTF-8, so a border drawn with `─` arrives as garbage: three characters of
// art become two characters of nothing.
//
// The fix is a transcode. The hard part is not the table, it is deciding *when*: a
// Western European text file saved in 8-bit Latin-1 is also "not valid UTF-8", and
// reading it as CP437 would turn `É` into `╔`. So the decision is made from the shape
// of the high bytes, and it is overridable.

enum TextCodepage {
    TEXT_CODEPAGE_AUTO = 0,   // decide from the bytes
    TEXT_CODEPAGE_UTF8,       // never transcode
    TEXT_CODEPAGE_CP437       // always transcode
};

// True when every byte of `bytes` is well-formed UTF-8. A sequence cut in half by the
// preview's byte cap counts as not well formed, which is what it is.
bool text_is_utf8(const std::string& bytes);

// CP437's box-drawing and shade block, 0xB0 to 0xDF: ░▒▓│┤╡╢╖╕╣║╗╝╜╛┐ and
// └┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀. Everything an ANSI artist reaches for.
// Also the range where Latin-1's accented capitals happen to sit, which is why the
// count alone is not the answer and `text_looks_cp437_art` looks at runs instead.
bool text_byte_is_cp437_art(unsigned char b);

// The shape of the high bytes: how many are in the art block, how many different ones,
// and the longest run of them in a row. A border is a run. Prose is not.
void text_cp437_art_shape(const std::string& bytes, int* total, int* distinct, int* longest_run);

// The rule, in one place so it can be argued about and tested:
//   - the bytes are not valid UTF-8 (otherwise UTF-8 is the answer, always), and
//   - at least four of them are in the art block, and
//   - either four of them in a row - a border - or at least six different ones, which
//     is what a table of box characters looks like when the text between them is ASCII.
// A file that fails this gets what it always got. `preview_codepage = cp437` forces
// the transcode for the case the rule misses.
bool text_looks_cp437_art(const std::string& bytes);

// What to do with these bytes, given what the config asked for. AUTO consults the
// rule above; the other two do what they say.
TextCodepage text_decide_codepage(const std::string& bytes, TextCodepage requested);

// CP437 bytes to UTF-8. Bytes below 0x80 pass through, so ASCII is unchanged. The
// table is the one Python's cp437 codec produces, not one typed from memory.
std::string text_cp437_to_utf8(const std::string& bytes);

// "auto" / "utf8" / "cp437" as stored in the config, and back. Anything unrecognised
// comes back as AUTO, the same rule `sfv_verify` follows: a typo should not silently
// turn off a behaviour.
TextCodepage text_codepage_from_name(const char* name);
const char* text_codepage_name(TextCodepage page);

#endif
