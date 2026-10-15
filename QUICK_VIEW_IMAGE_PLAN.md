# Quick view shows pictures — F3 on an image file

Status: **built.** §12 is what turned out differently once it was written.

`F3` on a picture today prints `(binary - hex dump)` and 2048 bytes of the file's
insides. This makes the quick view draw the picture instead, and leaves everything
else it does today exactly as it is.

---

## 1. The shape

The quick view window keeps its header and swaps the body:

```
┌ Quick view ────────────────────────────────────────────────┐
│ DSC_0417.jpg                                               │
│ Type: file                                               │
│ Size: 3417880                                            │
│ Mode: -rw-r--r--                                         │
│ ─────────────────────────────────────────────────────────  │
│                                                          │
│        ┌────────────────────────────────────┐            │
│        │                                    │            │
│        │        the picture, fitted         │            │
│        │        into the space below        │            │
│        │        the header, aspect kept     │            │
│        │                                    │            │
│        └────────────────────────────────────┘            │
│ 4032×3024 JPEG · 3.3 MB · shown 700×525 · rotated 90°    │
└──────────────────────────────────────────────────────────┘
```

- **`F3`** on a row that decodes to a picture draws it. `F3` on anything else does
  what it does now: text, or the hex dump.
- The **`Ctrl+Q` quick view panel** draws pictures too. It already shares the
  preview code with the `F3` window, so it gets the same body.
- The picture **fits the space left under the header** and is never upscaled: a
  100×80 icon shows at 100×80, a 4032×3024 photo shows at 700×525 in a 720×460
  window. Dragging the window refits it, which is free — `render_quick_view`
  already reads `GetWindowSize()` every frame @5599.
- The caption line is the honest one: what the file really is, how big it is, how
  big it is being drawn, and whether the picture was rotated to match the camera.
- `Alt+F3` still opens the file with the desktop's default program, and that stays
  the answer for a format this build cannot decode.

---

## 2. Decisions

| Point | Decision | Why |
|---|---|---|
| Which rows | Local files and archive members. A row on a site keeps today's preview. | Quick view on a site already pulls only the first 64 KB on purpose (`net_download_prefix` @5058). Finding out what a 3 MB JPEG on a server is should not cost 3 MB over FTP. |
| Decoder | **stb_image**, fetched pinned by `scripts/get-deps.sh`, like ImGui. | One public-domain header, no system dependency, decodes PNG / JPEG / BMP / GIF / PNM / TGA / HDR. A build without libcurl is unaffected because nothing here links a new library. |
| Detection | **Content, not extension.** Sniff the head bytes the preview already read. | `photo.JPG`, `photo.jpg.txt` and a PNG named `data` all work. An extension table is a list of promises this build cannot keep (`.webp`, `.tif`, `.svg` are not decodable here). |
| Read order | Open once, read 64 KB, sniff, and **keep reading the same handle** if it is a picture. | No second read, and the text path is untouched: a file that is not a picture never reads past the 64 KB it reads today. |
| Threading | Local pictures move onto the existing worker. | Today `qv_preview` reads a local file **on the UI thread** @5161 because 64 KB is nothing. 3 MB plus a decode is not nothing, and a UI that stalls on a thumbnail is worse than no thumbnail. |
| EXIF | Parse the JPEG orientation tag and rotate the decoded pixels. | Most phone photos carry orientation 6 or 8. Without this the feature's headline case — a photo from a phone — lands sideways. |
| Size limits | Ask the format for its dimensions **before** decoding, refuse above a cap, and box-downscale anything above 4096 px per side. | A 200 KB PNG can declare 30000×30000. Decoding that is 3.6 GB of pixels, and the file that does it is on disk in a directory being browsed. |
| Texture | The app creates a `GLuint` and hands it to `ImGui::Image` as a raw `ImTextureID`. | This build's renderer is `ImGui_ImplOpenGL3_InitForOpenGL` (main.cpp @161) and that backend binds `(GLuint)(intptr_t)pcmd->GetTexID()` (imgui_impl_opengl3.cpp @618). There is no `ImGui::CreateTexture` in the pinned ImGui (1.93.0 WIP) and the OpenGL3 backend ships no device-texture creation. |
| Reach | Quick view only: the `F3` window and the `Ctrl+Q` panel. | The viewer (`F4`) and thumbnails in the rows are different features with different costs. Thumbnails mean a cache of many decodes per directory load. |
| Off switch | `qv_images = 1 \| 0` in the config file, not in the Options dialog. | Same rule as `console_save`: a knob for someone who wants the old behaviour, not a checkbox to explain to everyone. |

---

## 3. The dependency

`extern_dont_commit/` is gitignored and holds no tracked files — ImGui is *fetched*,
by `scripts/get-deps.sh`, from a pinned commit, and CI runs that script
(`.github/workflows/ci.yml`, "fetch Dear ImGui"). A decoder follows the same rule
rather than becoming the first vendored thing in the tree.

`scripts/get-deps.sh` gains a second half:

```sh
STB_REPO="${STB_REPO:-https://raw.githubusercontent.com/nothings/stb}"
STB_COMMIT="${STB_COMMIT:-f58386843249a2406d303800235de0f842f0c23d}"   # v2.30
STB_SHA256="${STB_SHA256:-<pinned hash of stb_image.h at that commit>}"

dest="${STB_DEST:-$here/../extern_dont_commit/stb}"
mkdir -p "$dest"
if [ ! -f "$dest/stb_image.h" ] || ! sha256sum -c "$STB_SHA256" "$dest/stb_image.h" >/dev/null 2>&1; then
    curl -fsSL "$STB_REPO/$STB_COMMIT/stb_image.h" -o "$dest/stb_image.h.tmp"
    sha256sum -c "$STB_SHA256" "$dest/stb_image.h.tmp"
    mv "$dest/stb_image.h.tmp" "$dest/stb_image.h"
fi
```

Pinned by commit and checked by hash for the same reason ImGui is: a build whose own
source never changed should not break because someone else's repository moved. The
`.tmp` then hash-then-move order means a refused download cannot leave a truncated
header that the next build half-compiles with.

`CMakeLists.txt` gains:

```cmake
set(STB_DIR ${CMAKE_SOURCE_DIR}/extern_dont_commit/stb)
```

and `src/img.cpp` is added to the `mc-clone`, `test-panes` source lists with
`${STB_DIR}` on their include path. `stb_image.h` is included **once** with
`#define STB_IMAGE_IMPLEMENTATION` — in `src/img.cpp`, which is the only file that
knows stb exists.

A fresh clone needs `scripts/get-deps.sh` run before the first configure, which is
already true today for ImGui, and CI already does it.

**If the header is missing**, `src/img.cpp` compiles against a stub: `img_decode()`
reports "this build has no image decoder" and quick view shows the hex dump. Same
shape as the no-libcurl build — a supported configuration, not a broken one.

---

## 4. `src/img.{h,cpp}` — decode, and nothing else

No SDL, no ImGui, no GL, no panes. That is what makes it testable headless, and it is
the half the tests will cover hardest.

```cpp
enum ImgFormat { IMG_NONE, IMG_PNG, IMG_JPEG, IMG_BMP, IMG_GIF,
                 IMG_PNM, IMG_TGA, IMG_HDR,
                 IMG_UNSUPPORTED };   // recognised, and this build cannot decode it

// What these bytes are, from the bytes themselves.
ImgFormat img_sniff(const unsigned char* p, size_t n);

// Dimensions without decoding: the guard against a file that declares a huge canvas.
bool img_dims(const std::string& bytes, int* w, int* h, std::string* err);

// Bytes -> RGBA8, top row first, rotated to match the camera.
bool img_decode(const std::string& bytes, ImgFormat fmt,
                int* w, int* h, std::vector<unsigned char>* rgba,
                int* exif_orientation, std::string* err);

// Integer box-downscale in place, to fit a box. Returns the scale used.
float img_fit(int* w, int* h, std::vector<unsigned char>* rgba, int max_px);
```

**Sniffing.** PNG `\x89PNG\r\n\x1a\n`, JPEG `\xFF\xD8\xFF`, BMP `BM`, GIF `GIF8`,
PNM `P1`–`P7`, TGA and HDR by their shapes. `IMG_UNSUPPORTED` for WEBP (`RIFF....WEBP`),
TIFF (`II*\0` / `MM\0*`), SVG (`<svg`), and ICO — recognised so the window can say
*"webp: this build cannot decode it, Alt+F3 opens it with the desktop's program"*
instead of printing hex and leaving the user to guess why.

**EXIF orientation.** stb_image reads the pixels and throws the EXIF away. The
orientation is read from the JPEG bytes here: walk the marker segments from `SOI`,
take `APP1` whose payload starts `Exif\0\0`, parse the TIFF header (its own byte
order, little or big), walk IFD0, find tag `0x0112`, and apply one of eight
transforms to the decoded RGBA. Everything about this walks untrusted bytes, so every
offset is bounds-checked and a malformed APP1 answers "no orientation" rather than
anything else. JPEG only: PNG's `eXIf` chunk is rare enough to leave out and say so.

**The dimension guard.** `img_dims` wraps `stbi_info_from_memory`, which reads the
header without decoding. `k_img_max_px_side = 16384` refuses before a single pixel is
allocated, and the refusal is a sentence with the numbers in it:
`too large to preview (30000×30000)`.

**The downscale.** `img_fit` box-averages by an integer factor so a 4032×3024 decode
lands at 2016×1512 before it ever reaches the GPU: a quarter of the memory and the
same picture at the size the window can draw. `k_img_tex_max_px = 4096` per side.

**Row order.** stb_image returns row 0 as the top row and the texture is uploaded
unchanged, drawn with uv `(0,0)→(1,1)`. `stbi_set_flip_vertically_on_load` is **not**
used: it is a global, and this app has three quick views on two threads. Where a flag
is needed it is `stbi_set_flip_vertically_on_load_thread`, which is the thread-local
form stb ships for exactly that.

---

## 5. `src/texture.{h,cpp}` — the GL half, kept apart on purpose

```cpp
bool     tex_available(void);                                     // a GL context is current
unsigned long long tex_upload(int w, int h, const unsigned char* rgba);   // 0 on failure
void     tex_free(unsigned long long id);
```

Kept in its own file so `src/img.cpp` stays testable and `src/panes.cpp` never
includes a GL header.

**Entry points** come from `imgui_impl_opengl3_loader.h`, which the pinned ImGui
exports as one `extern union ImGL3WProcs imgl3wProcs` (line 562) already populated by
`ImGui_ImplOpenGL3_Init`. Using it means the functions are the ones the renderer
itself calls against this context. If any of `GenTextures`, `BindTexture`,
`TexParameteri`, `TexImage2D`, `DeleteTextures` is null in that table,
`tex_available()` says false and quick view falls back to the hex dump.
`SDL_GL_GetProcAddress` is the fallback for the same five symbols if the loader path
turns out to be awkward; it resolves against the context SDL created.

**The guard is `SDL_GL_GetCurrentContext() != nullptr`.** This matters for the tests:
`test-panes` builds ImGui contexts and draws frames headless, and a `glGenTextures`
call with no context is a crash. With the guard, the whole image pipeline runs in the
test process — read, sniff, decode, EXIF, fit, cache — and only the upload is skipped.
The feature is then testable where it is testable and the untestable half is four
lines wide.

**Texture lifetime.** Created lazily on the UI thread the first time a decoded image
is drawn (a context is current inside the frame). Destroyed when its record is
evicted from the cache, and every remaining one in `app_panes_free`, which main.cpp
already calls at @335 **before** `ImGui_ImplOpenGL3_Shutdown()` @336 and
`SDL_GL_DestroyContext()` @340. That order is what makes the teardown safe, and it is
already the order.

---

## 6. Fitting it into the preview pipeline

The pipeline is already the right shape and is extended, not replaced.

```
qv_job_for @5135      which row, which kind  ->  unchanged
qv_preview @5156      cache hit? worker?     ->  gains the image branch
qv_read_file @5034    read up to 64 KB       ->  reads on when the head is a picture
qv_run_fetch @5052    worker: read, decode   ->  decodes when it is a picture
qv_reap @5104         file the answer        ->  files it in the right cache
render_quick_view @5592 / render_quick_view_panel @5186   ->  share one body helper
```

**One read, decided by content.** `qv_read_file` opens the file, reads the first
64 KB as it does now, sniffs, and only then continues to `k_qv_image_max_bytes`
(32 MB) on the same handle. A file that is not a picture never reads past what it
reads today, so `k_qv_max_bytes` @4968 and every text assertion in the suite stay.

**Local pictures go on the worker.** `qv_preview` @5161 answers a local row
synchronously today. It keeps doing that for text. For a row whose head sniffs as a
picture it takes the worker path instead — start a `QvFetch`, return `k_qv_reading`,
and the next frame has the picture. The read loop on the worker checks `f->cancel`
between chunks, so a 30 MB file the cursor already left stops being read; today that
cancel question is only asked by the curl progress callback @5046, which local reads
never see.

**Two caches, because the things in them are different sizes.**

```cpp
struct QvImage {
    int w = 0, h = 0;
    ImgFormat fmt = IMG_NONE;
    int orientation = 0;              // the EXIF tag that was applied, 0 if none
    long long file_bytes = 0;
    std::vector<unsigned char> rgba;
    unsigned long long tex = 0;       // 0 until it is first drawn
    std::string err;                  // why it is not a picture
};
std::vector<std::pair<std::string, std::unique_ptr<QvImage>>> qv_images;  // in AppPanes
```

`k_qv_image_cache_max = 4` and `k_qv_image_cache_bytes = 96 MB`, oldest evicted
first, texture freed on eviction. The text cache stays at 32 entries @4972: text
previews are kilobytes and pictures are megabytes, and one cap cannot serve both.

`QvFetch` gains `bool is_image`, the same fields, and `qv_reap` files the answer in
`qv_images` or `qv_cache` depending on which it is. A failed decode is **not** cached,
so the next look at that name tries again — the rule `qv_reap` already follows for a
failed text read.

**The drawing** is one helper both windows call:

```cpp
// Returns true when a picture was drawn for this row.
static bool qv_draw_image(AppPanes* app, const Pane& pane, int idx);
```

Called from `render_quick_view` before the `text == k_qv_binary` hex-dump branch
@5626, and from `render_quick_view_panel`. When it returns true the hex dump and the
text body are skipped; when it returns false nothing about today's behaviour changes.

Inside: `ImGui::Image(ImTextureRef((ImTextureID)img->tex), ImVec2(dw, dh))` sized to
`ImGui::GetContentRegionAvail()` with the aspect kept and `scale` clamped to 1.0,
then the caption. `style.ImageBorderSize` is forced to 0 for this one call — the
theme gives frames a border, and a border around a photograph reads as a mistake.

**Archive members** work because `arch_extract_temp` @5063 already puts the whole
member on a temp file and the read is the same read. **Remote rows** are excluded in
`qv_job_for`: `job.kind == QV_REMOTE` never takes the image branch, and the caption
for a picture on a site is today's text or hex, unchanged.

---

## 7. Config

`MCConfig` gains `int qv_images;` — default 1, loaded and saved next to `qv_w` and
`qv_h`, copied into `AppPanes::qv_images` by `app_panes_apply_state` the way
`sfv_verify` already is. With `qv_images = 0` the sniff is never reached and a
picture row shows the hex dump exactly as it does today.

Not in the Options dialog, following `console_save`: it is a setting for the person
who wants the old behaviour back, and every dialog row is a thing every user has to
read past.

---

## 8. Tests

`src/img.cpp` is a headless module, so the hard cases are unit-testable and none of
them need a display, a network, or an image tool that CI does not have. CI installs
`cmake g++ curl zip unzip p7zip-full python3` and no ImageMagick and no PIL, so the
fixtures are **written by the test itself**.

**Fixtures built in C++:**

- **BMP** — an 8×8 24-bit bitmap, byte by byte. Trivial, and it exercises the decode
  path with no format subtleties at all.
- **PNG** — a hand-built 8×8 RGBA file: signature, `IHDR`, `IDAT` carrying **stored
  (uncompressed) deflate blocks** with an adler32, `IEND`. Chunk CRCs use the CRC-32
  that already exists in this repository as `sfv_crc32` in `src/sfv.cpp`, which
  `test-panes` already links. No zlib, no external tool, and a real PNG the decoder
  cannot tell apart from one a program wrote.
- **JPEG** — a small real JPEG embedded as a byte array, generated once by hand with
  a local tool and pasted in, with an `APP1`/Exif segment carrying orientation 6.
  Regenerating it is a note in the test, not a build step.
- **A PNG whose `IHDR` claims 30000×30000** — the decompression bomb, refused by
  `img_dims` before allocation.
- **A truncated PNG** — valid signature, cut mid-`IDAT`: sniff says picture, decode
  fails, quick view falls back to the hex dump.
- **Fuzzed `APP1`** — a JPEG whose EXIF segment is truncated, has a bogus IFD count,
  an out-of-range offset and both byte orders. Every one answers "no orientation".

**`test_image_decode`** (new `tests/test_img.cpp`, its own `add_test`, plus the
existing four suites staying as they are): sniff on each format and on the
unsupported ones; `img_dims` on the bomb; decode BMP and PNG and check specific
pixels; the eight EXIF orientations each producing the expected pixel positions;
`img_fit` scaling 4032×3024 to 2016×1512 and leaving a 100×80 alone; a garbage buffer
returning false with a reason.

**`test_qv_images`** in `test-panes`, headless ImGui frames like the ones the transfer
strip and the tab bar already use:

- a PNG row: `qv_preview` returns `k_qv_reading`, the worker answers, the next frame
  has a `QvImage` with the right dimensions and format
- a text row and a non-image binary row are **unchanged**, asserted against the same
  strings the existing suite checks
- a picture inside a zip built with `zip` decodes; a picture on a site does not take
  the image branch (asserted with the fixture-free `NetSite()` the resume tests
  already use)
- the cache: two looks at one name decode once; a fifth picture evicts the oldest
  and its texture id is released
- `qv_images = 0` restores the hex dump
- `tex_available()` is false in this process and **no texture is created** — the
  guard is what keeps a headless frame from calling GL, and it is worth an assertion
  of its own
- a cursor that moves on while a big local file is being read cancels the read
  instead of finishing it

**Manual check**, because a picture on a screen is the one part no headless test can
confirm: `SDL_VIDEODRIVER=offscreen` for the regression run, and a real window for
`samples/sample1.jpg`, a rotated phone JPEG, a 4000×3000 PNG, a webp, and a corrupt
file — the last two to see the sentences rather than a crash.

---

## 9. Phases

1. **`src/img.{h,cpp}`** — sniff, dims, decode, EXIF, fit. `scripts/get-deps.sh` and
   `CMakeLists.txt` wiring, the missing-header stub, `tests/test_img.cpp`. Nothing
   else in the app changes yet; the feature is invisible and the tests are the whole
   phase.
2. **`src/texture.{h,cpp}`** — the GL half with the context guard, and a temporary
   hard-coded path in `render_quick_view` to prove a picture reaches the screen.
   Deleted at the end of the phase.
3. **The pipeline** — one longer read, the image branch in `qv_preview`, local
   pictures on the worker, `QvImage` and its cache, `qv_draw_image` shared by the
   `F3` window and the `Ctrl+Q` panel, the caption, `qv_images` in the config.
   `test_qv_images`.
4. **Docs** — README key table note under `F3`, `src/img.*` and `src/texture.*` in
   "Where things are", the `stb_image` line in the dependency paragraph, a
   `test-img` row in the test-suites table, and a FEATURES_PLAN row.

---

## 10. Deliberately not here

- **Thumbnails in the file list.** A row in this app is text in a column layout; a
  row that can hold a bitmap is a different widget, and a directory that decodes
  sixty pictures on load needs a thumbnail cache, a queue, and a policy for what to
  drop. That is its own feature.
- **The `F4` viewer.** It gets a picture later if the quick view proves worth it;
  sharing `QvImage` is exactly what makes that cheap later, which is why the cache is
  keyed by path and not by window.
- **Pictures on a site.** Decided in §2: a preview should not download the file.
- **WEBP, TIFF, SVG, ICO.** Named and refused with a sentence. WEBP needs a decoder
  this build does not have; SVG needs a rasterizer, which is a different dependency
  class entirely.
- **Animated GIF.** stb_image returns the first frame. The caption says `GIF` and the
  window shows a still; animation is not a preview concern.
- **Colour management.** No ICC profile is applied. A wide-gamut JPEG shows with
  whatever the decoder produced, which is slightly off and not worth a colour engine.
- **Zoom and 1:1 toggling.** Fit only. Once the picture is on screen, the keys to
  move around it are a second feature with its own keyboard questions, in a window
  whose keys already belong to the panes.

---

## 11. What can go wrong, and what the plan does about it

| Risk | The answer here |
|---|---|
| A file that declares an enormous canvas | `img_dims` before decoding, 16384 px per side, refusal names the numbers. |
| Malformed EXIF walked as untrusted bytes | Bounds-checked parse, fuzzed fixtures, a bad segment answers "no orientation". |
| GL called with no context in a headless test | `tex_available()` guards every call; the pipeline is tested up to the pixels. |
| A 30 MB read the cursor already left | `f->cancel` checked between chunks on the worker, not only in the curl callback. |
| Three quick views fighting over a global stb flag | `stbi_set_flip_vertically_on_load_thread`, and no flip at all in this design. |
| GPU memory | 4096 px per side, box-downscale before upload, 96 MB cache budget, texture freed on eviction. |
| `app_panes_free` destroying textures after GL is gone | main.cpp already frees the panes @335 before the renderer @336 and the context @340. Asserted, not assumed. |
| A build with no `stb_image.h` | Compiles against a stub, quick view shows the hex dump, same shape as the no-libcurl build. |
| The pinned ImGui has no texture-creation API | Confirmed against the pinned tree: 1.93.0 WIP, `Image(ImTextureRef, …)` and a raw `GLuint` through `GetTexID()` at imgui_impl_opengl3.cpp @618. |

---

## 12. What changed while it was built

**The texture layer asks SDL, not the renderer's loader.** The plan's first choice was
`imgui_impl_opengl3_loader.h`, because that is the table the OpenGL backend itself calls
through. In the end `src/texture.cpp` resolves the six entry points with
`SDL_GL_GetProcAddress`. Same context, same functions, and the difference matters twice:
`imgl3wProcs` is a name inside one backend's private header, and depending on it makes
this file untestable without ImGui. With SDL, `tests/test_texture.cpp` needs a window, a
context, and nothing else.

**Recognition is a mechanism with two halves, and both are checked.** A name that says
picture routes the read onto a worker. A name that does not gets the old synchronous
screenful, and if the magic is in it the rest of the file goes to a worker a frame later.
Removing the routing hint fails 2 checks; removing the sniff escape fails 4. Neither is
decorative.

**The size guard is the one that catches the merely very large.** stb refuses a
30000x30000 PNG before it will report the size at all, so the project's caps - 8192 on a
side, 40 million pixels - are what catch a real scan or a panorama. The test declares
9000x9000 in a header whose CRC is correct, so what refuses it is the guard and not a
checksum.

**A refusal is a sentence, not a hex dump.** A file that claims to be a picture and
cannot be decoded - truncated, oversized, or a format this build recognises but does not
do - is cached as the sentence explaining it. The hex dump is what a file that is not a
picture gets, which is what it always got.

**The caption keeps two sizes.** `img_decode` returns the pixels already turned to match
the camera, so the fetch asks `img_dims()` for the canvas the file declared before
decoding. `16x8 JPEG shown 8x16 turned to match the camera` is the difference between a
preview and a lie about one.

**The fixtures found four bugs, all in the tests.** A PNG chunk is length, type and data,
then CRC - writing the CRC before the body makes a file that is a magic number and
nothing else. ADLER32 is two running sums. The IHDR CRC is at byte 29, not 28. An icon
directory entry is 16 bytes, and a 20-byte fixture asked for at length 22 was an
out-of-bounds read that ASAN reported on the spot.

**Two sniffers were wrong.** Radiance HDR starts `#!` as well as `#?` - `#!RADIANCE` and
`#!NEWENV` are both written by real tools - and `BM` alone is not a BMP: the header size
after it has to be one of 12, 40, 64, 108 or 124.

**`test-gl_preview` exists because "made a texture" is not "drew a picture".** It renders
a solid magenta fixture through the real renderer, reads the framebuffer back, and
requires a run of magenta pixels. Deleting the `ImGui::Image()` call and leaving the
upload in place fails that check and nothing else. It skips itself, with a printed reason,
where there is no display or no decoder.

**Verification.** `ctest` is 8/8 with libcurl, 8/8 without it, and 8/8 with no decoder
fetched. `test-panes` 2145 checks, `test-img` 148 (102 with the skip line in a build with
no decoder), `test-texture` 12, `test-gl-preview` 7. ASAN is clean over the new code; the
reports it still makes are pre-existing test hygiene and libdbus internals.
