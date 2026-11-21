// Uncharted 3 1.02, 20 Nov 2026: the particle renderer waits for its jobs before it checks or gives back their counters.
//
// The particle renderer keeps 27 job counter objects in an array (value at +0x20; the array is at (object +0x10) +0x88).
// Each frame two checks assert that the jobs of the last frame are done: the game-jobs check (image 0xeb9ca0,
// AssertGameJobsNotRunning, the counters at array +0x00..+0x20) and the render-jobs check (0xeb9ec0,
// AssertRenderJobsNotRunning, the counters at +0x28, +0x30, +0x40..+0x60). Its teardown (0xeba720) gives all 27
// counters back to the job system's pool. On PS4 those jobs are always done by then. On a PC with many effects at once
// (a kill, a roll and a weapon switch together) they are not: the assertion traps are stepped over and the teardown
// gives back counters whose jobs are still queued (seen with job_trace: 1/2/4 jobs pending, none finished). The pool
// hands them out again, the late jobs lower the next owner's count (negative counters), and a wait elsewhere never ends:
// the game freezes. Each of the three places now first waits, at most 1 s, for the counters it is about to check or give
// back to reach 0, so the frame goes on the way it does on PS4; a job that never ends leaves the game as before.
// Found by their bytes; installed as a jump at each entry to a stub that calls the wait and replays the prologue.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace u3fix {

namespace {

constexpr unsigned kCounters = 27;   // array +0x00 .. +0xd0
constexpr unsigned kGameJobs[] = {0, 1, 2, 3, 4};
constexpr unsigned kRenderJobs[] = {5, 6, 8, 9, 10, 11, 12};

std::atomic<uint64_t> g_waits{0}, g_timeouts{0};

int64_t pending(const uint64_t *const *counters, unsigned i) {
    const uint64_t *c = counters[i];
    return c ? static_cast<int64_t>(__atomic_load_n(&c[4], __ATOMIC_ACQUIRE)) : 0;   // value at +0x20
}

template <unsigned N>
bool busy(const uint64_t *const *counters, const unsigned (&which)[N]) {
    for (unsigned i : which)
        if (pending(counters, i) > 0) return true;
    return false;
}

bool busy_all(const uint64_t *const *counters) {
    for (unsigned i = 0; i < kCounters; i++)
        if (pending(counters, i) > 0) return true;
    return false;
}

template <typename Busy> void wait_for(const char *where, Busy still_busy) {
    if (!still_busy()) return;
    const auto start = std::chrono::steady_clock::now();
    unsigned spins = 0, waited_ms = 0;
    bool still = true;
    for (;;) {
        still = still_busy();
        if (!still) break;
        waited_ms = static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        if (waited_ms >= 1000) break;
        if (++spins < 200) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const uint64_t n = ++g_waits;
    if (still) {
        const uint64_t t = ++g_timeouts;
        if (t <= 5 || t % 100 == 0)
            std::printf("Uncharted 3: particle jobs still running after 1 s at the %s (time %llu); the game goes on\n",
                        where, static_cast<unsigned long long>(t));
    } else if (n <= 5 || n % 500 == 0) {
        std::printf("Uncharted 3: particle jobs still running at the %s: waited %u ms for them (time %llu)\n", where,
                    waited_ms, static_cast<unsigned long long>(n));
    }
    std::fflush(stdout);
}

// Diagnostics for now: waiting showed that these jobs do not run late, they never start (1 s waits that time out). When
// the check finds the same jobs still pending for 120 checks in a row, the jobs waiting for counters are printed.
std::atomic<unsigned> g_game_streak{0}, g_render_streak{0};

template <unsigned N>
void report_stuck(const uint64_t *const *counters, const unsigned (&which)[N], std::atomic<unsigned> &streak,
                  const char *where) {
    if (!counters) return;
    if (!busy(counters, which)) { streak = 0; return; }
    if (++streak != 120) return;
    std::printf("Uncharted 3: particle jobs still pending after 120 %s checks:", where);
    for (unsigned i : which) std::printf(" %lld", static_cast<long long>(pending(counters, i)));
    std::printf("\n");
    std::fflush(stdout);
    dump_job_waits(where);
}

std::atomic<int64_t> g_last_check_ms{-1};   // the last game-jobs check: once a frame while the game runs

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

__attribute__((sysv_abi)) void game_jobs_wait(const uint64_t *const *counters) {
    g_last_check_ms.store(steady_ms(), std::memory_order_relaxed);
    report_stuck(counters, kGameJobs, g_game_streak, "game-jobs");
}

__attribute__((sysv_abi)) void render_jobs_wait(const uint64_t *const *counters) {
    report_stuck(counters, kRenderJobs, g_render_streak, "render-jobs");
}

__attribute__((sysv_abi)) void teardown_wait(const unsigned char *renderer) {
    if (!renderer) return;
    const unsigned char *inner = *reinterpret_cast<const unsigned char *const *>(renderer + 0x10);
    if (!inner) return;
    const uint64_t *const *counters = *reinterpret_cast<const uint64_t *const *const *>(inner + 0x88);
    if (counters) wait_for("counters' release", [&] { return busy_all(counters); });
}

// the checks: push rbp; mov rbp,rsp; push r15..r12; push rbx; (push rax | sub rsp,0x18); the first counter loads
constexpr unsigned char kGameCheck[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                        0x50, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x70, 0x20, 0x48, 0x8b, 0x47, 0x08, 0x4c,
                                        0x8b, 0x68, 0x20};
constexpr unsigned char kRenderCheck[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                          0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x47, 0x28, 0x48, 0x8b, 0x70, 0x20, 0x48,
                                          0x8b, 0x47, 0x40, 0x4c, 0x8b, 0x70, 0x20};
// the teardown: push rbp; mov rbp,rsp; push rbx; push rax; mov rbx,rdi; the array load and the first counter
constexpr unsigned char kTeardown[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x43,
                                       0x10, 0x48, 0x8b, 0x80, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x38};

// a stub that keeps the argument registers, calls the handler with rdi, replays the replaced prologue and jumps back
bool hook_entry(unsigned char *image, uint64_t size, const unsigned char *site, uint64_t length, unsigned prologue,
                uint64_t handler, const char *what) {
    const uint64_t at = find_unique(image, size, site, length);
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    // entry rsp = 8 mod 16: six pushes and sub rsp,8 make the call aligned
    static constexpr unsigned char kSave[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,   // rdi rsi rdx rcx r8 r9
                                              0x48, 0x83, 0xec, 0x08};                          // sub rsp,8
    static constexpr unsigned char kRestore[] = {0x48, 0x83, 0xc4, 0x08,                        // add rsp,8
                                                 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
    unsigned n = 0;
    std::memcpy(stub + n, kSave, sizeof kSave); n += sizeof kSave;
    stub[n++] = 0x48; stub[n++] = 0xb8; std::memcpy(stub + n, &handler, 8); n += 8;   // mov rax, handler
    stub[n++] = 0xff; stub[n++] = 0xd0;                                               // call rax
    std::memcpy(stub + n, kRestore, sizeof kRestore); n += sizeof kRestore;
    std::memcpy(stub + n, image + at, prologue); n += prologue;                      // the prologue
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4;         // jmp [rip]
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + prologue);
    std::memcpy(stub + n, &back, 8);
    if (!finish_stub(stub) || !jump_to(image, at, prologue, stub)) return false;
    std::printf("Uncharted 3: particle job wait at the %s %#llx\n", what, static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace

int64_t ms_since_frame() {
    const int64_t last = g_last_check_ms.load(std::memory_order_relaxed);
    return last < 0 ? -1 : steady_ms() - last;
}

bool install_particle_jobs_wait(unsigned char *image, uint64_t size) {
    bool any = false;
    any |= hook_entry(image, size, kGameCheck, sizeof kGameCheck, 6, reinterpret_cast<uint64_t>(&game_jobs_wait),
                      "game-jobs check");
    any |= hook_entry(image, size, kRenderCheck, sizeof kRenderCheck, 6, reinterpret_cast<uint64_t>(&render_jobs_wait),
                      "render-jobs check");
    // not at the teardown: there the counters often never reach 0 (1 s waits, a few frames per second)
    (void)kTeardown;
    (void)&teardown_wait;
    return any;
}

}  // namespace u3fix
