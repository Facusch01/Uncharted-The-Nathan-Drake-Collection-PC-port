// Uncharted 3 1.02, 20 Nov 2026: a job counter is not handed out again while jobs that lower it have yet to finish.
//
// The job system keeps its job counters in a pool (the job manager +0xae0: lock, bitmap of used counters at +0x20,
// counters at +0x120, count at +0x128; 0x80 bytes each, the count of jobs still to finish at +0x20). Each job names the
// counter it lowers when it finishes (job +0x10). Two functions give a counter back: one at once (image 0xd967c0), one
// after it waited for the count to reach 0 (0xd975b0). Neither looks for jobs still to finish. On PS4 there are none by
// then. On a PC there often are (job_trace: millions of times in a co-op game a job finished on a counter given back
// before, and the pool had handed it to a new owner). The late job lowers the new owner's count: its wait ends before
// its own jobs are done, it gives the counter back with its count at 0 while its own jobs still run, and so on, until
// a count skips the value a wait looks for and that wait never ends: the game freezes (Syria and Yemen co-op).
// The count itself can't tell (it is 0 while jobs still run), so the jobs are counted: each job a list submits (the
// queue push with "append", image 0xda57d0) adds one for its counter, each job that finishes (job_trace's hook on the
// lowering) takes one away. A counter given back with jobs still to finish stays out of the pool until they have, at
// most 2 s (it goes back then, as before); the counters kept are given back on later calls. When the pool is nearly
// full nothing is kept. The wait-then-give-back function becomes: the wait, then the give-back function.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace u3fix {

namespace {

constexpr unsigned kSlots = 4096;   // the pool's counters (2048 in this build)
constexpr unsigned kMaxKept = 1024;
constexpr uint64_t kKeepMs = 2000;

struct Kept {
    unsigned char *counter;
    uint64_t since_ms;
};

std::mutex g_lock;
Kept g_kept[kMaxKept];
unsigned g_kept_count = 0;
thread_local bool t_releasing = false;
std::atomic<int32_t> g_pending[kSlots];   // jobs submitted and not yet finished, per counter

using GiveBack = void(__attribute__((sysv_abi)) *)(unsigned char *);
GiveBack g_give_back = nullptr;   // the original function: the replayed prologue, then the rest of it
unsigned char *const *g_manager = nullptr;

std::atomic<uint64_t> g_kept_total{0}, g_done_total{0}, g_timeout_total{0}, g_full_total{0}, g_jobs_counted{0};
uint64_t g_last_report_ms = 0;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

const unsigned char *pool() {
    const unsigned char *manager = g_manager ? *g_manager : nullptr;
    return manager ? manager + 0xae0 : nullptr;
}

// the counter's place in the pool, or -1 when it is not one of the pool's counters
int slot_of(const void *counter) {
    const unsigned char *p = pool();
    if (!p || !counter) return -1;
    const uintptr_t base = *reinterpret_cast<const uintptr_t *>(p + 0x120);
    const uint32_t capacity = *reinterpret_cast<const uint32_t *>(p + 0x128);
    const uintptr_t c = reinterpret_cast<uintptr_t>(counter);
    if (c < base || (c - base) % 0x80) return -1;
    const uintptr_t i = (c - base) / 0x80;
    return i < capacity && i < kSlots ? static_cast<int>(i) : -1;
}

void pool_use(unsigned &used, unsigned &capacity) {
    used = capacity = 0;
    const unsigned char *p = pool();
    if (!p) return;
    capacity = *reinterpret_cast<const uint32_t *>(p + 0x128);
    for (unsigned i = 0; i < 32; i++)
        used += static_cast<unsigned>(__builtin_popcountll(__atomic_load_n(
            reinterpret_cast<const uint64_t *>(p + 0x20) + i, __ATOMIC_RELAXED)));
}

int32_t pending_of(int slot) { return slot < 0 ? 0 : g_pending[slot].load(std::memory_order_acquire); }

// the queue push: a list appended (esi 1) is one the game submits; its jobs are counted for their counters (a list
// pushed to the front, esi 0, holds jobs released from their waits: counted when their list was submitted)
__attribute__((sysv_abi)) void on_push(const unsigned char *list, uint64_t append) {
    if (!list || !(append & 0xff)) return;
    const int32_t write = *reinterpret_cast<const int32_t *>(list + 0x4);
    const int32_t count = *reinterpret_cast<const int32_t *>(list + 0x8);
    const unsigned char *items = *reinterpret_cast<unsigned char *const *>(list + 0x10);
    const int32_t capacity = *reinterpret_cast<const int32_t *>(list + 0x18);
    if (!items || count <= 0 || capacity <= 0 || count > capacity) return;
    const int32_t first = ((write - count) % capacity + capacity) % capacity;
    for (int32_t i = 0; i < count; i++) {
        const unsigned char *item = items + 0x40 * static_cast<uint64_t>((first + i) % capacity);
        const int slot = slot_of(*reinterpret_cast<void *const *>(item + 0x10));
        if (slot < 0) continue;
        g_pending[slot].fetch_add(1, std::memory_order_acq_rel);
        g_jobs_counted.fetch_add(1, std::memory_order_relaxed);
    }
}

// 1: give the counter back now (the original function goes on); 0: it is kept for later
__attribute__((sysv_abi)) uint64_t on_give_back(unsigned char *counter) {
    if (t_releasing || !counter) return 1;
    const uint64_t now = now_ms();
    unsigned used = 0, capacity = 0;
    pool_use(used, capacity);
    const bool nearly_full = capacity && used + 128 >= capacity;
    const int slot = slot_of(counter);
    bool keep = pending_of(slot) > 0;

    unsigned char *ready[64];
    unsigned ready_count = 0;
    bool report = false;
    unsigned kept_now = 0;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (unsigned i = 0; i < g_kept_count && ready_count < 64;) {
            const int s = slot_of(g_kept[i].counter);
            const bool done = pending_of(s) <= 0;
            const bool late = now - g_kept[i].since_ms >= kKeepMs;
            if (!done && !late && !nearly_full) { i++; continue; }
            if (done) g_done_total++;
            else {
                g_timeout_total++;
                if (s >= 0) g_pending[s].store(0, std::memory_order_release);   // its jobs are not coming
            }
            ready[ready_count++] = g_kept[i].counter;
            g_kept[i] = g_kept[--g_kept_count];
        }
        if (keep && (nearly_full || g_kept_count >= kMaxKept)) {
            keep = false;
            g_full_total++;
        }
        if (keep) {
            g_kept[g_kept_count++] = {counter, now};
            g_kept_total++;
        }
        kept_now = g_kept_count;
        if (now - g_last_report_ms >= 30000) {
            g_last_report_ms = now;
            report = true;
        }
    }
    if (!keep && slot >= 0) g_pending[slot].store(0, std::memory_order_release);
    t_releasing = true;
    for (unsigned i = 0; i < ready_count; i++) g_give_back(ready[i]);
    t_releasing = false;

    if (report) {
        std::printf("Uncharted 3: job counters given back with jobs still to finish: %llu kept (%llu after their jobs, "
                    "%llu after 2 s, %llu not kept: pool nearly full), %u kept now; %llu jobs counted; pool %u of %u "
                    "in use\n",
                    static_cast<unsigned long long>(g_kept_total.load()),
                    static_cast<unsigned long long>(g_done_total.load()),
                    static_cast<unsigned long long>(g_timeout_total.load()),
                    static_cast<unsigned long long>(g_full_total.load()), kept_now,
                    static_cast<unsigned long long>(g_jobs_counted.load()), used, capacity);
        std::fflush(stdout);
    }
    return keep ? 0 : 1;
}

// in both give-back functions: mov r13,[r14]; lea rbx,[r13+0xae0]; mov rax,[r13+0xc00] (the pool, its counters)
constexpr unsigned char kPoolLoads[] = {0x4d, 0x8b, 0x2e, 0x49, 0x8d, 0x9d, 0xe0, 0x0a, 0x00, 0x00,
                                        0x49, 0x8b, 0x85, 0x00, 0x0c, 0x00, 0x00};
constexpr uint64_t kGiveBackLoadsAt = 0xb6;   // from the entry of the give-back function
constexpr uint64_t kWaitFreeLoadsAt = 0xbb;   // from the entry of the wait-then-give-back function
// both entries: push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x18; mov r12,rdi
constexpr unsigned char kEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xfc};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15
// the give-back function then: lea rax,[rip+..]; mov rax,[rax]; mov [rbp-0x30],rax; lea r14,[rip+..] (the manager)
constexpr uint64_t kManagerLeaAt = 0x22;
// the wait-then-give-back function then: lea rax,[rip+..]; mov rax,[rax]; mov [rbp-0x30],rax; call the wait
constexpr uint64_t kWaitCallAt = 0x22;
// the queue push: push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax; movsxd rax,[rdi+0x4c]; lea r14,[rip+..]
constexpr unsigned char kPush[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53,
                                   0x50, 0x48, 0x63, 0x47, 0x4c, 0x4c, 0x8d, 0x35};

// entry rsp = 8 mod 16: six pushes and sub rsp,8 make the call aligned
constexpr unsigned char kSave[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,   // rdi rsi rdx rcx r8 r9
                                   0x48, 0x83, 0xec, 0x08};                          // sub rsp,8
constexpr unsigned char kRestore[] = {0x48, 0x83, 0xc4, 0x08,                        // add rsp,8
                                      0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};

void put(unsigned char *stub, unsigned &n, const void *bytes, unsigned length) {
    std::memcpy(stub + n, bytes, length);
    n += length;
}

void put_mov_rax(unsigned char *stub, unsigned &n, uint64_t value) {
    stub[n++] = 0x48; stub[n++] = 0xb8;
    put(stub, n, &value, 8);
}

void put_jmp_back(unsigned char *stub, unsigned &n, uint64_t to) {
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4;   // jmp [rip]
    put(stub, n, &to, 8);
}

bool hook_give_back(unsigned char *image, uint64_t size, uint64_t at) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    put(stub, n, kSave, sizeof kSave);
    put_mov_rax(stub, n, reinterpret_cast<uint64_t>(&on_give_back));
    stub[n++] = 0xff; stub[n++] = 0xd0;                                               // call rax
    put(stub, n, kRestore, sizeof kRestore);
    stub[n++] = 0x84; stub[n++] = 0xc0;                                               // test al,al
    stub[n++] = 0x75; stub[n++] = 0x01;                                               // jnz the original
    stub[n++] = 0xc3;                                                                 // ret: kept
    unsigned char *original = stub + n;
    put(stub, n, image + at, kPrologue);                                              // the prologue
    put_jmp_back(stub, n, reinterpret_cast<uint64_t>(image + at + kPrologue));
    if (!finish_stub(stub)) return false;
    g_give_back = reinterpret_cast<GiveBack>(original);
    return jump_to(image, at, kPrologue, stub);
}

// the wait-then-give-back function: the wait (its arguments as they came), then the give-back function (hooked)
bool replace_wait_free(unsigned char *image, uint64_t size, uint64_t at, uint64_t give_back) {
    const unsigned char *call = image + at + kWaitCallAt;
    if (call[0] != 0xe8) return false;
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    const uint64_t wait = reinterpret_cast<uint64_t>(call + 5 + rel);
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    stub[n++] = 0x57;                                                                 // push rdi: aligned
    put_mov_rax(stub, n, wait);
    stub[n++] = 0xff; stub[n++] = 0xd0;                                               // call rax
    stub[n++] = 0x5f;                                                                 // pop rdi
    put_mov_rax(stub, n, reinterpret_cast<uint64_t>(image + give_back));
    stub[n++] = 0xff; stub[n++] = 0xe0;                                               // jmp rax
    if (!finish_stub(stub)) return false;
    return jump_to(image, at, kPrologue, stub);
}

bool hook_push(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kPush, sizeof kPush);
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    put(stub, n, kSave, sizeof kSave);
    put_mov_rax(stub, n, reinterpret_cast<uint64_t>(&on_push));
    stub[n++] = 0xff; stub[n++] = 0xd0;                                               // call rax
    put(stub, n, kRestore, sizeof kRestore);
    put(stub, n, image + at, kPrologue);                                              // the prologue
    put_jmp_back(stub, n, reinterpret_cast<uint64_t>(image + at + kPrologue));
    if (!finish_stub(stub)) return false;
    return jump_to(image, at, kPrologue, stub);
}

}  // namespace

void counter_job_finished(const void *counter) {
    const int slot = slot_of(counter);
    if (slot < 0) return;
    if (g_pending[slot].fetch_sub(1, std::memory_order_acq_rel) <= 0)
        g_pending[slot].store(0, std::memory_order_release);   // a job counted before the hooks were there
}

// ---- the safety net for a wait that can't end ----
// A freeze (Syria and Yemen co-op) leaves jobs parked on counters that are short of 0 although no job submitted on
// them is left to finish, every queue is empty and the frame has stopped: nothing will ever lower them. The checker in
// counter_wait_log.cpp asks for such a counter to be settled; a game thread that waits in the thread loop (it holds no
// job system lock) sets it to 0 and calls the job system's counter update (image 0xd969c0: under the job lock, the
// jobs waiting for that value go back to the queues), as a job finishing on it would have.

using Notify = void(__attribute__((sysv_abi)) *)(unsigned char *, int64_t);
Notify g_notify = nullptr;

struct SettleRequest {
    unsigned char *counter;
    char chain[160];
};
std::mutex g_settle_lock;
SettleRequest g_settle[32];
unsigned g_settle_count = 0;
std::atomic<unsigned> g_settled_total{0};

// the counter update: push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r14,rsi; mov r12,rdi;
// lea r15,[rip+..] (the job manager)
constexpr unsigned char kNotify[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                     0x53, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xfc, 0x4c, 0x8d, 0x3d};

bool job_queues_empty() {
    const unsigned char *manager = g_manager ? *g_manager : nullptr;
    if (!manager) return false;
    const unsigned char *queues = *reinterpret_cast<const unsigned char *const *>(manager + 0x4f10);
    if (!queues) return false;
    for (unsigned p = 0; p < 8; p++)
        if (__atomic_load_n(reinterpret_cast<const uint64_t *>(queues + p * 0x2078), __ATOMIC_RELAXED)) return false;
    return true;
}

void request_counter_settle(const unsigned char *counter, const char *chain) {
    if (!g_notify || !counter) return;
    std::lock_guard<std::mutex> guard(g_settle_lock);
    for (unsigned i = 0; i < g_settle_count; i++)
        if (g_settle[i].counter == counter) return;
    if (g_settle_count >= 32) return;
    SettleRequest &r = g_settle[g_settle_count++];
    r.counter = const_cast<unsigned char *>(counter);
    std::snprintf(r.chain, sizeof r.chain, "%s", chain ? chain : "");
}

void settle_requested_counters() {
    if (!g_notify) return;
    SettleRequest todo[32];
    unsigned n = 0;
    {
        std::lock_guard<std::mutex> guard(g_settle_lock);
        if (!g_settle_count) return;
        n = g_settle_count;
        std::memcpy(todo, g_settle, sizeof(SettleRequest) * n);
        g_settle_count = 0;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned char *counter = todo[i].counter;
        int64_t *value = reinterpret_cast<int64_t *>(counter + 0x20);
        int64_t seen = __atomic_load_n(value, __ATOMIC_ACQUIRE);
        if (seen <= 0 || counter_pending_jobs(counter) > 0) continue;   // it moved on after all
        if (!__atomic_compare_exchange_n(value, &seen, int64_t{0}, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        g_notify(counter, 0);
        const unsigned t = ++g_settled_total;
        if (t <= 50 || t % 100 == 0) {
            char story[768];
            counter_story(counter, story, sizeof story);
            std::printf("Uncharted 3: job counter %p stuck at %lld with no job left to lower it, every queue empty and "
                        "the frame stopped; set to 0 and its waiting jobs released so the game goes on (time %u); "
                        "waiting code%s; history: %s\n",
                        static_cast<void *>(counter), static_cast<long long>(seen), t, todo[i].chain, story);
            std::fflush(stdout);
        }
    }
}

int counter_pending_jobs(const void *counter) {
    const int slot = slot_of(counter);
    return slot < 0 ? -1 : g_pending[slot].load(std::memory_order_acquire);
}

bool install_counter_release(unsigned char *image, uint64_t size) {
    uint64_t give_back = kNotFound, wait_free = kNotFound;
    for (uint64_t i = kWaitFreeLoadsAt; i + sizeof kPoolLoads <= size; i++) {
        if (image[i] != kPoolLoads[0] || std::memcmp(image + i, kPoolLoads, sizeof kPoolLoads) != 0) continue;
        if (std::memcmp(image + i - kGiveBackLoadsAt, kEntry, sizeof kEntry) == 0) {
            if (give_back != kNotFound) return false;   // more than one: not the build this was written for
            give_back = i - kGiveBackLoadsAt;
        } else if (std::memcmp(image + i - kWaitFreeLoadsAt, kEntry, sizeof kEntry) == 0) {
            if (wait_free != kNotFound) return false;
            wait_free = i - kWaitFreeLoadsAt;
        }
    }
    if (give_back == kNotFound || wait_free == kNotFound) return false;
    const unsigned char *lea = image + give_back + kManagerLeaAt;
    if (lea[0] != 0x4c || lea[1] != 0x8d || lea[2] != 0x35) return false;   // lea r14,[rip+disp32]
    int32_t disp;
    std::memcpy(&disp, lea + 3, 4);
    g_manager = reinterpret_cast<unsigned char *const *>(lea + 7 + disp);

    const uint64_t notify = find_unique(image, size, kNotify, sizeof kNotify);
    if (notify != kNotFound) g_notify = reinterpret_cast<Notify>(image + notify);
    // the push first: once counters are kept, every job must be counted
    if (!hook_push(image, size)) return false;
    if (!hook_give_back(image, size, give_back)) return false;
    if (!replace_wait_free(image, size, wait_free, give_back)) return false;
    std::printf("Uncharted 3: job counters stay out of the pool until their jobs have finished (give back %#llx, wait "
                "and give back %#llx)\n",
                static_cast<unsigned long long>(give_back), static_cast<unsigned long long>(wait_free));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
