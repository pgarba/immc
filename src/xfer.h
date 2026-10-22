#ifndef MC_XFER_H
#define MC_XFER_H

// The transfer queue: every file a copy to or from a site has to move, worked
// through by a pool of threads per site.
//
// A copy of a folder used to be one thread walking the tree and fetching one
// file after another over one connection. A server that gives each connection a
// fraction of the line - most of them do - is then as slow as one connection no
// matter how much line there is. So a transfer is now planned first (the tree is
// walked and every file it holds becomes an item with its size), the items go on
// the site's queue behind whatever is already waiting there, and as many threads
// as the site allows take items from the front, each with its own connection.
// libcurl's handle cache is per thread (remote.cpp), so a thread is a connection.
//
// A job is the thing the user started: one row in the transfer strip. Its items
// carry a group, which is the index of the top-level name it came from, so the
// job can tell which names arrived whole - a move removes only those sources.
//
// Threading: the queue and the pools are guarded by one mutex inside xfer.cpp.
// A job's counters are atomics, written by whichever thread is moving one of its
// files and read by the frame that draws it. A worker never touches anything but
// the job and the pool, so the UI's own state stays the UI thread's.

#include "remote.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

enum { XFER_MIN_THREADS = 1, XFER_MAX_THREADS = 10 };
int xfer_clamp_threads(int n);

struct XferJob {
    std::atomic<bool>      abort{false};      // the UI's request; workers poll it
    std::atomic<long long> done{0};           // bytes the job has, resumed ones included
    std::atomic<long long> total{0};          // bytes the job's files add up to
    std::atomic<int>       files_done{0};     // files that arrived whole
    std::atomic<int>       files_total{0};

    // Completion, guarded by `mu`: how many items are still queued or moving, which
    // groups lost a file, and the first reason one was lost.
    std::mutex              mu;
    std::condition_variable cv;
    int                     pending = 0;
    std::vector<char>       group_failed;
    std::string             first_err;

    // Ready for a run over `groups` top-level names. Not while items are pending.
    void begin(int groups);
    bool group_ok(int g);
};

struct XferItem {
    XferJob*    job = nullptr;
    int         group = 0;
    bool        download = true;
    NetSite     site;                  // the login, by value: the saved one can change
    std::string remote;                // path on the server
    std::string local;                 // path on this disk
    long long   size = 0;              // what the listing or stat said
    bool        resume = false;        // continue from the .part, or from the server's size
};

// Plan one top-level entry. A download walks the server's tree and makes every
// local directory now, so an empty folder arrives too; an upload walks the local
// tree and makes every remote directory now, so no two threads race to create the
// same one. Files become items; their sizes go into the job's total.
bool xfer_plan_download(const NetSite& site, const std::string& rpath, const std::string& local,
                        XferJob* job, int group, bool resume, std::vector<XferItem>* out,
                        std::string* err);
bool xfer_plan_upload(const NetSite& site, const std::string& local, const std::string& rpath,
                      XferJob* job, int group, bool resume, std::vector<XferItem>* out,
                      std::string* err);

// RAR volumes in the order the archive reads them. A set is either the old
// naming - name.rar, then name.r00 to .r99, then .s00 to .s99, .t00 and on - or
// the new one, name.part1.rar, name.part2.rar. `index` is the volume's place in
// its set: .rar is 0, .r00 is 1, .s00 is 101; partN.rar is N. False for a file
// that is not a volume. Case does not matter; `set` comes back in lower case.
bool xfer_rar_volume(const std::string& name, std::string* set, int* index);

// Reorder a plan so every folder's RAR sets go out volume by volume, set after
// set. Folders keep their order, and in each one the files that are not volumes
// (the .sfv, the .nfo) go first, in the order the listing gave them. A site's
// "transfer RAR volumes in order" setting turns this on; with several threads
// the volumes still start in order, so the first ones are the first to arrive.
void xfer_order_rar(std::vector<XferItem>* items);

// Put the items at the back of their site's queue and make sure up to `threads`
// workers are taking from it, then block until every one of them is done. An
// abort takes the job's items that have not started out of the queue at once;
// the ones being moved stop at the transport's next progress call.
void xfer_run(XferJob* job, const NetSite& site, int threads, std::vector<XferItem> items);

// What the workers are doing right now, for the strip under the panes. `bytes`
// only ever grows for a given id, so a rate is the difference between two looks.
struct XferThreadStat {
    uint64_t    id = 0;
    std::string site;                  // the site root the thread works for
    std::string file;                  // the file it is moving, "" when idle
    long long   bytes = 0;             // moved by this thread since it started
};
void xfer_snapshot(std::vector<XferThreadStat>* threads, int* queued);

// Per-thread speed over the last few seconds: the "current" speed a transfer
// window shows, rather than an average since the start that a stall at the
// beginning drags down for the rest of the run. Fed one snapshot per frame.
struct XferMeter {
    double window = 3.0;               // seconds of history a rate is taken over
    std::map<uint64_t, std::deque<std::pair<double, long long>>> hist;
    std::map<uint64_t, double> rate;   // bytes per second, per thread id

    // Returns the sum of the threads' rates.
    double update(double now, const std::vector<XferThreadStat>& threads);
};

// Stop every worker and wait for it. Every job has to have been aborted and its
// xfer_run() returned first; this is what Quit does before net_shutdown().
void xfer_shutdown();

#endif
