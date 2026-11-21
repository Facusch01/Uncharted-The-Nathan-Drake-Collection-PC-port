// Uncharted 3 1.02, 20 Nov 2026: print the jobs that wait for a counter before they may start (diagnostics only).
//
// The job system keeps jobs that may only start after one or two counters reach a value in a table of 0x80-byte
// entries (job manager, the pointer at image 0x1808f00: entries at +0x4ed8, count at +0x4ee0). Each entry names up to
// two counters (+0x18, +0x20) with their target values (+0x28, +0x30) and how they compare (+0x38: bits 0-1 for the
// first, 2-3 for the second; 0 = exactly, 1 = at most). Every time a job lowers a counter, the job system compares the
// new value with the entries that name it and queues the jobs whose conditions are met (image 0xda5c20). A counter that
// changes in another way (set to 0 by the runtime, made again from the pool) is never compared, and the jobs that wait
// for it never start, while their own counter waits for them for ever. This prints those entries, with each counter's
// current value, when a wait does not end, so a stuck job can be told apart from a lost one. Nothing in the game changes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace u3fix {

namespace {

constexpr uint64_t kManager = 0x1808f00;
const unsigned char *g_image = nullptr;
std::atomic<unsigned> g_dumps{0};

bool readable(const void *p, size_t n) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!p || !VirtualQuery(p, &info, sizeof info) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
#else
    return p != nullptr && n > 0;
#endif
}

template <typename T> T read(const unsigned char *p) { T v; std::memcpy(&v, p, sizeof v); return v; }

int64_t counter_value(const unsigned char *counter) {
    return counter && readable(counter + 0x20, 8) ? read<int64_t>(counter + 0x20) : INT64_MIN;
}

}  // namespace

void dump_job_waits(const char *why) {
    if (!g_image) return;
    const unsigned n = ++g_dumps;
    if (n > 10) return;
    const unsigned char *manager = read<const unsigned char *>(g_image + kManager);
    if (!readable(manager + 0x4ed8, 16)) return;
    const unsigned char *entries = read<const unsigned char *>(manager + 0x4ed8);
    const uint64_t count = read<uint64_t>(manager + 0x4ee0);
    if (!count || count > 4096 || !readable(entries, count * 0x80)) {
        std::printf("Uncharted 3: jobs waiting for counters (%s): %llu\n", why, static_cast<unsigned long long>(count));
        std::fflush(stdout);
        return;
    }
    unsigned shown = 0, waiting = 0;
    std::printf("Uncharted 3: jobs waiting for counters (%s), table of %llu:\n", why,
                static_cast<unsigned long long>(count));
    for (uint64_t i = 0; i < count; i++) {
        const unsigned char *e = entries + i * 0x80;
        const unsigned char *c1 = read<const unsigned char *>(e + 0x18), *c2 = read<const unsigned char *>(e + 0x20);
        if (!c1 && !c2) continue;
        waiting++;
        if (shown >= 40) continue;
        shown++;
        const uint64_t flags = read<uint64_t>(e + 0x38);
        std::printf("Uncharted 3:   #%llu prio %d:", static_cast<unsigned long long>(i), read<int32_t>(e + 0x48));
        if (c1)
            std::printf(" counter %p value %lld, waits for %s %lld;", static_cast<const void *>(c1),
                        static_cast<long long>(counter_value(c1)), (flags & 3) == 1 ? "at most" : "exactly",
                        static_cast<long long>(read<int64_t>(e + 0x28)));
        if (c2)
            std::printf(" counter %p value %lld, waits for %s %lld;", static_cast<const void *>(c2),
                        static_cast<long long>(counter_value(c2)), ((flags >> 2) & 3) == 1 ? "at most" : "exactly",
                        static_cast<long long>(read<int64_t>(e + 0x30)));
        std::printf(" job %#llx %#llx\n", static_cast<unsigned long long>(read<uint64_t>(e + 0x60)),
                    static_cast<unsigned long long>(read<uint64_t>(e + 0x68)));
    }
    std::printf("Uncharted 3:   %u entries wait for a counter\n", waiting);
    std::fflush(stdout);
}

bool install_job_waits(unsigned char *image, uint64_t size) {
    if (!image || size <= kManager + 8) return false;
    g_image = image;
    return true;
}

}  // namespace u3fix
