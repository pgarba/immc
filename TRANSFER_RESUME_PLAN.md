# Resume for a stopped transfer, a real choice when the target exists, and SFV verification

Status: **built.** Three changes that belong together: a stopped FTP
transfer gets a `Resume` that continues from the bytes that already arrived, the
"already exists" prompt stops being a Yes/No, and a `.sfv` that turns up in the
destination is offered as a check on what just landed.

Everything below was written before the work. §16 is what turned out differently
while it was being built, and it is the part to read if this file is being used to
change the code again.

Decisions taken (2026-10-11): byte-level resume through a `.part` sidecar; the
exists prompt offers Overwrite / Skip / Rename; `Resume` asks before it continues;
partial data survives the session, not the restart. For the SFV half: ask by
default with `sfv_verify = ask|always|never` to make it permanent; a failed
checksum reports and offers to re-fetch exactly the files that failed; verification
is also a menu command; CRC-32 only, no MD5.

---

## 1. What is true today

| Fact | Where | Why it matters |
|---|---|---|
| An aborted download deletes its own partial file | `net_download()` ends `if (!ok) remove(local);` | There is nothing to resume from. It also means a crash leaves a truncated file at the **final** name, which is worse than the abort case. |
| A transfer row cannot restart itself | `BgTransfer` keeps `src`/`dst` display strings only; `OpPlan` and `NetSite` are handed to the thread by value and dropped | Resume needs the plan and the login the transfer started with. |
| The exists prompt already runs in both directions | `plan_existing()` lists the destination once for an upload, stats each target for a download | The plumbing is there; only the answer set is missing. |
| The prompt is Yes/No | `render_confirm()`, `CONF_OVERWRITE`, `CONF_OVERWRITE_MOVE`, `CONF_OVERWRITE_PASTE`, `CONF_OVERWRITE_EXTRACT` | Yes overwrites everything, No does nothing. There is no middle. |
| The FTP fixture has no `REST` | `tests/ftp_server.py` implements USER/PASS, PASV, CWD/PWD, LIST, NLST, MLSD, MLST, RETR, STOR, MKD, RMD, DELE | Byte-level resume cannot be tested until the fixture answers `REST`. |

## 2. The partial file

`net_download` writes `<local>.part` and renames it to `<local>` when the transfer
is good. Rename on the same filesystem is atomic, so the final name is either the
complete file or it is not there.

- **Abort or failure leaves the `.part`.** That is the resume material.
- **Restart deletes it first.** A restart that appends to a partial file is a
  corrupt file.
- **It is visible in the pane.** A half-downloaded file that the file manager
  hides is a file the user finds later with no explanation. The transfer row names
  it, and the row's numbers say how much of it is real.
- **Session only.** Quitting keeps the `.part` on disk and drops the row. Starting
  the same transfer again picks the `.part` up, because picking it up is what a
  `.part` file is for; nothing is offered at startup.

`net_upload` needs no sidecar: the source is already a complete local file and the
partial one is on the server.

## 3. Transport (`src/remote.{h,cpp}`)

`net_download` and `net_upload` gain a resume offset and an out-param for what
actually arrived:

```cpp
bool net_download(const NetSite&, const std::string& rpath, const char* local,
                  fs_progress_fn cb, void* user, std::string* err,
                  long long resume_from = 0, long long* got = nullptr);
```

- **Download.** `resume_from > 0` opens the `.part` with `"ab"` and sets
  `CURLOPT_RESUME_FROM_LARGE`, which makes curl send `REST <offset>` before `RETR`.
  `resume_from == 0` is today's `"wb"`.
- **Upload.** The offset is the size the server reports for the target
  (`net_stat`, which the tree code already calls). curl's FTP resume is an append:
  it sends `APPE` rather than `REST` + `STOR`, and it reads the **local source** from
  the offset. Two things about that turned out to matter, and both are in §3a.
- **What resume assumes.** The first `resume_from` bytes of the source are still
  the bytes that produced the partial file. For a download that cannot be verified
  without re-reading the remote file; the plan accepts the assumption and says it
  in §9.
- `net_download_tree` / `net_upload_tree` pass the offset down per entry: a tree
  that died on the fourth file resumes the fourth and re-runs the fifth.

## 3a. What the implementation found (phases 1 and 2, built)

Three things the plan had wrong, found by running them against the fixture:

- **`CURLOPT_INFILESIZE_LARGE` is the whole local size, not the remainder.** curl
  subtracts the resume offset itself. Telling it the remainder *and* the offset made
  it compute "send zero bytes": it logged in, sent `TYPE I`, sent no transfer
  command at all, and reported `CURLE_OK`. The upload half then looked like a success
  while the server still held half a file. That is why `net_upload` checks the
  remote size after an append: an append that did not land is otherwise
  indistinguishable from one that did.
- **With our own read callback curl cannot seek the source**, so `net_upload` seeks
  its own `FILE*` to the offset before handing it over.
- **The download size check is gone.** The plan's guard - compare the remote size
  with the `.part` afterwards - turned out to be unreachable and wrong.
  - curl asks the size, subtracts the offset it put in `REST`, and stops reading at
    that many bytes. An honest resume lands on the remote size by construction.
  - A server that refuses `REST` fails in curl (`Could not use REST`). A resume asked
    for past the end of the file fails in curl too, with the server's own words:
    `Offset (5000) was beyond file size (12)`. Both leave the partial untouched,
    which is right: nothing is wrong with those bytes.
  - A server that answers `REST` and ignores it, with `SIZE` available, produces a
    file of exactly the expected length containing the wrong bytes. No length check
    can see that, because curl stopped reading at the number the lie added up to.
  - The one shape the guard could reach is a remote file that changed size between
    the listing and the transfer - and there the file curl produced is complete, so
    deleting it would throw away a good download.

  Whether the bytes are the *right* bytes is a checksum question. That is the whole
  argument for part two of this plan, and it is now the only integrity check a
  resumed download gets.

## 4. The record keeps its plan (`src/panes.h`)

```cpp
struct BgTransfer {
    ...
    OpPlan   plan;                  // fixed before the worker starts
    NetSite  site;                  // the login it started with, by value
    std::atomic<long long> resumed_from{0};   // for the row's numbers
};
```

`bg_run_remote(t, site, plan)` becomes `bg_run_remote(t)` reading both from the
record. The site-by-value rule stays for the same reason it exists now: the user
can edit or delete the saved site mid-transfer.

`OpPlan` is copied, not referenced — `run_op_plan()` clears `app->op_plan` the
moment the transfer starts, and a resume has to outlive that.

## 5. The row's Resume button (`render_transfers`)

A row that is `BG_ABORTED` or `BG_FAILED` gets `Resume` beside `clear`. Pressing it
opens the choice dialog, because the target exists by definition:

```
Download to /home/adam/dl stopped with 12.4M of 48.9M done. Continue from what is
already there, throw it away and run again, or leave it?

[Resume]  [Start over]  [Skip]        (press R / S / Esc)
```

- **Resume** — continue from the `.part` (download) or from the size the server
  reports (upload).
- **Start over** — clear the partial, run the plan from zero.
- **Skip** — leave the row exactly as it is. The `.part` stays on disk and the row
  keeps its reason.

A row whose transfer failed for a reason that is not an abort (no login, refused
store) shows the reason and still offers Resume, because the reason may already be
fixed — `Ctrl+F` edits the site, Resume tries again with the plan it already has.

### What phase 3 actually built

- `BgTransfer` carries `plan`, `site` and `resume`, and `resumable` says whether the
  row can be started again at all. `bg_run_remote(t)` reads all of it out of the
  record, so the worker's signature lost its two side arguments.
- **The row is reused, not replaced.** The plan proposed a new record; the
  implementation joins the finished worker, resets `done` / `total` / `files_*` /
  `error` / `ok_names` / `t_end_ms`, clears `settled` so the finish is reported
  again, and starts a thread on the same record. One job keeps one row, and the
  publish rule still holds: a state that is not `BG_RUNNING` means the worker has
  run to completion, which is the only kind of worker this joins.
- The dialog is `CHOICE_RESUME` in a new `ConfirmChoice` enum, drawn by
  `render_choice()` next to `render_confirm()`, and its answer is an index in
  `choice_pick`, consumed in `app_panes_run_pending()` — a thread is not started
  inside a draw call, which is the same reason a confirm's Yes is consumed there.
- Keys: `R` resumes, `S` starts over, `Esc` (or `N`) skips, Enter is the recommended
  answer. `main.cpp` routes the whole keyboard to the pane handler while it is up,
  the way it does for a confirm, so a stray letter is an answer and not a command
  to the panes behind it.
- A row the question is about is not retired or cleared underneath it:
  `bg_reap()` and `bg_retire_oldest_finished()` skip it, and `bg_stop_all()` drops
  the question when the rows go away.

## 6. Overwrite / Skip / Rename

`AppConfirm` keeps its five values and gains an answer:

```cpp
enum ConfirmChoice { CH_NONE = 0, CH_OVERWRITE, CH_SKIP, CH_RENAME };
ConfirmChoice confirm_choice;   // set by the dialog, read by the runner
```

`render_confirm()` draws three buttons and keeps the key rule people already have:
**Y overwrites, N skips, R renames**. The text names the count and the directory,
as it does now.

- **Skip** leaves the existing target alone and counts itself: the row and the
  status line say `3 of 5  2 skipped`. A plan where every name was skipped is not a
  failure and does not say "failed".
- **Rename** writes beside the existing file: `report.pdf` → `report (2).pdf`,
  then `(3)`, until the name is free. The number goes before the extension, and a
  name with no extension - or a directory - takes it at the end.
- **Applies to the whole batch**, which is what the count in the prompt means.
- `CONF_DELETE` and the other non-overwrite confirms keep their Yes/No: this is a
  choice about clobbering, not a choice about deleting.

### What phase 4 actually built

- The decision travels in the plan: `OpPlan::Overwrite` (`OW_ASK`, `OW_OVERWRITE`,
  `OW_SKIP`, `OW_RENAME`), set by the confirm and read by every runner - the local
  copy, the paste, the extractor, and the network worker. A choice only one of them
  honoured would be a choice that changes shape on the way.
- `pending_confirm_yes` is gone. A confirm now records `pending_confirm_answer`:
  `CONF_ANS_YES` / `CONF_ANS_NO` for the delete confirm, and `CONF_ANS_OVERWRITE` /
  `CONF_ANS_SKIP` / `CONF_ANS_RENAME` / `CONF_ANS_CANCEL` for an overwrite one.
  **Esc is not an answer** - it cancels the operation outright, which is what No
  used to mean and what a user reaches for when the confirm turns out to be the
  wrong question.
- **Skip is decided by filtering the plan, not by asking the tools to behave.**
  zip, tar and 7z each spell "do not overwrite" differently (`-n`,
  `--skip-existing-files` against `--no-overwrite`, `-aos`), and the app's own
  extraction path already names the members it wants. Not asking for the member
  whose target exists is the same answer with no per-tool flag and no new test
  surface per tool.
- **Rename is not offered for archive extraction**, and the dialog says so by not
  drawing the button: the names inside an archive are the extractor's to write,
  where the archive says they go, and "write beside" is not something it can be
  asked for a subtree.
- `rename_beside(dst, taken)` takes an optional set of names the destination already
  holds. For a local destination it is null and each candidate is stat'ed - cheaper
  than listing the directory, and it cannot be stale. For a destination on a server
  the set comes from one `net_list` of that directory, because the protocol offers no
  per-name lookup. `plan_existing()` did not have to return the colliding names for
  that: the worker asks once, lazily, and only when the answer was skip or rename.
- A skip is reported and is not a failure. `BgTransfer::skipped_names` joins
  `ok_names` before the state that publishes them, and the status lines read
  `Copied 3 items to /x  2 skipped` or `Nothing transferred to /x  5 targets left
  alone`.

## 7. Fixture and tests

`tests/ftp_server.py` gains `REST` (remember the offset, apply it as a seek for
`RETR` and as a seek-and-truncate for `STOR`, clear it after the transfer) and
`SIZE`. A flag makes it refuse `REST` so the "server did not resume" path is a test
and not a hope.

- `test_remote`: a resumed download is byte-identical to a full one; a refused
  `REST` leaves no corrupt file and says why; a resumed upload appends at the
  offset the server reported; a `.part` is renamed on success and survives an abort.
- `test_panes`: the row keeps its plan after an abort; `Resume` continues from the
  `.part` size and the row's numbers start from there, not from zero; `Restart`
  deletes the `.part`; `Skip` retires the row and keeps the partial; the exists
  choice overwrites, skips with the count, and renames to `name (2).ext`; a rename
  collision at `(2)` lands on `(3)`.

## 8. Phases

1. `.part` sidecar + rename in `net_download`, and the size check. Lands the
   crash-leaves-a-truncated-file fix on its own.
2. `REST`/`SIZE` in the fixture, the resume options in the transport, `test_remote`
   coverage.
3. `BgTransfer` carries `plan` + `site`; `Resume` button and the choice dialog.
4. Overwrite / Skip / Rename across the local confirms and the remote plans.
5. `src/sfv.{h,cpp}`: the CRC-32 table and the parser, tested on their own before
   anything calls them.
6. `SfvJob` on a worker thread, the report window, the ask in `bg_reap()`,
   `sfv_verify` in the config.
7. `Commands → Verify SFV`, and the re-fetch that reuses the plan from phase 3.
8. README and `FEATURES_PLAN.md`.

## 9. Out of scope, and what to be honest about

- No resume across a restart of the app. The rows are session state; the `.part`
  files are the only thing that outlives it, and nothing offers them at startup.
- No resume for archive extraction or a local copy. A local copy that dies at 90%
  is a filesystem problem, not a transport one, and a `.part` in every local copy
  would be noise.
- No parallel files inside one plan. A plan is still sequential, so a resume
  restarts the file that was in flight and re-runs the ones after it.
- **A resumed download trusts the bytes it already has.** Verifying them means
  re-reading the remote file, which is the transfer all over again. The size check
  catches the common failure (a server that ignored `REST`) and not the rare one
  (a remote file that changed mid-transfer).
- **A resumed upload trusts the size the server reports.** If the remote partial
  came from a different source file, appending produces a mixture. The dialog says
  the offset and the name it is appending to, which is the most a file manager can
  do about that.
- `REST` is not universal. Some servers disable it; some answer 350 and then send
  the whole file. Both land on the same message and a restart.

---

## Part two — the `.sfv` that arrives with the download

## 10. What it is

An SFV is a text file that lists a CRC-32 for every file in a set:

```text
; generated by QuickSFV on 2024-05-02 at 19:14
pack.part1.rar 3f2a91c4
pack.part2.rar 8b10dd07
big.tar.gz     00c7fe12
```

It is what a scene or a Usenet download carries to answer "did everything come
through intact", and it is the one verification a file manager can do without a
trust story: the numbers came with the files, and both are untrusted. It answers
"is this folder internally consistent", not "is this the real file".

## 11. The parser and the CRC (`src/sfv.{h,cpp}`)

CRC-32 is the reflected 32-bit polynomial `0xEDB88320` — the same one zlib uses —
computed over 64 KB chunks with a 256-entry table in the source. `find_package`
gains nothing here: the app does not link zlib today (only libcurl pulls it in, and
the no-libcurl build configuration has to keep working), and a checksum this small
should not decide whether the app builds.

Parsing rules, because real SFV files are messy:

- `;` and `#` comments, blank lines, trailing spaces, CRLF line endings.
- The name is everything before the **last** run of whitespace, and the value is
  that last token: names with spaces in them are legal and common.
- Some writers put the CRC first. If the last token is eight hex digits, use it;
  otherwise if the *first* token is eight hex digits and the rest is not, swap.
- `\` in a name becomes `/`. Windows writers produce `folder\file.rar`.
- The `.sfv` file never verifies itself.
- A line whose value is not eight hex digits is a malformed line: counted, reported
  once in the report header, and not treated as a failure of the file it names.

The check value for `"123456789"` is `0xCBF43926`. That is the test the table is
written against, before anything else in this part is built.

## 12. The ask, and the setting

`bg_reap()` is where a finished download already refreshes its destination and
reports the result. That is where the offer belongs:

```cpp
// a download that landed something, was not aborted, and has a local destination
if (t->download && t->ok_names && !t->abort.load())
    sfv_offer_after_download(app, t);   // CONF_SFV_VERIFY, or start the job directly
```

```text
pack.sfv is in /downloads/pack.
Verify 3 files against it?

[Verify]  [Not now]  [Always]      (press V / N / A)
```

- **`sfv_verify = ask`** (default) — the dialog above.
- **`Always`** in the dialog sets it for the session and says so in the status line;
  it does not write the config file.
- **`sfv_verify = always`** in the config file starts the job with no dialog, and
  the result goes to the status line: `12 files  11 ok  1 bad`.
- **`sfv_verify = never`** — nothing happens after a download.

Which `.sfv` when the folder has more than one: the one whose name matches the base
of what was transferred (`pack.sfv` for `pack.part1.rar`), then the only one there,
then a list to pick from. The file itself is never verified against itself.

## 13. The job and the report

Hashing a 4 GB file is not a frame's work, so it runs on a thread with the rule
`BgTransfer` and `QvFetch` already follow: the UI thread owns the record, the worker
writes through atomics, and the join happens before the record goes away.

```cpp
struct SfvJob {
    std::atomic<int>  state;              // RUNNING / DONE / FAILED
    std::atomic<bool> cancel;
    std::atomic<long long> done, total;   // files checked of files listed
    std::string sfv_path, dir;            // fixed before the thread starts
    std::mutex mu;                        // guards `rows`
    std::vector<SfvRow> rows;             // name, want, got, status
    std::thread worker;
};
```

It shows as a row in the transfer strip while it runs (`verify 7 of 12`), and as a
report window when it finishes:

```text
pack.sfv   3 listed   2 ok   1 bad

big.tar.gz        ok            00c7fe12
pack.part1.rar    ok            3f2a91c4
pack.part2.rar    MISMATCH      want 8b10dd07  got 4c9a1f00
missing.rar       MISSING       want 77ab3010

[Re-fetch the bad ones]   [Close]
```

- **`Re-fetch the bad ones`** builds an `OpPlan` from the failed names out of the
  original transfer's plan — the one §4 made the record keep — and hands it to
  `bg_start_remote_plan()`. It is offered only when that plan is still in the
  session; a verification run from the menu has no source to fetch from and reports
  only.
- A file the SFV does not list is not reported. The SFV says what it expects; a
  folder with extra files is not the SFV's problem, and saying so every time would
  train people to ignore the report.
- The list is capped (100 rows) with the count of what was left out, because an SFV
  for a 10 000-file set is a real thing.

## 14. The menu command

`Commands → Verify SFV` runs the same job on the focused pane's directory: the
`.sfv` there, every file it lists, no fetching involved. It is refused with a
reason in the status line when the pane is inside an archive or on a site — the
same rule the console already applies to a directory that is not a directory.

No key binding in this plan. Every plausible one (`Ctrl+Shift+V`, `Alt+V`) needs the
collision audit `FEATURES_PLAN.md` §2 does before it is claimed.

## 15. Tests

- The CRC table against `0xCBF43926`, and against a file whose bytes are known.
- Parser: comments, CRLF, a name with spaces, a CRC-first line, a `\` in a name, a
  malformed value, the `.sfv` skipping itself.
- A folder of three files and one `.sfv`: clean, one byte changed (`MISMATCH` with
  both numbers), one file deleted (`MISSING`), one file not listed (not reported).
- `sfv_verify` in all three modes after a download that used the FTP fixture.
- `Re-fetch the bad ones` produces a plan holding exactly the failed names, and the
  transfer that follows moves exactly those.
- The menu command on a directory with no `.sfv` says so instead of reporting
  success over nothing.

## 16. What changed while it was built

**Upload resume is `APPE`, not `REST` + `STOR`.** The plan assumed a resumed upload
would seek the remote file. curl does not do that: given `-C` on an upload it sends
`APPE` and the server appends. The fixture had to learn `APPE` before the C++ path
could be tested at all, and the check that a resumed upload is correct is a post
transfer `net_stat()` against the source length rather than a protocol guarantee.

**`--rest-lies` became `--no-size`.** The plan wanted a server that accepts `REST`,
ignores the offset, and gets caught by the length guard. It cannot be tested that
way. When the server also answers `SIZE`, curl asks for exactly the remainder, the
server sends from the beginning of the file, and the finished length is right - so the
guard passes and the bytes are wrong. A length check cannot see a file of the correct
length with the wrong content. What is testable is a server that will not answer
`SIZE`: curl refuses the resume before transferring, the `.part` stays intact, and the
transfer is reported as needing to start again. The untestable case is the argument
for §11's checksums rather than an argument for the guard.

**The guard says what it checked.** A resumed transfer that finishes at a length the
server does not agree with removes the `.part` and reports that the partial does not
match the file on the server. Keeping the partial would offer the same broken bytes to
the next attempt.

**`sfv_retry_plan()` is a function, not part of the window.** "Re-fetch the bad ones"
started as inline code in the report window, where it could not be tested. As a
function the rule - only the failed names, only out of the transfer that fetched them,
`OW_OVERWRITE` because the bad copy is what gets replaced, and no plan at all for a
verification that had no source - is checked without a display.

**The report window opens only when there is something to report.** A run where every
file matched puts `N listed  N ok  0 bad` in the status line and opens nothing. A
report that is always correct is a report people learn to click past.

**Which `.sfv` a folder means is a longest common prefix.** `pack.part1.rar` beside
`pack.sfv` and `a-set-of-files.sfv` picks `pack.sfv` by matching the transferred name
against each candidate; with nothing to match, a single file is that file and several
are the shortest name.

**Two fixture rules.** `.sfv` fixtures are written with `fwrite` and not through the
shell: Linux caps one argument at 128 KiB, and the multi-chunk CRC fixture is bigger
than that, so a `printf` fixture was being silently truncated. And each FTP fixture
now writes its own server log - the modes share a root directory, and one log name
meant the last fixture truncated the first one's protocol transcript.

**Tree resume recurses through the tree function.** `net_download_tree()` initially
called `net_download()` for directory children, which asked the transport to fetch a
directory as a file. Each file resumes from its own `.part` size; each upload resumes
from the size the remote file already has.
