// Uncharted 3 1.02, 20 Nov 2026: a job wait that can never end is ended, so the game goes on instead of freezing.
//
// In Syria and Yemen co-op the game froze now and then (a kill, a roll and a weapon switch at once in the busy maps):
// the frame stopped with jobs parked on job counters that were short of their target while no job submitted on them
// was left to finish, every priority queue was empty and every worker idle. Nothing would ever lower those counters.
// settle_negative (counter_wait_log.cpp) already ends a wait on a counter that went below 0; this is its other side, a
// counter left above 0. To know that no job is left to lower a counter, the jobs are counted per counter: each job a
// list submits (the queue push with "append", image 0xda57d0; the job names the counter it lowers at +0x10) adds one,
// each job that finishes (the lowering in the worker loop, 0xda80bb) takes one away. A parked job's wait for 0 (or for
// at most 0 or more) is settled when it has lasted 4 s, its counter is above 0 with no counted job left on it, every
// queue is empty and the frame has stopped for 3 s (the particle renderer's game-jobs check, 0xeb9ca0, runs once a
// frame): the counter is set to 0 and the job system's counter update (0xd969c0: under the job lock, the jobs waiting
// for that value go back to the queues) is called, as a job finishing on it would have. The update runs on a game thread
// that sleeps in the thread wait loop (it holds no job system lock); the counter_wait_log checker only asks for it.
// Tested in a full Syria co-op run (0.10.118): one freeze began, the counter was settled, the game paused about 2 s and
// went on to the end. Found by their bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace u3fix {

namespace {

constexpr unsigned kSlots = 4096;   // the counter pool's counters (2048 in this build)
std::atomic<int32_t> g_pending[kSlots];   // jobs submitted and not yet finished, per counter
std::atomic<int64_t> g_last_frame_ms{-1};
unsigned char *const *g_manager = nullptr;

using Update = void(__attribute__((sysv_abi)) *)(unsigned char *, int64_t);
Update g_update = nullptr;

struct SettleRequest {
    unsigned char *counter;
    char chain[160];
};
std::mutex g_settle_lock;
SettleRequest g_settle[32];
unsigned g_settle_count = 0;
std::atomic<unsigned> g_settled{0};

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// the counter's place in the pool (the job manager +0xae0: counters at +0x120, count at +0x128, 0x80 bytes each)
int slot_of(const void *counter) {
    const unsigned char *manager = g_manager ? *g_manager : nullptr;
    if (!manager || !counter) return -1;
    const unsigned char *pool = manager + 0xae0;
    const uintptr_t base = *reinterpret_cast<const uintptr_t *>(pool + 0x120);
    const uint32_t capacity = *reinterpret_cast<const uint32_t *>(pool + 0x128);
    const uintptr_t c = reinterpret_cast<uintptr_t>(counter);
    if (c < base || (c - base) % 0x80) return -1;
    const uintptr_t i = (c - base) / 0x80;
    return i < capacity && i < kSlots ? static_cast<int>(i) : -1;
}

int pending_jobs(const void *counter) {
    const int slot = slot_of(counter);
    return slot < 0 ? -1 : g_pending[slot].load(std::memory_order_acquire);
}

bool queues_empty() {
    const unsigned char *manager = g_manager ? *g_manager : nullptr;
    if (!manager) return false;
    const unsigned char *queues = *reinterpret_cast<const unsigned char *const *>(manager + 0x4f10);
    if (!queues) return false;
    for (unsigned p = 0; p < 8; p++)   // 8 priorities, 0x2078 bytes each, the count first
        if (__atomic_load_n(reinterpret_cast<const uint64_t *>(queues + p * 0x2078), __ATOMIC_RELAXED)) return false;
    return true;
}

// the queue push: a list appended (esi 1) is one the game submits; its jobs are counted for their counters (a list
// pushed to the front, esi 0, holds jobs released from their waits: counted when their list was submitted).
// The list: +0x4 write index, +0x8 job count, +0x10 the jobs (0x40 bytes each), +0x18 capacity.
__attribute__((sysv_abi)) void on_push(const unsigned char *list, uint64_t append) {
    if (!list || !(append & 0xff)) return;
    const int32_t write = *reinterpret_cast<const int32_t *>(list + 0x4);
    const int32_t count = *reinterpret_cast<const int32_t *>(list + 0x8);
    const unsigned char *jobs = *reinterpret_cast<unsigned char *const *>(list + 0x10);
    const int32_t capacity = *reinterpret_cast<const int32_t *>(list + 0x18);
    if (!jobs || count <= 0 || capacity <= 0 || count > capacity) return;
    const int32_t first = ((write - count) % capacity + capacity) % capacity;
    for (int32_t i = 0; i < count; i++) {
        const unsigned char *job = jobs + 0x40 * static_cast<uint64_t>((first + i) % capacity);
        const int slot = slot_of(*reinterpret_cast<void *const *>(job + 0x10));
        if (slot >= 0) g_pending[slot].fetch_add(1, std::memory_order_acq_rel);
    }
}

// a job finished and lowers its counter
__attribute__((sysv_abi)) void on_finish(const void *counter) {
    const int slot = slot_of(counter);
    if (slot < 0) return;
    if (g_pending[slot].fetch_sub(1, std::memory_order_acq_rel) <= 0)
        g_pending[slot].store(0, std::memory_order_release);   // submitted before the count began
}

__attribute__((sysv_abi)) void on_frame() { g_last_frame_ms.store(steady_ms(), std::memory_order_relaxed); }

// the queue push: push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax; movsxd rax,[rdi+0x4c]; lea r14,[rip+..]
constexpr unsigned char kPush[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53,
                                   0x50, 0x48, 0x63, 0x47, 0x4c, 0x4c, 0x8d, 0x35};
// the lowering: mov rax,-1; mov rdi,[rbp-0x78]; lock xadd [rdi+0x20],rax; mov [rbp-0x40],rax; mov rsi,[rbp-0x40];
// dec rsi; call (the counter update)
constexpr unsigned char kLower[] = {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x7d, 0x88, 0xf0,
                                    0x48, 0x0f, 0xc1, 0x47, 0x20, 0x48, 0x89, 0x45, 0xc0, 0x48, 0x8b, 0x75,
                                    0xc0, 0x48, 0xff, 0xce, 0xe8};
constexpr unsigned kLowerAt = 11, kLowerLength = 10;   // lock xadd [rdi+0x20],rax; mov [rbp-0x40],rax
// the game-jobs check: push rbp; mov rbp,rsp; push r15..r12; push rbx; push rax; the first counter loads
constexpr unsigned char kGameCheck[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                        0x50, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x70, 0x20, 0x48, 0x8b, 0x47, 0x08, 0x4c,
                                        0x8b, 0x68, 0x20};
// the counter update: push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r14,rsi; mov r12,rdi;
// lea r15,[rip+..] (the job manager)
constexpr unsigned char kUpdate[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                     0x53, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xfc, 0x4c, 0x8d, 0x3d};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15

void put(unsigned char *stub, unsigned &n, const void *bytes, unsigned length) {
    std::memcpy(stub + n, bytes, length);
    n += length;
}

void put_call(unsigned char *stub, unsigned &n, uint64_t to) {
    stub[n++] = 0x48; stub[n++] = 0xb8; put(stub, n, &to, 8);   // mov rax, to
    stub[n++] = 0xff; stub[n++] = 0xd0;                         // call rax
}

void put_jmp(unsigned char *stub, unsigned &n, uint64_t to) {
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4;   // jmp [rip]
    put(stub, n, &to, 8);
}

// at a function entry: keep the argument registers, call the handler with them, replay the prologue, go on
bool hook_entry(unsigned char *image, uint64_t size, uint64_t at, uint64_t handler) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    // entry rsp = 8 mod 16: six pushes and sub rsp,8 make the call aligned
    static constexpr unsigned char kSave[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08};
    static constexpr unsigned char kRestore[] = {0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
    unsigned n = 0;
    put(stub, n, kSave, sizeof kSave);
    put_call(stub, n, handler);
    put(stub, n, kRestore, sizeof kRestore);
    put(stub, n, image + at, kPrologue);
    put_jmp(stub, n, reinterpret_cast<uint64_t>(image + at + kPrologue));
    return finish_stub(stub) && jump_to(image, at, kPrologue, stub);
}

// inside the worker loop: keep the caller-saved registers, align the stack, call on_finish(rdi), replay the lowering
bool hook_lower(unsigned char *image, uint64_t size, uint64_t at) {
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    static constexpr unsigned char kSave[] = {0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41,
                                              0x53,                     // rax rcx rdx rsi rdi r8..r11
                                              0x55, 0x48, 0x89, 0xe5,   // push rbp; mov rbp,rsp
                                              0x48, 0x83, 0xe4, 0xf0};  // and rsp,-16
    static constexpr unsigned char kRestore[] = {0x48, 0x89, 0xec, 0x5d,   // mov rsp,rbp; pop rbp
                                                 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a,
                                                 0x59, 0x58};
    unsigned n = 0;
    put(stub, n, kSave, sizeof kSave);
    put_call(stub, n, reinterpret_cast<uint64_t>(&on_finish));
    put(stub, n, kRestore, sizeof kRestore);
    put(stub, n, image + at, kLowerLength);
    put_jmp(stub, n, reinterpret_cast<uint64_t>(image + at + kLowerLength));
    return finish_stub(stub) && jump_to(image, at, kLowerLength, stub);
}

}  // namespace

bool consider_stuck_wait(const unsigned char *counter, int64_t target, int mode, int64_t value, int64_t waited_ms,
                         const char *chain) {
    if (!g_update || !counter) return false;
    const bool for_zero = (mode == 0 && target == 0) || (mode == 1 && target >= 0);
    const int64_t last_frame = g_last_frame_ms.load(std::memory_order_relaxed);
    if (!for_zero || value <= 0 || waited_ms < 4000 || last_frame < 0 || steady_ms() - last_frame < 3000) return false;
    if (pending_jobs(counter) != 0 || !queues_empty()) return false;
    std::lock_guard<std::mutex> guard(g_settle_lock);
    for (unsigned i = 0; i < g_settle_count; i++)
        if (g_settle[i].counter == counter) return true;
    if (g_settle_count >= 32) return false;
    SettleRequest &r = g_settle[g_settle_count++];
    r.counter = const_cast<unsigned char *>(counter);
    std::snprintf(r.chain, sizeof r.chain, "%s", chain ? chain : "");
    return true;
}

void settle_stuck_counters() {
    if (!g_update) return;
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
        if (seen <= 0 || pending_jobs(counter) > 0) continue;   // it moved on after all
        if (!__atomic_compare_exchange_n(value, &seen, int64_t{0}, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        g_update(counter, 0);
        const unsigned t = ++g_settled;
        if (t <= 50 || t % 100 == 0) {
            std::printf("Uncharted 3: job counter %p stuck at %lld with no job left to lower it, every queue empty and "
                        "the frame stopped: set to 0 and its waiting jobs released so the game goes on (time %u); "
                        "waiting code%s\n",
                        static_cast<void *>(counter), static_cast<long long>(seen), t, todo[i].chain);
            std::fflush(stdout);
        }
    }
}

bool install_stuck_counter(unsigned char *image, uint64_t size) {
    const uint64_t update = find_unique(image, size, kUpdate, sizeof kUpdate);
    const uint64_t push = find_unique(image, size, kPush, sizeof kPush);
    const uint64_t lower = find_unique(image, size, kLower, sizeof kLower);
    const uint64_t check = find_unique(image, size, kGameCheck, sizeof kGameCheck);
    if (update == kNotFound || push == kNotFound || lower == kNotFound || check == kNotFound) return false;
    const unsigned char *lea = image + update + sizeof kUpdate - 3;   // lea r15,[rip+disp32]: the job manager
    int32_t disp;
    std::memcpy(&disp, lea + 3, 4);
    g_manager = reinterpret_cast<unsigned char *const *>(lea + 7 + disp);
    // count the jobs before anything is settled: the push first, then the lowering
    if (!hook_entry(image, size, push, reinterpret_cast<uint64_t>(&on_push))) return false;
    if (!hook_lower(image, size, lower + kLowerAt)) return false;
    if (!hook_entry(image, size, check, reinterpret_cast<uint64_t>(&on_frame))) return false;
    g_update = reinterpret_cast<Update>(image + update);
    std::printf("Uncharted 3: a job wait that can't end (no job left to lower its counter, queues empty, frame stopped) "
                "is settled after 4 s (push %#llx, lowering %#llx, frame check %#llx, counter update %#llx)\n",
                static_cast<unsigned long long>(push), static_cast<unsigned long long>(lower + kLowerAt),
                static_cast<unsigned long long>(check), static_cast<unsigned long long>(update));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
