// The transfer queue. See xfer.h for why a copy is planned into files and the
// files are shared out between threads.

#include "xfer.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

int xfer_clamp_threads(int n) {
    if (n < XFER_MIN_THREADS) return XFER_MIN_THREADS;
    if (n > XFER_MAX_THREADS) return XFER_MAX_THREADS;
    return n;
}

void XferJob::begin(int groups) {
    std::lock_guard<std::mutex> lk(mu);
    pending = 0;
    group_failed.assign(groups > 0 ? (size_t)groups : 0, 0);
    first_err.clear();
    done = 0;
    total = 0;
    files_done = 0;
    files_total = 0;
}

bool XferJob::group_ok(int g) {
    std::lock_guard<std::mutex> lk(mu);
    return g >= 0 && g < (int)group_failed.size() && !group_failed[g];
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

// The walk polls the abort flag between directories: a tree of ten thousand
// folders is minutes of listings, and Abort has to mean something during them.
static bool aborted(const XferJob* job) { return job && job->abort.load(); }

static void add_item(XferJob* job, int group, bool download, const NetSite& site,
                     const std::string& remote, const std::string& local, long long size,
                     bool resume, std::vector<XferItem>* out) {
    XferItem it;
    it.job = job;
    it.group = group;
    it.download = download;
    it.site = site;
    it.remote = remote;
    it.local = local;
    it.size = size > 0 ? size : 0;
    it.resume = resume;
    out->push_back(it);
    if (job) {
        job->total += it.size;
        job->files_total++;
    }
}

static bool walk_remote(const NetSite& site, const std::string& rpath, const std::string& local,
                        XferJob* job, int group, bool resume, std::vector<XferItem>* out,
                        std::string* err) {
    if (mkdir(local.c_str(), 0755) != 0 && errno != EEXIST) {
        if (err) *err = local + ": " + strerror(errno);
        return false;
    }
    std::vector<FileEntry> rows;
    if (!net_list(site, rpath, &rows, err)) return false;
    for (const FileEntry& e : rows) {
        if (aborted(job)) { if (err) *err = "aborted"; return false; }
        std::string child = rpath.empty() ? e.name : rpath + "/" + e.name;
        std::string child_local = local + "/" + e.name;
        // The listing already says which entries are folders, so the walk does not
        // ask the server the same question again per entry.
        if (S_ISDIR(e.mode)) {
            if (!walk_remote(site, child, child_local, job, group, resume, out, err)) return false;
        } else {
            add_item(job, group, true, site, child, child_local, (long long)e.size, resume, out);
        }
    }
    return true;
}

bool xfer_plan_download(const NetSite& site, const std::string& rpath, const std::string& local,
                        XferJob* job, int group, bool resume, std::vector<XferItem>* out,
                        std::string* err) {
    FileEntry me;
    std::string stat_err;
    // Not a folder, or not something the parent's listing names: fetch it as a
    // file and let the transfer itself say what is wrong with it.
    if (!net_stat(site, rpath, &me, &stat_err) || !S_ISDIR(me.mode)) {
        long long size = stat_err.empty() ? (long long)me.size : 0;
        add_item(job, group, true, site, rpath, local, size, resume, out);
        return true;
    }
    return walk_remote(site, rpath, local, job, group, resume, out, err);
}

static bool walk_local(const NetSite& site, const std::string& local, const std::string& rpath,
                       XferJob* job, int group, bool resume, std::vector<XferItem>* out,
                       std::string* err) {
    // An existing folder answers MKD with an error, and uploading over a folder
    // that is already there is an overwrite, not a failure, so the answer is not
    // read. A folder that really could not be made fails the files that go in it,
    // with the server's reason.
    std::string ignored;
    net_mkdir(site, rpath, &ignored);
    DIR* d = opendir(local.c_str());
    if (!d) {
        if (err) *err = local + ": " + strerror(errno);
        return false;
    }
    bool ok = true;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (aborted(job)) { if (err) *err = "aborted"; ok = false; break; }
        std::string child_local = local + "/" + de->d_name;
        std::string child_remote = rpath.empty() ? de->d_name : rpath + "/" + de->d_name;
        struct stat st;
        if (stat(child_local.c_str(), &st) != 0) {
            if (err) *err = child_local + ": " + strerror(errno);
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!walk_local(site, child_local, child_remote, job, group, resume, out, err)) {
                ok = false;
                break;
            }
        } else {
            add_item(job, group, false, site, child_remote, child_local, (long long)st.st_size,
                     resume, out);
        }
    }
    closedir(d);
    return ok;
}

bool xfer_plan_upload(const NetSite& site, const std::string& local, const std::string& rpath,
                      XferJob* job, int group, bool resume, std::vector<XferItem>* out,
                      std::string* err) {
    struct stat st;
    if (stat(local.c_str(), &st) != 0) {
        if (err) *err = local + ": " + strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        add_item(job, group, false, site, rpath, local, (long long)st.st_size, resume, out);
        return true;
    }
    return walk_local(site, local, rpath, job, group, resume, out, err);
}

// ---------------------------------------------------------------------------
// RAR volume order
// ---------------------------------------------------------------------------

static bool all_digits(const std::string& s, size_t from, size_t to) {
    if (from >= to) return false;
    for (size_t i = from; i < to; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

bool xfer_rar_volume(const std::string& name, std::string* set, int* index) {
    std::string low = name;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    size_t n = low.size();
    if (n > 4 && !low.compare(n - 4, 4, ".rar")) {
        std::string base = low.substr(0, n - 4);
        // name.part07.rar: the new naming, numbered from one.
        size_t dot = base.rfind(".part");
        if (dot != std::string::npos && all_digits(base, dot + 5, base.size())) {
            if (set) *set = base.substr(0, dot);
            if (index) *index = atoi(base.c_str() + dot + 5);
            return true;
        }
        if (set) *set = base;
        if (index) *index = 0;
        return true;
    }
    // name.r00: the old naming. After .r99 comes .s00, then .t00, so the letter
    // is the hundreds.
    if (n > 4 && low[n - 4] == '.' && low[n - 3] >= 'r' && low[n - 3] <= 'z' &&
        all_digits(low, n - 2, n)) {
        if (set) *set = low.substr(0, n - 4);
        if (index) *index = (low[n - 3] - 'r') * 100 + atoi(low.c_str() + n - 2) + 1;
        return true;
    }
    return false;
}

void xfer_order_rar(std::vector<XferItem>* items) {
    struct Key {
        size_t dir_rank;     // folders in the order the plan reached them
        int    volume;       // 0: not a volume, 1: a volume
        std::string set;
        int    index;
        size_t pos;          // where it was, which settles every tie
    };
    std::map<std::string, size_t> dir_rank;
    std::vector<Key> keys;
    keys.reserve(items->size());
    for (size_t i = 0; i < items->size(); i++) {
        const XferItem& it = (*items)[i];
        // The source's name: what the listing or the disk called it.
        const std::string& path = it.download ? it.remote : it.local;
        size_t slash = path.find_last_of('/');
        std::string dir = slash == std::string::npos ? "" : path.substr(0, slash);
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        auto found = dir_rank.find(dir);
        size_t rank = found != dir_rank.end() ? found->second : dir_rank.size();
        if (found == dir_rank.end()) dir_rank[dir] = rank;
        Key k = { rank, 0, "", 0, i };
        if (xfer_rar_volume(name, &k.set, &k.index)) k.volume = 1;
        keys.push_back(k);
    }
    std::vector<size_t> order(items->size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&keys](size_t a, size_t b) {
        const Key& x = keys[a];
        const Key& y = keys[b];
        if (x.dir_rank != y.dir_rank) return x.dir_rank < y.dir_rank;
        if (x.volume != y.volume) return x.volume < y.volume;
        if (x.volume) {
            if (x.set != y.set) return x.set < y.set;
            if (x.index != y.index) return x.index < y.index;
        }
        return x.pos < y.pos;
    });
    std::vector<XferItem> sorted;
    sorted.reserve(items->size());
    for (size_t i : order) sorted.push_back(std::move((*items)[i]));
    items->swap(sorted);
}

// ---------------------------------------------------------------------------
// The pools
// ---------------------------------------------------------------------------

namespace {

struct Worker {
    uint64_t id = 0;
    std::thread th;
    std::atomic<long long> bytes{0};
    bool exited = false;               // under g_mu: the loop has returned
    std::string file;                  // under g_mu
};

struct Pool {
    std::string root;
    int want = 2;                      // threads the site allows, from the last job
    int live = 0;                      // workers whose loop has not returned
    std::deque<XferItem> q;
    std::vector<std::unique_ptr<Worker>> workers;
};

std::mutex g_mu;
std::condition_variable g_cv;          // items arrived, or stop
std::vector<std::unique_ptr<Pool>> g_pools;
bool g_stop = false;
uint64_t g_next_id = 1;

// How long an idle worker waits for more before it ends. Long enough that a
// queue fed by several F5s in a row keeps its connections, short enough that a
// finished queue does not hold the server's connection slots.
const auto kLinger = std::chrono::seconds(2);

// One item has been dealt with. The notify is sent while the job's lock is held:
// the job's owner is waiting for `pending` to reach zero and frees the job right
// after, so nothing may touch the job once the lock is let go.
void finish(const XferItem& it, bool ok, const std::string& err) {
    XferJob* j = it.job;
    std::lock_guard<std::mutex> lk(j->mu);
    if (ok) {
        j->files_done++;
    } else {
        if (it.group >= 0 && it.group < (int)j->group_failed.size()) j->group_failed[it.group] = 1;
        if (j->first_err.empty()) j->first_err = err.empty() ? "transfer failed" : err;
    }
    j->pending--;
    j->cv.notify_all();
}

struct ItemProgress {
    XferJob* job;
    Worker* worker;
    long long last;                    // bytes this transfer call had reported
};

// The transport reports the bytes of the call in flight; the job and the thread
// want the growth since the last report.
bool item_cb(long long done, long long, int, int, void* user) {
    ItemProgress* p = (ItemProgress*)user;
    if (done < p->last) p->last = 0;   // a new call started counting from zero
    long long delta = done - p->last;
    p->last = done;
    if (delta > 0) {
        p->job->done += delta;
        p->worker->bytes += delta;
    }
    return !p->job->abort.load();
}

bool run_item(const XferItem& it, Worker* w, std::string* err) {
    ItemProgress prog = { it.job, w, 0 };
    long long from = 0;
    bool ok;
    if (it.download) {
        if (it.resume) from = net_part_size(it.local.c_str());
        if (from > 0) it.job->done += from;
        ok = net_download(it.site, it.remote, it.local.c_str(), item_cb, &prog, err, from, nullptr);
    } else {
        if (it.resume) {
            FileEntry there;
            std::string serr;
            if (net_stat(it.site, it.remote, &there, &serr) && S_ISREG(there.mode) &&
                (long long)there.size <= it.size)
                from = (long long)there.size;
        }
        if (from > 0) it.job->done += from;
        ok = net_upload(it.site, it.remote, it.local.c_str(), item_cb, &prog, err, from);
    }
    // A transport that did not report every byte still delivered the file: the
    // job's count is brought up to the file's size so the bar reaches the end.
    if (ok) {
        long long missing = it.size - from - prog.last;
        if (missing > 0) it.job->done += missing;
    }
    return ok;
}

void worker_main(Pool* pool, Worker* w) {
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        // A site whose thread count was lowered sheds workers between files.
        if (g_stop || pool->live > pool->want) break;
        if (pool->q.empty()) {
            if (!g_cv.wait_for(lk, kLinger, [pool] { return g_stop || !pool->q.empty(); })) break;
            continue;
        }
        XferItem it = pool->q.front();
        pool->q.pop_front();
        if (it.job->abort.load()) {
            lk.unlock();
            finish(it, false, "aborted");
            lk.lock();
            continue;
        }
        size_t slash = it.remote.find_last_of('/');
        w->file = slash == std::string::npos ? it.remote : it.remote.substr(slash + 1);
        lk.unlock();
        std::string err;
        bool ok = run_item(it, w, &err);
        finish(it, ok, err);
        lk.lock();
        w->file.clear();
    }
    pool->live--;
    w->exited = true;
    // The connection this thread held is released as the thread ends: the handle
    // cache is thread_local.
}

Pool* pool_for(const std::string& root) {
    for (std::unique_ptr<Pool>& p : g_pools)
        if (p->root == root) return p.get();
    g_pools.push_back(std::make_unique<Pool>());
    g_pools.back()->root = root;
    return g_pools.back().get();
}

// Take the threads of workers that have ended out of the pools, to be joined
// once the lock is let go: joining waits for the thread's connection to close,
// and the frame drawing the strip must not wait on that.
void collect_exited(std::vector<std::thread>* out) {
    for (std::unique_ptr<Pool>& p : g_pools) {
        for (size_t i = 0; i < p->workers.size();) {
            if (p->workers[i]->exited) {
                out->push_back(std::move(p->workers[i]->th));
                p->workers.erase(p->workers.begin() + (long)i);
            } else {
                i++;
            }
        }
    }
}

void join_all(std::vector<std::thread>& threads) {
    for (std::thread& t : threads)
        if (t.joinable()) t.join();
}

// An aborted job's items that no worker has picked up yet: out of the queue and
// counted as failed now, rather than waiting for a worker to come free and do it.
void cancel_queued(XferJob* job) {
    std::vector<XferItem> dropped;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (std::unique_ptr<Pool>& p : g_pools) {
            for (auto i = p->q.begin(); i != p->q.end();) {
                if (i->job == job) { dropped.push_back(*i); i = p->q.erase(i); }
                else ++i;
            }
        }
    }
    for (const XferItem& it : dropped) finish(it, false, "aborted");
}

} // namespace

void xfer_run(XferJob* job, const NetSite& site, int threads, std::vector<XferItem> items) {
    if (items.empty()) return;
    {
        std::lock_guard<std::mutex> jl(job->mu);
        job->pending += (int)items.size();
    }
    std::vector<std::thread> reap;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        collect_exited(&reap);
        Pool* pool = pool_for(net_site_root(site));
        pool->want = xfer_clamp_threads(threads);
        for (XferItem& it : items) pool->q.push_back(std::move(it));
        while (pool->live < pool->want && pool->live < (int)pool->q.size()) {
            std::unique_ptr<Worker> w = std::make_unique<Worker>();
            w->id = g_next_id++;
            Worker* wp = w.get();
            pool->live++;
            pool->workers.push_back(std::move(w));
            wp->th = std::thread(worker_main, pool, wp);
        }
    }
    g_cv.notify_all();
    join_all(reap);

    std::unique_lock<std::mutex> jl(job->mu);
    while (job->pending > 0) {
        job->cv.wait_for(jl, std::chrono::milliseconds(100));
        if (job->pending > 0 && job->abort.load()) {
            jl.unlock();
            cancel_queued(job);
            jl.lock();
        }
    }
}

void xfer_snapshot(std::vector<XferThreadStat>* threads, int* queued) {
    threads->clear();
    int q = 0;
    std::vector<std::thread> reap;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        collect_exited(&reap);
        for (const std::unique_ptr<Pool>& p : g_pools) {
            q += (int)p->q.size();
            for (const std::unique_ptr<Worker>& w : p->workers) {
                XferThreadStat s;
                s.id = w->id;
                s.site = p->root;
                s.file = w->file;
                s.bytes = w->bytes.load();
                threads->push_back(s);
            }
        }
    }
    // A worker that has ended has nothing left to do but close its connection, so
    // this join is short; it is still done outside the lock.
    join_all(reap);
    if (queued) *queued = q;
}

double XferMeter::update(double now, const std::vector<XferThreadStat>& threads) {
    std::map<uint64_t, std::deque<std::pair<double, long long>>> next;
    std::map<uint64_t, double> next_rate;
    double sum = 0.0;
    for (const XferThreadStat& s : threads) {
        std::deque<std::pair<double, long long>> h;
        auto found = hist.find(s.id);
        if (found != hist.end()) h = std::move(found->second);
        h.emplace_back(now, s.bytes);
        while (h.size() > 2 && now - h[1].first >= window) h.pop_front();
        double span = h.back().first - h.front().first;
        double r = span >= 0.25 ? (double)(h.back().second - h.front().second) / span : 0.0;
        if (r < 0.0) r = 0.0;
        // An idle thread is moving nothing, whatever it moved a second ago.
        if (s.file.empty()) r = 0.0;
        next_rate[s.id] = r;
        sum += r;
        next[s.id] = std::move(h);
    }
    hist = std::move(next);
    rate = std::move(next_rate);
    return sum;
}

void xfer_shutdown() {
    std::vector<std::thread> threads;
    std::vector<XferItem> dropped;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
        for (std::unique_ptr<Pool>& p : g_pools) {
            for (XferItem& it : p->q) dropped.push_back(it);
            p->q.clear();
        }
    }
    g_cv.notify_all();
    for (const XferItem& it : dropped) finish(it, false, "aborted");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (std::unique_ptr<Pool>& p : g_pools)
            for (std::unique_ptr<Worker>& w : p->workers) threads.push_back(std::move(w->th));
    }
    join_all(threads);
    std::lock_guard<std::mutex> lk(g_mu);
    g_pools.clear();
    g_stop = false;
}
