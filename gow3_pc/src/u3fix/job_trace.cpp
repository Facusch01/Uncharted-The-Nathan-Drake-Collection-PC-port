// Uncharted 3 1.02, 20 Nov 2026: a history of every job counter, to find why Syria co-op's jobs never finish.
//
// A job counter is made with the number of jobs that will lower it (image 0xd96620; the count is written with an xchg at
// 0xd96716) and each finished job lowers it by one (0xda80bc, a locked xadd of -1, right before the waiters of the new
// value are woken). Both places now also note the counter in a table: who made it and with how many jobs, how many
// jobs have finished since, and the value before the last one. The counter wait reports (counter_wait_log.cpp) print
// that history for a counter that does not reach its target, which tells apart "its jobs never finished" from "they
// all finished and the waiter was not woken". Nothing in the game changes. Found by their bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace u3fix {

namespace {

struct CounterStory {
    const void *counter;
    uint64_t creator;     // guest offset the counter was made from
    int64_t initial;      // jobs it was made for
    int64_t finished;     // jobs that lowered it since
    int64_t last_before;  // its value before the last job lowered it
    uint64_t made_ms, last_ms;
    // the last four jobs that lowered it (the job system's current job and its list at the lowering), and the same
    // for the counter's previous life (before it was made again from the pool)
    uint64_t jobs[4][2];
    uint64_t prev_jobs[4][2];
    int64_t prev_finished, prev_initial;
    bool freed;          // given back to the pool since it was made
    uint64_t freer;      // guest offset of the code that gave it back
    uint64_t prev_creator;
};

std::atomic<unsigned> g_alarms{0};

uint64_t guest(uint64_t a);

// A counter made again without being freed, or lowered after it was freed, has two owners: report it.
void alarm(const char *what, const void *counter, const CounterStory &s, uint64_t other) {
    const unsigned n = ++g_alarms;
    if (n > 40 && n % 100) return;
    std::printf("Uncharted 3: job counter %p %s: made at guest %#llx for %lld jobs, %lld finished, %s %#llx; "
                "the life before: made at guest %#llx for %lld jobs, %lld finished (time %u)\n",
                counter, what, static_cast<unsigned long long>(s.creator), static_cast<long long>(s.initial),
                static_cast<long long>(s.finished), s.freed ? "freed at guest" : "now by guest",
                static_cast<unsigned long long>(s.freed ? s.freer : other),
                static_cast<unsigned long long>(s.prev_creator), static_cast<long long>(s.prev_initial),
                static_cast<long long>(s.prev_finished), n);
    std::fflush(stdout);
}

constexpr unsigned kSlots = 1u << 16;
CounterStory g_stories[kSlots];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
const unsigned char *g_image = nullptr;
uint64_t g_size = 0;

uint64_t guest(uint64_t a) {
    const uint64_t base = reinterpret_cast<uint64_t>(g_image);
    return a >= base && a < base + g_size ? a - base : a;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

unsigned slot_of(const void *counter) {
    uint64_t h = reinterpret_cast<uint64_t>(counter) >> 6;   // counters are 64-byte aligned pool entries
    h ^= h >> 17;
    h *= 0x9e3779b97f4a7c15ull;
    return static_cast<unsigned>(h >> 48) & (kSlots - 1);
}

void lock() { while (g_lock.test_and_set(std::memory_order_acquire)) {} }
void unlock() { g_lock.clear(std::memory_order_release); }

// the slot of this counter, or a free one (linear probing over a few slots, then the home slot is reused)
CounterStory &find(const void *counter) {
    const unsigned home = slot_of(counter);
    for (unsigned i = 0; i < 8; i++) {
        CounterStory &s = g_stories[(home + i) & (kSlots - 1)];
        if (s.counter == counter || !s.counter) return s;
    }
    return g_stories[home];
}

__attribute__((sysv_abi)) void counter_made(const void *counter, int64_t initial, uint64_t ret) {
    lock();
    CounterStory &s = find(counter);
    CounterStory fresh{};
    fresh.counter = counter;
    fresh.creator = guest(ret);
    fresh.initial = initial;
    fresh.made_ms = now_ms();
    if (s.counter == counter) {   // the pool hands this counter out again: keep what its last life saw
        if (!s.freed && s.initial != LLONG_MIN) alarm("made again while still in use", counter, s, fresh.creator);
        std::memcpy(fresh.prev_jobs, s.jobs, sizeof s.jobs);
        fresh.prev_finished = s.finished;
        fresh.prev_initial = s.initial;
        fresh.prev_creator = s.creator;
    }
    s = fresh;
    unlock();
}

__attribute__((sysv_abi)) void counter_freed(const void *counter, uint64_t ret) {
    lock();
    CounterStory &s = find(counter);
    if (s.counter == counter) {
        s.freed = true;
        s.freer = guest(ret);
    }
    unlock();
}

__attribute__((sysv_abi)) void counter_lowered(const void *counter, int64_t before, uint64_t job, uint64_t list) {
    lock();
    CounterStory &s = find(counter);
    if (s.counter != counter) {   // made before the trace saw it
        s = CounterStory{};
        s.counter = counter;
        s.initial = LLONG_MIN;
    }
    // only for counters whose making the trace saw (others are made by code it does not watch and look freed)
    if (s.freed && s.initial != LLONG_MIN && s.creator) alarm("lowered by a job after it was freed", counter, s, 0);
    std::memmove(s.jobs[1], s.jobs[0], sizeof(s.jobs) - sizeof(s.jobs[0]));
    s.jobs[0][0] = job;
    s.jobs[0][1] = list;
    s.finished++;
    s.last_before = before;
    s.last_ms = now_ms();
    unlock();
}

// mov [rbx+0x18],rdx (rdtsc); xchg r13,[rbx+0x20]; mov [rbp-0x38],r13; mov rax,[rbp-0x38]
constexpr unsigned char kMake[] = {0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x89,
                                   0x53, 0x18, 0x4c, 0x87, 0x6b, 0x20, 0x4c, 0x89, 0x6d, 0xc8, 0x48,
                                   0x8b, 0x45, 0xc8, 0x49, 0x8b, 0x3e};
constexpr unsigned kMakeAt = 13, kMakeLength = 8;
// mov rax,-1; mov rdi,[rbp-0x78]; lock xadd [rdi+0x20],rax; mov [rbp-0x40],rax; mov rsi,[rbp-0x40]; dec rsi; call wake
constexpr unsigned char kLower[] = {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x7d, 0x88, 0xf0,
                                    0x48, 0x0f, 0xc1, 0x47, 0x20, 0x48, 0x89, 0x45, 0xc0, 0x48, 0x8b, 0x75,
                                    0xc0, 0x48, 0xff, 0xce, 0xe8};
constexpr unsigned kLowerAt = 11, kLowerLength = 10;

struct Writer {
    unsigned char *p;
    unsigned n = 0;
    void put(std::initializer_list<unsigned char> b) { for (unsigned char c : b) p[n++] = c; }
    void imm64(uint64_t v) { std::memcpy(p + n, &v, 8); n += 8; }
    void save() { put({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53}); }
    void restore() { put({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58}); }
    // push rbp; mov rbp,rsp; and rsp,-16; mov rax,h; call rax; mov rsp,rbp; pop rbp
    void call(uint64_t h) {
        put({0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0xb8});
        imm64(h);
        put({0xff, 0xd0, 0x48, 0x89, 0xec, 0x5d});
    }
    void jump(uint64_t to) { put({0xff, 0x25, 0, 0, 0, 0}); imm64(to); }
};

}  // namespace

// the history of one counter as text (empty when the trace has not seen it)
void counter_story(const void *counter, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    lock();
    const CounterStory s = find(counter);
    unlock();
    if (s.counter != counter) return;
    const uint64_t now = now_ms();
    int used;
    if (s.initial == LLONG_MIN)
        used = std::snprintf(out, size, "made before the trace; %lld jobs finished on it, the last %llu ms ago (value %lld before it)",
                             static_cast<long long>(s.finished), static_cast<unsigned long long>(now - s.last_ms),
                             static_cast<long long>(s.last_before));
    else
        used = std::snprintf(out, size, "made %llu ms ago at guest %#llx for %lld jobs; %lld finished, the last %s%llu%s (value %lld before it)",
                             static_cast<unsigned long long>(now - s.made_ms), static_cast<unsigned long long>(s.creator),
                             static_cast<long long>(s.initial), static_cast<long long>(s.finished),
                             s.finished ? "" : "(none) ", static_cast<unsigned long long>(s.finished ? now - s.last_ms : 0),
                             s.finished ? " ms ago" : "", static_cast<long long>(s.last_before));
    auto list = [&](const char *what, const uint64_t (&jobs)[4][2]) {
        if (used < 0 || static_cast<size_t>(used) >= size) return;
        used += std::snprintf(out + used, size - used, "; %s", what);
        for (int i = 0; i < 4 && jobs[i][0] | jobs[i][1]; i++)
            if (static_cast<size_t>(used) < size)
                used += std::snprintf(out + used, size - used, " %#llx/%#llx", static_cast<unsigned long long>(jobs[i][0]),
                                      static_cast<unsigned long long>(jobs[i][1]));
    };
    list("lowered by jobs (latest first)", s.jobs);
    if (s.prev_initial || s.prev_finished) {
        if (used >= 0 && static_cast<size_t>(used) < size)
            used += std::snprintf(out + used, size - used, "; its previous life: %lld jobs, %lld finished",
                                  static_cast<long long>(s.prev_initial), static_cast<long long>(s.prev_finished));
        list("by", s.prev_jobs);
    }
}

bool install_job_trace(unsigned char *image, uint64_t size) {
    const uint64_t make = find_unique(image, size, kMake, sizeof(kMake));
    const uint64_t lower = find_unique(image, size, kLower, sizeof(kLower));
    if (make == kNotFound || lower == kNotFound) return false;
    g_image = image;
    g_size = size;
    unsigned char *page = stub_page_near(image, size);
    if (!page) return false;

    // counter made: note it (counter rbx, count r13, the maker's return address [rbp+8]), then the replaced code
    Writer a{page};
    a.save();
    a.put({0x48, 0x89, 0xdf});          // mov rdi,rbx
    a.put({0x4c, 0x89, 0xee});          // mov rsi,r13
    a.put({0x48, 0x8b, 0x55, 0x08});    // mov rdx,[rbp+8]
    a.call(reinterpret_cast<uint64_t>(&counter_made));
    a.restore();
    std::memcpy(page + a.n, image + make + kMakeAt, kMakeLength);   // xchg r13,[rbx+0x20]; mov [rbp-0x38],r13
    a.n += kMakeLength;
    a.jump(reinterpret_cast<uint64_t>(image + make + kMakeAt + kMakeLength));

    // counter lowered: the replaced code first (the xadd leaves the value before in rax), then note it
    Writer b{page + ((a.n + 15) & ~15u)};
    std::memcpy(b.p, image + lower + kLowerAt, kLowerLength);        // lock xadd [rdi+0x20],rax; mov [rbp-0x40],rax
    b.n = kLowerLength;
    b.save();
    b.put({0x48, 0x89, 0xc6});          // mov rsi,rax (rdi is the counter)
    b.put({0x48, 0x8b, 0x55, 0xa8});    // mov rdx,[rbp-0x58] (the job)
    b.put({0x48, 0x8b, 0x4d, 0x90});    // mov rcx,[rbp-0x70] (its list)
    b.call(reinterpret_cast<uint64_t>(&counter_lowered));
    b.restore();
    b.jump(reinterpret_cast<uint64_t>(image + lower + kLowerAt + kLowerLength));

    // counter freed (both places that give a counter back to the pool run the same code, the counter in r12):
    // shr rcx,6; and [r13+rcx*8+0xb00],rax (its bit in the pool's map); sfence
    static constexpr unsigned char kFree[] = {0x48, 0xc1, 0xe9, 0x06, 0x49, 0x21, 0x84, 0xcd,
                                              0x00, 0x0b, 0x00, 0x00, 0x0f, 0xae, 0xf8};
    constexpr unsigned kFreeAt = 4, kFreeLength = 8;
    uint64_t frees[4];
    unsigned nfrees = 0;
    for (uint64_t at = 0; at + sizeof(kFree) <= size && nfrees < 4; at++)
        if (image[at] == kFree[0] && std::memcmp(image + at, kFree, sizeof(kFree)) == 0) frees[nfrees++] = at;
    unsigned char *next = b.p + b.n;
    Writer f[4] = {};
    for (unsigned i = 0; i < nfrees; i++) {
        next = reinterpret_cast<unsigned char *>((reinterpret_cast<uintptr_t>(next) + 15) & ~uintptr_t{15});
        f[i].p = next;
        // noted first, then the bit is cleared: another thread can take the counter only after that
        f[i].save();
        f[i].put({0x4c, 0x89, 0xe7});          // mov rdi,r12 (the counter)
        f[i].put({0x48, 0x8b, 0x75, 0x08});    // mov rsi,[rbp+8] (who frees it)
        f[i].call(reinterpret_cast<uint64_t>(&counter_freed));
        f[i].restore();
        std::memcpy(f[i].p + f[i].n, image + frees[i] + kFreeAt, kFreeLength);   // the and
        f[i].n += kFreeLength;
        f[i].jump(reinterpret_cast<uint64_t>(image + frees[i] + kFreeAt + kFreeLength));
        next = f[i].p + f[i].n;
    }

    if (!finish_stub(page)) return false;
    if (!jump_to(image, make + kMakeAt, kMakeLength, a.p) || !jump_to(image, lower + kLowerAt, kLowerLength, b.p))
        return false;
    for (unsigned i = 0; i < nfrees; i++)
        if (!jump_to(image, frees[i] + kFreeAt, kFreeLength, f[i].p)) return false;
    std::printf("Uncharted 3: job counter frees traced at %u places\n", nfrees);
    std::printf("Uncharted 3: job counter history kept (made %#llx, lowered %#llx)\n",
                static_cast<unsigned long long>(make + kMakeAt), static_cast<unsigned long long>(lower + kLowerAt));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
